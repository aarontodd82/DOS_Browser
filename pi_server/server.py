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
    NAV_BACK, NAV_FORWARD, NAV_RELOAD, NAV_STOP, NAV_TOGGLE_MODE,
    MOUSE_MOVE, MOUSE_CLICK, MOUSE_RELEASE, MOUSE_DBLCLICK,
    FLAG_CONTINUED,
    decode_header, decode_client_hello, decode_mouse_event,
    decode_key_event, decode_scroll_event, decode_text_input,
    decode_navigate, decode_nav_action, decode_native_click,
    encode_message, encode_server_hello, encode_palette,
    encode_frame_delta, encode_interaction_map, encode_status,
    encode_mode_switch,
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

            # Scroll coalescing: buffer rapid scroll events and execute
            # them as one combined scroll before the next frame capture
            scroll_accum = {'dy_pixels': 0, 'last_time': 0}

            # Run push and receive loops concurrently
            await asyncio.gather(
                self._push_loop(writer, session, seq, frame_ack_event, scroll_accum),
                self._receive_loop(reader, writer, session, seq, frame_ack_event, scroll_accum),
            )

        except asyncio.IncompleteReadError:
            print(f"[Server] Client {addr} disconnected (incomplete read)")
        except ConnectionResetError:
            print(f"[Server] Client {addr} connection reset")
        except asyncio.TimeoutError:
            print(f"[Server] Client {addr} timed out during handshake")
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
            print(f"[Server] Client {addr} connection closed")

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
                        except Exception:
                            pass
                        last_interaction_time = now
                    if now - last_status_time > status_interval:
                        try:
                            status_text = await session.get_status_text()
                            await self._send_message(writer, MSG_STATUS, encode_status(status_text), seq)
                        except Exception:
                            pass
                        last_status_time = now

                    await asyncio.sleep(0.01)  # 10ms poll

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
                except Exception:
                    pass
                last_interaction_time = now
            if now - last_status_time > status_interval:
                try:
                    status_text = await session.get_status_text()
                    await self._send_message(writer, MSG_STATUS, encode_status(status_text), seq)
                except Exception:
                    pass
                last_status_time = now

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
        """Send a single protocol message."""
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

        except Exception as e:
            print(f"[Server] Native content extraction failed: {e}")
            import traceback
            traceback.print_exc()
            # Fall back to screenshot mode
            session.render_mode = 'screenshot'
            if session.pipeline:
                session.pipeline.prev_indexed = None
            await self._send_message(
                writer, MSG_MODE_SWITCH,
                encode_mode_switch(0), seq)
            return False

        return True

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
        """Clean shutdown."""
        print("[Server] Shutting down...")

        for session in self.sessions.values():
            await session.close()
        self.sessions.clear()

        if self.browser:
            await self.browser.close()
        if self.playwright:
            await self.playwright.stop()

        print("[Server] Shutdown complete.")


async def main():
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

    try:
        await server.start()
    except KeyboardInterrupt:
        pass
    finally:
        await server.shutdown()


if __name__ == '__main__':
    asyncio.run(main())
