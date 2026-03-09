"""RetroSurf rendering server.

Accepts TCP connections from DOS clients, manages headless Chromium
sessions via Playwright, and streams rendered page content as compressed
tile deltas.

Usage:
    python server.py [--port 8086] [--config config.json]
"""

import argparse
import asyncio
import struct
import time
import random

from playwright.async_api import async_playwright

from config import load_config
from session import BrowserSession
from palette import build_256_palette, build_16_palette, palette_to_rgb_bytes
from protocol import (
    HEADER_SIZE, HEADER_FORMAT,
    MSG_CLIENT_HELLO, MSG_MOUSE_EVENT, MSG_KEY_EVENT, MSG_SCROLL_EVENT,
    MSG_TEXT_INPUT, MSG_NAVIGATE, MSG_NAV_ACTION, MSG_ACK, MSG_KEEPALIVE,
    MSG_SERVER_HELLO, MSG_PALETTE, MSG_FRAME_FULL, MSG_FRAME_DELTA,
    MSG_INTERACTION_MAP, MSG_STATUS, MSG_KEEPALIVE_ACK,
    MSG_NATIVE_CONTENT, MSG_NATIVE_IMAGE, MSG_MODE_SWITCH, MSG_NATIVE_CLICK,
    MSG_GLYPH_CACHE,
    MSG_YT_START, MSG_YT_FRAME, MSG_YT_AUDIO, MSG_YT_EOF,
    MSG_YT_CONTROL, MSG_YT_ACK,
    YT_ACTION_PAUSE, YT_ACTION_RESUME, YT_ACTION_STOP,
    NAV_BACK, NAV_FORWARD, NAV_RELOAD, NAV_STOP, NAV_TOGGLE_MODE,
    MOUSE_MOVE, MOUSE_CLICK, MOUSE_RELEASE, MOUSE_DBLCLICK,
    FLAG_CONTINUED,
    decode_header, decode_client_hello, decode_mouse_event,
    decode_key_event, decode_scroll_event, decode_text_input,
    decode_navigate, decode_nav_action, decode_native_click,
    decode_yt_control, decode_yt_ack,
    encode_message, encode_server_hello, encode_palette,
    encode_frame_delta, encode_interaction_map, encode_status,
    encode_mode_switch, encode_yt_start, encode_yt_frame_chunk, encode_yt_audio,
    SequenceCounter, split_large_payload,
)


    # Number of tiles to compress and send per progressive batch.
    # Lower = faster time-to-first-tile, higher = less protocol overhead.
    # 40 tiles × ~140 bytes avg = ~5.6KB per batch → 16ms at 350KB/s.
PROGRESSIVE_BATCH_SIZE = 40


class RetroSurfServer:
    """Main server managing browser sessions and client connections."""

    def __init__(self, config):
        self.config = config
        self.playwright = None
        self.browser = None
        self.sessions = {}  # session_id -> BrowserSession
        self._next_session_id = 1

    async def start(self):
        """Launch the browser and start the TCP server."""
        print("Starting RetroSurf server...")

        # Launch real Chrome (headed) to avoid bot detection.
        # Uses your installed Chrome instead of Playwright's bundled Chromium.
        # A Chrome window will appear — minimize it, don't close it.
        print("Launching Chrome (headed)...")
        self.playwright = await async_playwright().start()
        try:
            self.browser = await self.playwright.chromium.launch(
                headless=False,
                channel='chrome',
                args=self.config.get('chromium_args', []),
            )
            print("Chrome (real) ready.")
        except Exception as e:
            print(f"Real Chrome not available ({e}), falling back to bundled Chromium...")
            self.browser = await self.playwright.chromium.launch(
                headless=False,
                args=self.config.get('chromium_args', []),
            )
            print("Chromium (headed) ready.")

        # Start session cleanup task
        asyncio.create_task(self._cleanup_sessions_loop())

        # Start TCP server
        port = self.config.get('port', 8086)
        server = await asyncio.start_server(
            self._handle_client,
            '0.0.0.0',
            port,
        )
        print(f"RetroSurf server listening on port {port}")
        print("Waiting for DOS client connection...")

        async with server:
            await server.serve_forever()

    async def _handle_client(self, reader, writer):
        """Handle a single DOS client connection."""
        addr = writer.get_extra_info('peername')
        print(f"\n[Server] Client connected from {addr}")

        seq = SequenceCounter()
        session = None

        try:
            # --- Handshake ---
            header_data = await asyncio.wait_for(
                reader.readexactly(HEADER_SIZE), timeout=10.0
            )
            header = decode_header(header_data)

            if header['msg_type'] != MSG_CLIENT_HELLO:
                print(f"[Server] Expected CLIENT_HELLO, got 0x{header['msg_type']:02X}")
                writer.close()
                return

            payload = await asyncio.wait_for(
                reader.readexactly(header['payload_len']), timeout=10.0
            )
            hello = decode_client_hello(payload)
            print(f"[Server] CLIENT_HELLO: {hello['screen_width']}x{hello['screen_height']} "
                  f"@ {hello['color_depth']}bpp, tile={hello['tile_size']}")

            # Get or create session
            session = await self._get_or_create_session(hello)
            print(f"[Server] Session {session.session_id} "
                  f"({'resumed' if hello.get('session_id') else 'new'})")

            # Configure viewport
            content_width = hello['screen_width']
            content_height = hello['screen_height'] - hello['chrome_height']
            await session.configure_viewport(
                content_width, content_height,
                hello['color_depth'], hello['tile_size'],
            )

            # Send SERVER_HELLO
            server_hello = encode_server_hello(
                protocol_version=1,
                session_id=session.session_id,
                content_width=content_width,
                content_height=content_height,
                tile_size=hello['tile_size'],
            )
            await self._send_message(writer, MSG_SERVER_HELLO, server_hello, seq)
            print(f"[Server] Sent SERVER_HELLO (session={session.session_id})")

            # Send palette
            if hello['color_depth'] == 8:
                palette = build_256_palette()
            else:
                palette = build_16_palette()
            palette_bytes = palette_to_rgb_bytes(palette)
            await self._send_message(writer, MSG_PALETTE, palette_bytes, seq)
            print(f"[Server] Sent PALETTE ({len(palette_bytes)} bytes)")

            # Navigate to default URL
            default_url = self.config.get('default_url', 'https://www.google.com')
            await session.navigate(default_url)
            print(f"[Server] Navigated to {default_url}")

            # Small delay for page to settle
            await asyncio.sleep(0.5)

            # Check if page is simple enough for native rendering
            is_native = await self._check_and_send_native(
                writer, session, seq)

            if not is_native:
                # Send initial full frame (progressive)
                prep = await session.capture_and_prepare()
                if prep:
                    indexed, changed, scroll_dy = prep
                    if changed:
                        count = await self._send_progressive(
                            writer, session, indexed, changed, seq,
                            scroll_dy=scroll_dy, msg_type=MSG_FRAME_FULL,
                        )
                        session.pipeline.commit_frame(indexed)
                        print(f"[Server] Sent initial frame ({count} tiles, progressive)")
                    else:
                        session.pipeline.commit_frame(indexed)

                # Send initial interaction map
                elements, scroll_y, scroll_height = await session.get_interaction_map()
                map_payload = encode_interaction_map(elements, scroll_y, scroll_height)
                await self._send_message(writer, MSG_INTERACTION_MAP, map_payload, seq)
                print(f"[Server] Sent interaction map ({len(elements)} elements)")

            # Send initial status
            status_text = await session.get_status_text()
            await self._send_message(writer, MSG_STATUS,
                                     encode_status(status_text), seq)

            print(f"[Server] Handshake complete. Entering main loop.")

            # --- Main Loop ---
            # ACK event gates frame delivery: set initially so first frame sends
            frame_ack_event = asyncio.Event()
            frame_ack_event.set()

            # YouTube mode state (shared between receive_loop and youtube_loop)
            session.yt_state = {}
            session.yt_ack_event = asyncio.Event()

            # Scroll coalescing: buffer rapid scroll events and execute
            # them as one combined scroll before the next frame capture
            scroll_accum = {'dy_pixels': 0, 'last_time': 0}

            # Run push and receive loops as separate tasks so we can
            # cancel the survivor when one dies (e.g. client disconnect).
            push_task = asyncio.create_task(
                self._push_loop(writer, session, seq,
                                frame_ack_event, scroll_accum))
            recv_task = asyncio.create_task(
                self._receive_loop(reader, writer, session, seq,
                                   frame_ack_event, scroll_accum))

            done, pending = await asyncio.wait(
                [push_task, recv_task],
                return_when=asyncio.FIRST_COMPLETED,
            )

            # Cancel the surviving task
            for task in pending:
                task.cancel()
            for task in pending:
                try:
                    await task
                except (asyncio.CancelledError, Exception):
                    pass

            # Re-raise exception from the completed task (if any)
            for task in done:
                if not task.cancelled():
                    exc = task.exception()
                    if exc:
                        raise exc

        except (asyncio.IncompleteReadError, ConnectionResetError,
                ConnectionAbortedError, BrokenPipeError):
            print(f"[Server] Client {addr} disconnected")
        except OSError as e:
            print(f"[Server] Client {addr} connection error: {e}")
        except asyncio.TimeoutError:
            print(f"[Server] Client {addr} timed out during handshake")
        except asyncio.CancelledError:
            print(f"[Server] Client {addr} handler cancelled (shutdown)")
        except Exception as e:
            print(f"[Server] Error handling client {addr}: {e}")
            import traceback
            traceback.print_exc()
        finally:
            try:
                writer.close()
                await writer.wait_closed()
            except Exception:
                pass

            # Clean up YouTube if active
            if session and hasattr(session, 'yt_state'):
                yt_task = session.yt_state.get('task')
                if yt_task and not yt_task.done():
                    yt_task.cancel()
                    try:
                        await yt_task
                    except (asyncio.CancelledError, Exception):
                        pass
                yt_handler = session.yt_state.get('handler')
                if yt_handler:
                    try:
                        await yt_handler.stop()
                    except Exception:
                        pass

            # Close session to free browser resources immediately
            if session and session.session_id in self.sessions:
                try:
                    await session.close()
                except Exception:
                    pass
                self.sessions.pop(session.session_id, None)

            print(f"[Server] Client {addr} session ended")
            print(f"[Server] Waiting for next client connection...")

    async def _push_loop(self, writer, session, seq, frame_ack_event,
                         scroll_accum):
        """Frame push loop with progressive tile streaming.

        Tiles are compressed and sent in small batches (~40 tiles each),
        interleaving CPU work with network transfer. The DOS client sees
        tiles start appearing ~110ms after capture instead of ~300ms.

        Two modes:
        - ACTIVE: Recent frames had tile changes. Capture immediately after
          each ACK with zero delay.
        - IDLE: No tile changes for several frames. Poll for dirty state
          with short intervals to avoid wasting CPU on screenshots.
        """
        interaction_interval = self.config.get('interaction_scan_interval_ms', 500) / 1000.0
        status_interval = self.config.get('status_update_interval_ms', 2000) / 1000.0
        ack_timeout = 10.0
        last_interaction_time = 0
        last_status_time = 0
        SCROLL_COALESCE_MS = 40  # Wait this long for more scroll events

        # After this many consecutive frames with no tile changes,
        # switch from active to idle (polling) mode
        IDLE_THRESHOLD = 3
        empty_frames = 0
        last_capture_time = 0
        NAV_BURST_MIN_INTERVAL = 0.15  # 150ms between captures during page load

        try:
         while True:
            # Wait for client ACK (or timeout as safety net)
            t_ack_wait = time.perf_counter()
            try:
                await asyncio.wait_for(frame_ack_event.wait(), timeout=ack_timeout)
            except asyncio.TimeoutError:
                pass
            t_ack_got = time.perf_counter()
            if t_ack_got - t_ack_wait > 0.01:
                print(f"[Timing] ACK wait: {(t_ack_got-t_ack_wait)*1000:.1f}ms")
            frame_ack_event.clear()

            # IDLE MODE: poll for dirty state before capturing
            if empty_frames >= IDLE_THRESHOLD:
                loop = asyncio.get_event_loop()
                deadline = loop.time() + ack_timeout
                is_dirty = False
                while not is_dirty and loop.time() < deadline:
                    try:
                        is_dirty = await session.check_dirty()
                    except Exception:
                        is_dirty = False
                    if is_dirty:
                        break

                    # Send non-frame updates while waiting
                    now = time.time()
                    if now - last_interaction_time > interaction_interval:
                        try:
                            elements, sy, sh = await session.get_interaction_map()
                            map_payload = encode_interaction_map(elements, sy, sh)
                            await self._send_message(writer, MSG_INTERACTION_MAP, map_payload, seq)
                        except (ConnectionError, OSError):
                            raise
                        except Exception:
                            pass
                        last_interaction_time = now
                    if now - last_status_time > status_interval:
                        try:
                            status_text = await session.get_status_text()
                            await self._send_message(writer, MSG_STATUS, encode_status(status_text), seq)
                        except (ConnectionError, OSError):
                            raise
                        except Exception:
                            pass
                        last_status_time = now

                    await asyncio.sleep(0.01)  # 10ms poll

            # YOUTUBE MODE: idle completely, youtube_loop handles streaming
            if session.render_mode == 'youtube':
                frame_ack_event.set()
                await asyncio.sleep(0.1)
                continue

            # NATIVE MODE: skip screenshot capture entirely.
            # Content is static until next navigation.
            if session.render_mode == 'native':
                # Still send periodic status updates
                now = time.time()
                if now - last_status_time > status_interval:
                    try:
                        status_text = await session.get_status_text()
                        status_text = '[N] ' + status_text
                        await self._send_message(writer, MSG_STATUS,
                                                 encode_status(status_text), seq)
                    except (ConnectionError, OSError):
                        raise
                    except Exception:
                        pass
                    last_status_time = now
                frame_ack_event.set()
                await asyncio.sleep(0.05)  # 50ms poll in native mode
                continue

            # ACTIVE MODE: capture immediately, no delay

            # Flush any accumulated scroll events before capturing.
            # Waits briefly for more events to coalesce rapid scrolling.
            if scroll_accum['dy_pixels'] != 0:
                elapsed = (time.time() - scroll_accum['last_time']) * 1000
                if elapsed < SCROLL_COALESCE_MS:
                    await asyncio.sleep((SCROLL_COALESCE_MS - elapsed) / 1000)
                total_pixels = scroll_accum['dy_pixels']
                scroll_accum['dy_pixels'] = 0
                scroll_accum['last_time'] = 0
                await session.inject_scroll_pixels(total_pixels)
                await asyncio.sleep(0.01)  # Let browser settle

            # During page load burst, enforce minimum interval between
            # captures so Chrome has time to render more content per frame.
            # Reduces intermediate frames from 4-5 to 2-3.
            if time.time() < session._nav_burst_until:
                since_last = time.time() - last_capture_time
                if since_last < NAV_BURST_MIN_INTERVAL:
                    await asyncio.sleep(NAV_BURST_MIN_INTERVAL - since_last)

            # Capture screenshot, dither/quantize, detect changed tiles
            frame_sent = False
            last_capture_time = time.time()
            prep = await session.capture_and_prepare()
            if prep:
                indexed, changed, scroll_dy = prep
                if changed:
                    # Progressive streaming: compress + send in batches
                    # Tiles start flowing to DOS immediately — url+title
                    # update happens AFTER to avoid 300-400ms stall
                    await self._send_progressive(
                        writer, session, indexed, changed, seq,
                        scroll_dy=scroll_dy,
                    )
                    session.pipeline.commit_frame(indexed)
                    await session.update_page_info()
                    frame_sent = True
                    empty_frames = 0
                else:
                    session.pipeline.commit_frame(indexed)
                    await session.update_page_info()
                    empty_frames += 1
            else:
                empty_frames += 1

            # If no frame was sent, don't wait for an ACK
            if not frame_sent:
                frame_ack_event.set()

            # Periodic non-frame updates
            now = time.time()
            if now - last_interaction_time > interaction_interval:
                try:
                    elements, sy, sh = await session.get_interaction_map()
                    map_payload = encode_interaction_map(elements, sy, sh)
                    await self._send_message(writer, MSG_INTERACTION_MAP, map_payload, seq)
                except (ConnectionError, OSError):
                    raise
                except Exception:
                    pass
                last_interaction_time = now
            if now - last_status_time > status_interval:
                try:
                    status_text = await session.get_status_text()
                    await self._send_message(writer, MSG_STATUS, encode_status(status_text), seq)
                except (ConnectionError, OSError):
                    raise
                except Exception:
                    pass
                last_status_time = now

        except (ConnectionError, OSError):
            return  # Client disconnected — exit cleanly
        except asyncio.CancelledError:
            return  # Task cancelled (sibling died or shutdown)

    async def _receive_loop(self, reader, writer, session, seq, frame_ack_event,
                            scroll_accum):
        """Process incoming messages from the DOS client."""
        while True:
            # Read message header
            header_data = await reader.readexactly(HEADER_SIZE)
            header = decode_header(header_data)

            # Read payload
            payload = b''
            if header['payload_len'] > 0:
                payload = await reader.readexactly(header['payload_len'])

            msg_type = header['msg_type']

            if msg_type == MSG_MOUSE_EVENT:
                # In native mode, check if the click hit a form element.
                # Only switch to screenshot mode if it did.
                if session.render_mode == 'native':
                    evt = decode_mouse_event(payload)
                    if evt['event_type'] == MOUSE_CLICK:
                        try:
                            is_form = await session.page.evaluate(
                                '''([x, y]) => {
                                    const el = document.elementFromPoint(x, y);
                                    if (!el) return false;
                                    const tag = el.tagName;
                                    return tag === 'INPUT' || tag === 'TEXTAREA'
                                        || tag === 'SELECT' || tag === 'BUTTON'
                                        || el.isContentEditable
                                        || !!el.closest('button, label');
                                }''', [evt['x'], evt['y']])
                        except Exception:
                            is_form = False
                        if is_form:
                            session.render_mode = 'screenshot'
                            if session.pipeline:
                                session.pipeline.prev_indexed = None
                            await self._send_message(
                                writer, MSG_MODE_SWITCH,
                                encode_mode_switch(0), seq)
                            print("[Server] Form element clicked -> "
                                  "screenshot mode")
                            await self._handle_mouse(session, payload)
                        # Non-form clicks in native mode: ignore
                    continue
                await self._handle_mouse(session, payload)

            elif msg_type == MSG_KEY_EVENT:
                await self._handle_key(session, payload)

            elif msg_type == MSG_SCROLL_EVENT:
                # Buffer scroll events for coalescing instead of executing
                # immediately. The push_loop flushes accumulated scroll
                # before each frame capture, combining rapid scroll events.
                evt = decode_scroll_event(payload)
                pixels = evt['amount'] * 48
                if evt['direction'] == 1:
                    pixels = -pixels
                scroll_accum['dy_pixels'] += pixels
                scroll_accum['last_time'] = time.time()
                session._dirty = True
                session._interaction_dirty = True

            elif msg_type == MSG_TEXT_INPUT:
                await self._handle_text_input(session, payload)

            elif msg_type == MSG_NAVIGATE:
                url = decode_navigate(payload)
                print(f"[Server] Navigate to: {url}")

                # Check if this is a YouTube URL
                from youtube_handler import is_youtube_url
                if is_youtube_url(url):
                    await self._start_youtube(
                        writer, session, seq, url)
                else:
                    await session.navigate(url)

                    # Send status update immediately
                    status_text = await session.get_status_text()
                    await self._send_message(writer, MSG_STATUS,
                                             encode_status(status_text), seq)

                    # Check complexity for new page
                    await asyncio.sleep(0.3)  # let page settle
                    await self._check_and_send_native(
                        writer, session, seq)

            elif msg_type == MSG_NAV_ACTION:
                action = decode_nav_action(payload)
                if action == NAV_BACK:
                    print("[Server] Nav: Back")
                    await session.go_back()
                elif action == NAV_FORWARD:
                    print("[Server] Nav: Forward")
                    await session.go_forward()
                elif action == NAV_RELOAD:
                    print("[Server] Nav: Reload")
                    await session.reload()
                elif action == NAV_STOP:
                    print("[Server] Nav: Stop")
                    await session.stop_loading()
                elif action == NAV_TOGGLE_MODE:
                    print("[Server] Nav: Toggle mode")
                    if session.render_mode == 'native':
                        # Force screenshot mode
                        session.render_mode = 'screenshot'
                        if session.pipeline:
                            session.pipeline.prev_indexed = None
                        await self._send_message(
                            writer, MSG_MODE_SWITCH,
                            encode_mode_switch(0), seq)
                        print("[Server] Forced screenshot mode")
                    else:
                        # Force native mode (re-extract)
                        await self._check_and_send_native(
                            writer, session, seq)

                # Re-check complexity after navigation actions
                if action in (NAV_BACK, NAV_FORWARD, NAV_RELOAD):
                    await asyncio.sleep(0.5)
                    await self._check_and_send_native(
                        writer, session, seq)

            elif msg_type == MSG_NATIVE_CLICK:
                click = decode_native_click(payload)
                link_id = click['link_id']
                # Look up URL from link table
                href = None
                for lid, url in session.native_link_table:
                    if lid == link_id:
                        href = url
                        break
                if href:
                    print(f"[Server] Native click: link {link_id} -> {href}")
                    await session.navigate(href)
                    status_text = await session.get_status_text()
                    await self._send_message(writer, MSG_STATUS,
                                             encode_status(status_text), seq)
                    await asyncio.sleep(0.3)
                    await self._check_and_send_native(
                        writer, session, seq)
                else:
                    print(f"[Server] Native click: link {link_id} not found")

            elif msg_type == MSG_YT_CONTROL:
                if session.render_mode == 'youtube':
                    ctrl = decode_yt_control(payload)
                    action = ctrl['action']
                    if action == YT_ACTION_STOP:
                        await self._stop_youtube(
                            writer, session, seq)
                    elif action == YT_ACTION_PAUSE:
                        session.yt_state['paused'] = True
                        print("[YouTube] Paused")
                    elif action == YT_ACTION_RESUME:
                        session.yt_state['paused'] = False
                        print("[YouTube] Resumed")

            elif msg_type == MSG_YT_ACK:
                ack = decode_yt_ack(payload)
                session.yt_state['client_audio_ms'] = ack['audio_buffer_ms']
                session.yt_state['client_frame_num'] = ack['last_frame_num']
                session.yt_state['last_ack_time'] = time.monotonic()
                session.yt_ack_event.set()

            elif msg_type == MSG_KEEPALIVE:
                await self._send_message(writer, MSG_KEEPALIVE_ACK, b'', seq)

            elif msg_type == MSG_ACK:
                frame_ack_event.set()

    async def _handle_mouse(self, session, payload):
        """Process a mouse event from the client."""
        evt = decode_mouse_event(payload)
        x, y = evt['x'], evt['y']
        event_type = evt['event_type']
        buttons = evt['buttons']

        if event_type == MOUSE_MOVE:
            await session.inject_mouse_move(x, y)
        elif event_type == MOUSE_CLICK:
            button = 'left'
            if buttons & 0x02:
                button = 'right'
            elif buttons & 0x04:
                button = 'middle'
            await session.inject_mouse_click(x, y, button)
        elif event_type == MOUSE_RELEASE:
            await session.inject_mouse_up()
        elif event_type == MOUSE_DBLCLICK:
            await session.inject_mouse_dblclick(x, y)

    async def _handle_key(self, session, payload):
        """Process a keyboard event from the client."""
        evt = decode_key_event(payload)
        await session.inject_key(
            evt['scancode'], evt['ascii'],
            evt['modifiers'], evt['event_type']
        )

    async def _handle_scroll(self, session, payload):
        """Process a scroll event from the client."""
        evt = decode_scroll_event(payload)
        await session.inject_scroll(evt['direction'], evt['amount'])

    async def _handle_text_input(self, session, payload):
        """Process a text input sync from the client."""
        data = decode_text_input(payload)
        await session.inject_text_input(data['element_id'], data['text'])

    async def _send_message(self, writer, msg_type, payload, seq):
        """Send a single protocol message. Raises on dead connection."""
        if writer.is_closing():
            raise ConnectionError("Connection closing")
        msg = encode_message(msg_type, payload, seq.next())
        writer.write(msg)
        await writer.drain()

    async def _send_progressive(self, writer, session, indexed, changed,
                                 seq, scroll_dy=0, msg_type=MSG_FRAME_DELTA):
        """Compress and send tiles in progressive batches.

        Instead of compressing ALL tiles then sending, this compresses
        a small batch, sends it immediately, then compresses the next.
        The DOS client sees tiles start appearing while the server is
        still processing the rest.

        Returns:
            Total number of tiles sent.
        """
        batch_size = PROGRESSIVE_BATCH_SIZE
        total = len(changed)
        t_start = time.perf_counter()

        for batch_start in range(0, total, batch_size):
            batch_end = min(batch_start + batch_size, total)
            batch_indices = changed[batch_start:batch_end]

            # Compress this batch
            t_comp = time.perf_counter()
            tiles = session.pipeline.compress_tile_batch(indexed, batch_indices)

            # Encode protocol message
            payload = encode_frame_delta(tiles)

            # Set FLAGS_CONTINUED on all but the last batch
            is_last = (batch_end >= total)
            flags = 0 if is_last else FLAG_CONTINUED

            # First batch carries tile-aligned scroll_dy in reserved field.
            # Must match the rounding used by prepare_indexed() so the
            # client's video_shift_content() aligns with the server's shift.
            if batch_start == 0 and scroll_dy != 0:
                tile_size = session.pipeline.tile_size
                aligned_dy = round(scroll_dy / tile_size) * tile_size
                reserved = max(-32768, min(32767, aligned_dy))
            else:
                reserved = 0

            msg = encode_message(msg_type, payload, seq.next(), flags, reserved)
            writer.write(msg)
            # Drain after each batch — pushes data to TCP immediately
            # so the DOS client can start receiving while we compress
            # the next batch.
            await writer.drain()
            t_sent = time.perf_counter()

            if batch_start == 0:
                print(f"[Timing] First batch ({len(batch_indices)} tiles, "
                      f"{len(payload)}B): compress+send={(t_sent-t_comp)*1000:.1f}ms")

        t_end = time.perf_counter()
        print(f"[Timing] All {total} tiles sent in {(t_end-t_start)*1000:.1f}ms "
              f"({total // batch_size + 1} batches)")

        return total

    async def _check_and_send_native(self, writer, session, seq):
        """Check page complexity and send native content if appropriate.

        Returns True if native mode was activated, False otherwise.
        """
        try:
            result = await session.check_complexity()
        except Exception as e:
            print(f"[Server] Complexity check failed: {e}")
            return False

        if not result['recommend_native']:
            if session.render_mode == 'native':
                # Switch back to screenshot mode
                session.render_mode = 'screenshot'
                if session.pipeline:
                    session.pipeline.prev_indexed = None
                await self._send_message(
                    writer, MSG_MODE_SWITCH,
                    encode_mode_switch(0), seq)
                print("[Server] Switched to screenshot mode")
            return False

        # Switch to native mode
        session.render_mode = 'native'
        await self._send_message(
            writer, MSG_MODE_SWITCH,
            encode_mode_switch(1), seq)
        print("[Server] Switched to native mode")

        try:
            # Extract content and generate glyph cache
            glyph_payload, content_payload = \
                await session.extract_native_content()

            # If extraction returned nothing useful (framesets, empty pages),
            # fall back to screenshot mode
            if len(content_payload) < 20:
                print("[Server] Native extraction empty, "
                      "falling back to screenshot mode")
                session.render_mode = 'screenshot'
                if session.pipeline:
                    session.pipeline.prev_indexed = None
                await self._send_message(
                    writer, MSG_MODE_SWITCH,
                    encode_mode_switch(0), seq)
                return False

            # Send glyph cache first — client needs fonts before text
            glyph_messages = split_large_payload(
                MSG_GLYPH_CACHE, glyph_payload, seq)
            for msg in glyph_messages:
                writer.write(msg)
            await writer.drain()
            print(f"[Server] Sent glyph cache "
                  f"({len(glyph_payload)} bytes, "
                  f"{len(glyph_messages)} chunks)")

            # Send content — client renders text/links instantly
            messages = split_large_payload(
                MSG_NATIVE_CONTENT, content_payload, seq)
            for msg in messages:
                writer.write(msg)
            await writer.drain()
            print(f"[Server] Sent native content "
                  f"({len(content_payload)} bytes, "
                  f"{len(messages)} chunks)")

            # Images stream in lazily — each one triggers a client redraw
            while session.has_pending_native_images():
                # Check we're still in native mode (user may have navigated)
                if session.render_mode != 'native':
                    break
                result = await session.process_next_native_image()
                if result:
                    img_id, img_payload = result
                    # Split large images across multiple messages
                    img_messages = split_large_payload(
                        MSG_NATIVE_IMAGE, img_payload, seq)
                    for msg in img_messages:
                        writer.write(msg)
                    await writer.drain()
                    print(f"[Server] Sent native image {img_id} "
                          f"({len(img_payload)} bytes, "
                          f"{len(img_messages)} chunks)")

        except (ConnectionError, OSError):
            raise  # Let connection errors propagate to caller
        except Exception as e:
            print(f"[Server] Native content extraction failed: {e}")
            import traceback
            traceback.print_exc()
            # Fall back to screenshot mode
            session.render_mode = 'screenshot'
            if session.pipeline:
                session.pipeline.prev_indexed = None
            try:
                await self._send_message(
                    writer, MSG_MODE_SWITCH,
                    encode_mode_switch(0), seq)
            except (ConnectionError, OSError):
                raise
            return False

        return True

    async def _start_youtube(self, writer, session, seq, url):
        """Start YouTube video playback mode."""
        from youtube_handler import YouTubeHandler
        from video_pipeline import VideoPipeline

        await self._send_message(writer, MSG_STATUS,
                                 encode_status("Loading YouTube..."), seq)

        try:
            handler = YouTubeHandler(self.config)
            await handler.extract_info(url)
        except Exception as e:
            print(f"[YouTube] Info extraction failed: {e}")
            await self._send_message(writer, MSG_STATUS,
                                     encode_status(f"YouTube error: {str(e)[:60]}"), seq)
            return

        # Switch client to YouTube mode
        await self._send_message(writer, MSG_MODE_SWITCH,
                                 encode_mode_switch(2), seq)

        try:
            await handler.start_video()
        except Exception as e:
            print(f"[YouTube] ffmpeg start failed: {e}")
            await self._send_message(writer, MSG_MODE_SWITCH,
                                     encode_mode_switch(0), seq)
            await self._send_message(writer, MSG_STATUS,
                                     encode_status(f"ffmpeg error: {str(e)[:60]}"), seq)
            await handler.stop()
            return

        # Start audio extraction (optional — silent fallback on failure)
        try:
            await handler.start_audio()
        except Exception as e:
            print(f"[YouTube] Audio extraction failed: {e}, continuing silent")

        # Create video pipeline (reuse palette/LUT from screenshot pipeline)
        pipeline = VideoPipeline(handler.width, handler.height,
                                 session.pipeline.palette,
                                 session.pipeline.lut)

        # Send YT_START with audio info
        audio_rate = handler.audio_rate if handler.has_audio() else 0
        audio_bits = 8 if handler.has_audio() else 0
        yt_start = encode_yt_start(handler.width, handler.height,
                                   handler.fps, audio_rate, audio_bits,
                                   handler.duration, handler.title)
        await self._send_message(writer, MSG_YT_START, yt_start, seq)

        # Enter YouTube mode
        session.render_mode = 'youtube'
        yt_state = session.yt_state
        yt_state['handler'] = handler
        yt_state['pipeline'] = pipeline
        yt_state['paused'] = False
        yt_state['stopping'] = False
        yt_state['client_audio_ms'] = 0
        yt_state['client_frame_num'] = 0xFFFF
        yt_state['last_ack_time'] = time.monotonic()
        session.yt_ack_event.set()  # allow first frame

        yt_state['task'] = asyncio.create_task(
            self._youtube_loop(writer, session, seq))

        print(f"[YouTube] Started: {handler.title} ({handler.duration}s)")

    async def _stop_youtube(self, writer, session, seq):
        """Stop YouTube playback and return to screenshot mode."""
        yt_state = session.yt_state
        yt_state['stopping'] = True

        # Kill ffmpeg FIRST — closes the pipe, unblocks read_video_frame
        handler = yt_state.get('handler')
        if handler:
            for proc in [handler._video_proc, handler._audio_proc]:
                if proc and proc.returncode is None:
                    try:
                        proc.kill()
                    except Exception:
                        pass

        # Cancel youtube loop and wait for it to finish writing
        task = yt_state.get('task')
        if task and not task.done():
            task.cancel()
            try:
                await asyncio.wait_for(
                    asyncio.shield(task), timeout=3.0)
            except (asyncio.CancelledError, asyncio.TimeoutError,
                    Exception):
                pass

        # Clean up handler
        if handler:
            handler._video_proc = None
            handler._audio_proc = None

        # Switch to screenshot mode
        session.render_mode = 'screenshot'
        if session.pipeline:
            session.pipeline.prev_indexed = None
        session._dirty = True
        await self._send_message(writer, MSG_MODE_SWITCH,
                                 encode_mode_switch(0), seq)
        print("[YouTube] Stopped, back to screenshot mode")

    async def _youtube_loop(self, writer, session, seq):
        """Stream video frames and audio from ffmpeg to the DOS client.

        Simple single-loop design: one audio chunk + one video frame per
        iteration, paced to real-time FPS. Audio chunk size = audio_rate/fps
        so audio production exactly matches DMA consumption rate.

        Extra audio is sent when the client reports low buffer level.
        """
        yt_state = session.yt_state
        yt_ack_event = session.yt_ack_event
        handler = yt_state['handler']
        pipeline = yt_state['pipeline']
        has_audio = handler.has_audio()

        # Audio chunk = exactly one frame's worth of audio samples.
        # At 11025 Hz / 10 fps = 1102 bytes/frame = 11020 bytes/sec,
        # which matches the DMA consumption rate almost exactly.
        audio_per_frame = (int(handler.audio_rate / handler.fps)
                           if has_audio else 0)

        frame_num = 0
        start_time = time.monotonic()
        frames_sent = 0

        try:
            # Pre-buffer: send 5 audio chunks (~500ms at 11025/10)
            if has_audio:
                for i in range(5):
                    audio_data = await handler.read_audio(audio_per_frame)
                    if audio_data:
                        payload = encode_yt_audio(0, audio_data)
                        await self._send_message(
                            writer, MSG_YT_AUDIO, payload, seq)
                    else:
                        has_audio = False
                        break
                if has_audio:
                    print(f"[YouTube] Pre-buffered "
                          f"{5 * audio_per_frame} audio samples "
                          f"(~{5 * audio_per_frame * 1000 // handler.audio_rate}ms)")

            while handler.is_running() and not yt_state.get('stopping'):
                # Wait for ACK from client
                try:
                    await asyncio.wait_for(yt_ack_event.wait(), timeout=5.0)
                except asyncio.TimeoutError:
                    print("[YouTube] ACK timeout, continuing")
                yt_ack_event.clear()

                # Handle pause
                while yt_state.get('paused') and not yt_state.get('stopping'):
                    await asyncio.sleep(0.1)
                    start_time += 0.1

                if yt_state.get('stopping'):
                    break

                timestamp_ms = int(frame_num * 1000 / handler.fps)

                # Send audio chunk for this frame interval
                if has_audio:
                    audio_data = await handler.read_audio(audio_per_frame)
                    if audio_data:
                        payload = encode_yt_audio(timestamp_ms, audio_data)
                        await self._send_message(
                            writer, MSG_YT_AUDIO, payload, seq)
                    else:
                        has_audio = False

                    # Send extra audio if client buffer is low
                    client_ms = yt_state.get('client_audio_ms', 500)
                    if client_ms < 400 and has_audio:
                        extra = await handler.read_audio(audio_per_frame)
                        if extra:
                            payload = encode_yt_audio(timestamp_ms, extra)
                            await self._send_message(
                                writer, MSG_YT_AUDIO, payload, seq)

                # Read next video frame from ffmpeg
                rgb_data = await handler.read_video_frame()
                if rgb_data is None:
                    await self._send_message(writer, MSG_YT_EOF, b'', seq)
                    print(f"[YouTube] EOF after {frames_sent} frames")
                    break

                # Process through dither pipeline
                blocks = pipeline.process_frame(rgb_data)

                # Encode and send video frame
                await self._send_yt_frame(writer, seq, frame_num,
                                          timestamp_ms, blocks)

                frame_num += 1
                frames_sent += 1

                if frames_sent % 100 == 0:
                    elapsed = time.monotonic() - start_time
                    actual_fps = frames_sent / elapsed if elapsed > 0 else 0
                    client_ms = yt_state.get('client_audio_ms', 0)
                    print(f"[YouTube] {frames_sent} frames, "
                          f"{actual_fps:.1f} FPS, "
                          f"audio buf {client_ms}ms")

                # Pace to target FPS
                target_time = start_time + frame_num / handler.fps
                now = time.monotonic()
                if target_time > now:
                    await asyncio.sleep(target_time - now)

        except (ConnectionError, OSError):
            print("[YouTube] Client disconnected during playback")
        except asyncio.CancelledError:
            pass
        except Exception as e:
            print(f"[YouTube] Error: {e}")
            import traceback
            traceback.print_exc()
        finally:
            if not yt_state.get('stopping'):
                try:
                    await handler.stop()
                except Exception:
                    pass
                try:
                    session.render_mode = 'screenshot'
                    if session.pipeline:
                        session.pipeline.prev_indexed = None
                    session._dirty = True
                    await self._send_message(writer, MSG_MODE_SWITCH,
                                             encode_mode_switch(0), seq)
                    print("[YouTube] Ended, back to screenshot mode")
                except (ConnectionError, OSError):
                    pass

    async def _send_yt_frame(self, writer, seq, frame_num,
                              timestamp_ms, blocks):
        """Send a YouTube frame, splitting across messages if needed."""
        MAX_CHUNK = 58000
        BLOCK_OVERHEAD = 4  # bx(1) + by(1) + comp_size(2)
        HEADER_OVERHEAD = 10  # frame_num(4) + timestamp_ms(4) + block_count(2)

        if not blocks:
            # Empty frame (no changes) — still send so client can ACK
            payload = struct.pack('<IIH', frame_num, timestamp_ms, 0)
            await self._send_message(writer, MSG_YT_FRAME, payload, seq)
            return

        # Split blocks into chunks that fit in max payload
        chunks = []
        current_blocks = []
        current_size = HEADER_OVERHEAD

        for bx, by, rle_data in blocks:
            entry_size = BLOCK_OVERHEAD + len(rle_data)
            if current_size + entry_size > MAX_CHUNK and current_blocks:
                chunks.append(current_blocks)
                current_blocks = []
                current_size = HEADER_OVERHEAD
            current_blocks.append((bx, by, rle_data))
            current_size += entry_size

        if current_blocks:
            chunks.append(current_blocks)

        for i, chunk_blocks in enumerate(chunks):
            is_last = (i == len(chunks) - 1)
            payload = encode_yt_frame_chunk(frame_num, timestamp_ms,
                                            chunk_blocks)
            flags = 0 if is_last else FLAG_CONTINUED
            msg = encode_message(MSG_YT_FRAME, payload, seq.next(), flags)
            writer.write(msg)
            await writer.drain()

    async def _send_tiles(self, writer, tiles, seq, full=False, scroll_dy=0):
        """Send tile data, splitting into multiple messages if needed.

        Uses FRAME_FULL for initial/full frames, FRAME_DELTA for updates.
        Splits payload at ~60KB boundaries to stay within uint16 payload_len.
        scroll_dy is passed in header.reserved of the first chunk for
        client-side prev_tiles shift optimization.
        """
        msg_type = MSG_FRAME_FULL if full else MSG_FRAME_DELTA
        max_tiles_per_msg = 200  # ~50KB assuming ~256 bytes avg per tile

        for i in range(0, len(tiles), max_tiles_per_msg):
            chunk = tiles[i:i + max_tiles_per_msg]
            payload = encode_frame_delta(chunk)

            flags = 0
            if i + max_tiles_per_msg < len(tiles):
                flags |= FLAG_CONTINUED

            # First chunk carries scroll_dy in reserved field
            reserved = scroll_dy if i == 0 else 0
            # Clamp to int16 range
            reserved = max(-32768, min(32767, reserved))
            msg = encode_message(msg_type, payload, seq.next(), flags, reserved)
            writer.write(msg)

        await writer.drain()

    async def _get_or_create_session(self, hello):
        """Get an existing session by ID or create a new one."""
        requested_id = hello.get('session_id', 0)

        if requested_id and requested_id in self.sessions:
            session = self.sessions[requested_id]
            # Close old context if it exists (will be reconfigured)
            await session.close()
            return session

        # Create new session
        session_id = self._next_session_id
        self._next_session_id += 1

        session = BrowserSession(self.browser, session_id, self.config)
        self.sessions[session_id] = session
        return session

    async def _cleanup_sessions_loop(self):
        """Periodically clean up expired sessions."""
        timeout = self.config.get('session_timeout_seconds', 300)

        while True:
            await asyncio.sleep(60)

            now = time.time()
            expired = [
                sid for sid, session in self.sessions.items()
                if now - session.last_activity > timeout
            ]

            for sid in expired:
                print(f"[Server] Cleaning up expired session {sid}")
                session = self.sessions.pop(sid)
                await session.close()

    async def shutdown(self):
        """Clean shutdown — suppress errors since browser may already be gone."""
        print("\n[Server] Shutting down...")

        for session in list(self.sessions.values()):
            try:
                await session.close()
            except Exception:
                pass
        self.sessions.clear()

        if self.browser:
            try:
                await self.browser.close()
            except Exception:
                pass
        if self.playwright:
            try:
                await self.playwright.stop()
            except Exception:
                pass

        print("[Server] Shutdown complete.")


def run_server():
    """Entry point with clean Ctrl+C handling."""
    import signal

    parser = argparse.ArgumentParser(description='RetroSurf Rendering Server')
    parser.add_argument('--port', type=int, default=None,
                        help='TCP port (default: 8086)')
    parser.add_argument('--config', type=str, default=None,
                        help='Path to config.json')
    parser.add_argument('--url', type=str, default=None,
                        help='Default URL to load')
    args = parser.parse_args()

    config = load_config(args.config)

    if args.port:
        config['port'] = args.port
    if args.url:
        config['default_url'] = args.url

    server = RetroSurfServer(config)

    async def _run():
        try:
            await server.start()
        except asyncio.CancelledError:
            pass
        finally:
            await server.shutdown()

    try:
        asyncio.run(_run())
    except KeyboardInterrupt:
        # asyncio.run already cancelled tasks — just print clean exit
        print("\n[Server] Interrupted. Goodbye.")


if __name__ == '__main__':
    run_server()
