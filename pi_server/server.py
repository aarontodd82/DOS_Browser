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
    NAV_BACK, NAV_FORWARD, NAV_RELOAD, NAV_STOP,
    MOUSE_MOVE, MOUSE_CLICK, MOUSE_RELEASE, MOUSE_DBLCLICK,
    FLAG_CONTINUED,
    decode_header, decode_client_hello, decode_mouse_event,
    decode_key_event, decode_scroll_event, decode_text_input,
    decode_navigate, decode_nav_action,
    encode_message, encode_server_hello, encode_palette,
    encode_frame_delta, encode_interaction_map, encode_status,
    SequenceCounter,
)


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

            # Send initial full frame
            result = await session.capture_frame()
            if result:
                tiles, _ = result
                if tiles:
                    await self._send_tiles(writer, tiles, seq, full=True)
                    print(f"[Server] Sent initial frame ({len(tiles)} tiles)")

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

            # Run push and receive loops concurrently
            await asyncio.gather(
                self._push_loop(writer, session, seq, frame_ack_event),
                self._receive_loop(reader, writer, session, seq, frame_ack_event),
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

    async def _push_loop(self, writer, session, seq, frame_ack_event):
        """Frame push loop — streams as fast as the client can receive.

        Two modes:
        - ACTIVE: Recent frames had tile changes. Capture immediately after
          each ACK with zero delay. This maximizes throughput during page
          loads and content changes.
        - IDLE: No tile changes for several frames. Poll for dirty state
          with short intervals to avoid wasting CPU on screenshots.
        """
        interaction_interval = self.config.get('interaction_scan_interval_ms', 500) / 1000.0
        status_interval = self.config.get('status_update_interval_ms', 2000) / 1000.0
        ack_timeout = 10.0
        last_interaction_time = 0
        last_status_time = 0

        # After this many consecutive frames with no tile changes,
        # switch from active to idle (polling) mode
        IDLE_THRESHOLD = 3
        empty_frames = 0

        while True:
            # Wait for client ACK (or timeout as safety net)
            try:
                await asyncio.wait_for(frame_ack_event.wait(), timeout=ack_timeout)
            except asyncio.TimeoutError:
                pass
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

                    await asyncio.sleep(0.01)  # 10ms poll (was 50ms)

            # ACTIVE MODE: capture immediately, no delay

            # Capture and send frame
            frame_sent = False
            result = await session.capture_frame()
            if result:
                tiles, scroll_dy = result
                if tiles:
                    await self._send_tiles(writer, tiles, seq,
                                           scroll_dy=scroll_dy)
                    frame_sent = True
                    empty_frames = 0
                else:
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

    async def _receive_loop(self, reader, writer, session, seq, frame_ack_event):
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
                await self._handle_mouse(session, payload)

            elif msg_type == MSG_KEY_EVENT:
                await self._handle_key(session, payload)

            elif msg_type == MSG_SCROLL_EVENT:
                await self._handle_scroll(session, payload)

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
