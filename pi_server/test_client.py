"""Test client that simulates a DOS client connecting to the RetroSurf server.

Connects to the server, completes the handshake, receives frames, and
logs statistics. Optionally sends mouse/keyboard events to test
interaction.

Usage:
    python test_client.py [--host 127.0.0.1] [--port 8086]

Run the server first:
    python server.py
"""

import argparse
import asyncio
import struct
import time
import os

import numpy as np
from PIL import Image

from protocol import (
    HEADER_SIZE,
    MSG_CLIENT_HELLO, MSG_MOUSE_EVENT, MSG_KEY_EVENT,
    MSG_SCROLL_EVENT, MSG_NAVIGATE, MSG_NAV_ACTION,
    MSG_SERVER_HELLO, MSG_PALETTE, MSG_FRAME_FULL, MSG_FRAME_DELTA,
    MSG_INTERACTION_MAP, MSG_CURSOR_SHAPE, MSG_STATUS, MSG_KEEPALIVE_ACK,
    NAV_BACK, NAV_FORWARD, NAV_RELOAD,
    MOUSE_CLICK, MOUSE_MOVE,
    decode_header, decode_server_hello, decode_frame_delta,
    encode_message, encode_client_hello, encode_mouse_event,
    encode_navigate, encode_scroll_event, encode_nav_action,
    SequenceCounter,
)
from image_pipeline import rle_decompress
from palette import indexed_to_rgb


CONTENT_WIDTH = 640
CONTENT_HEIGHT = 456
CHROME_HEIGHT = 24
TILE_SIZE = 16

OUTPUT_DIR = os.path.join(os.path.dirname(__file__), 'test_output')


class TestClient:
    """Simulates a DOS client for testing the server."""

    def __init__(self, host, port):
        self.host = host
        self.port = port
        self.reader = None
        self.writer = None
        self.seq = SequenceCounter()

        # Display state
        self.tile_cols = CONTENT_WIDTH // TILE_SIZE
        self.tile_rows = (CONTENT_HEIGHT + TILE_SIZE - 1) // TILE_SIZE
        tile_count = self.tile_cols * self.tile_rows
        self.tile_cache = [
            np.zeros((TILE_SIZE, TILE_SIZE), dtype=np.uint8)
            for _ in range(tile_count)
        ]
        self.framebuffer = np.zeros((CONTENT_HEIGHT, CONTENT_WIDTH), dtype=np.uint8)
        self.palette = None

        # Stats
        self.frames_received = 0
        self.total_tiles_received = 0
        self.total_bytes_received = 0
        self.interaction_maps_received = 0
        self.start_time = None

    async def connect(self):
        """Connect to the server and perform handshake."""
        print(f"Connecting to {self.host}:{self.port}...")
        self.reader, self.writer = await asyncio.open_connection(
            self.host, self.port
        )
        self.start_time = time.time()
        print("Connected!")

        # Send CLIENT_HELLO
        hello_payload = encode_client_hello(
            protocol_version=1,
            screen_width=CONTENT_WIDTH,
            screen_height=CONTENT_HEIGHT + CHROME_HEIGHT,
            color_depth=8,
            tile_size=TILE_SIZE,
            chrome_height=CHROME_HEIGHT,
            max_recv_buffer_kb=32,
            session_id=0,
        )
        msg = encode_message(MSG_CLIENT_HELLO, hello_payload, self.seq.next())
        self.writer.write(msg)
        await self.writer.drain()
        print("Sent CLIENT_HELLO")

        # Wait for SERVER_HELLO
        header = await self._read_header()
        assert header['msg_type'] == MSG_SERVER_HELLO, \
            f"Expected SERVER_HELLO, got 0x{header['msg_type']:02X}"
        payload = await self.reader.readexactly(header['payload_len'])
        server_hello = decode_server_hello(payload)
        print(f"Received SERVER_HELLO: session={server_hello['session_id']}, "
              f"content={server_hello['content_width']}x{server_hello['content_height']}")

        # Wait for PALETTE
        header = await self._read_header()
        assert header['msg_type'] == MSG_PALETTE, \
            f"Expected PALETTE, got 0x{header['msg_type']:02X}"
        palette_data = await self.reader.readexactly(header['payload_len'])
        self.palette = np.frombuffer(palette_data, dtype=np.uint8).reshape(-1, 3)
        print(f"Received PALETTE ({len(self.palette)} colors)")

        print("Handshake complete!\n")

    async def receive_loop(self, duration=10.0):
        """Receive and process messages for a given duration."""
        print(f"Receiving messages for {duration} seconds...\n")
        end_time = time.time() + duration

        while time.time() < end_time:
            try:
                header = await asyncio.wait_for(
                    self._read_header(), timeout=1.0
                )
            except asyncio.TimeoutError:
                continue

            payload = b''
            if header['payload_len'] > 0:
                payload = await self.reader.readexactly(header['payload_len'])

            self.total_bytes_received += HEADER_SIZE + len(payload)
            await self._process_message(header, payload)

    async def _process_message(self, header, payload):
        """Process a received message."""
        msg_type = header['msg_type']

        if msg_type in (MSG_FRAME_FULL, MSG_FRAME_DELTA):
            frame_type = "FULL" if msg_type == MSG_FRAME_FULL else "DELTA"
            tiles = decode_frame_delta(payload)

            for tile_idx, compressed in tiles:
                self._apply_tile(tile_idx, compressed)

            self.frames_received += 1
            self.total_tiles_received += len(tiles)

            elapsed = time.time() - self.start_time
            print(f"  [{elapsed:6.1f}s] FRAME_{frame_type}: "
                  f"{len(tiles)} tiles, {len(payload):,} bytes")

        elif msg_type == MSG_INTERACTION_MAP:
            # Parse element count from first 2 bytes
            if len(payload) >= 2:
                elem_count = struct.unpack('<H', payload[:2])[0]
                self.interaction_maps_received += 1
                print(f"           INTERACTION_MAP: {elem_count} elements")

        elif msg_type == MSG_STATUS:
            status_text = payload.decode('utf-8', errors='replace')
            print(f"           STATUS: {status_text}")

        elif msg_type == MSG_CURSOR_SHAPE:
            cursor_type = payload[0] if payload else 0
            names = {0: 'arrow', 1: 'hand', 2: 'text', 3: 'wait'}
            print(f"           CURSOR: {names.get(cursor_type, cursor_type)}")

        elif msg_type == MSG_KEEPALIVE_ACK:
            pass

    def _apply_tile(self, tile_idx, compressed):
        """Decompress and apply a tile update to the framebuffer."""
        row = tile_idx // self.tile_cols
        col = tile_idx % self.tile_cols
        y0 = row * TILE_SIZE
        x0 = col * TILE_SIZE
        y1 = min(y0 + TILE_SIZE, CONTENT_HEIGHT)
        x1 = min(x0 + TILE_SIZE, CONTENT_WIDTH)
        tile_h = y1 - y0
        tile_w = x1 - x0

        # Decompress
        raw = rle_decompress(compressed, tile_h * tile_w)

        # XOR with previous tile data
        prev = self.tile_cache[tile_idx][:tile_h, :tile_w].tobytes()
        new_data = bytearray(len(raw))
        for i in range(len(raw)):
            new_data[i] = raw[i] ^ prev[i]

        # Update tile cache
        tile_arr = np.frombuffer(bytes(new_data), dtype=np.uint8).reshape(tile_h, tile_w)
        self.tile_cache[tile_idx][:tile_h, :tile_w] = tile_arr

        # Update framebuffer
        self.framebuffer[y0:y1, x0:x1] = tile_arr

    def save_framebuffer(self, filename='client_frame.png'):
        """Save the current framebuffer as a PNG image."""
        if self.palette is None:
            print("No palette received yet, cannot save.")
            return

        rgb = indexed_to_rgb(self.framebuffer, self.palette)
        path = os.path.join(OUTPUT_DIR, filename)
        os.makedirs(OUTPUT_DIR, exist_ok=True)
        Image.fromarray(rgb).save(path)
        print(f"\nFramebuffer saved to {path}")

    async def send_click(self, x, y):
        """Send a mouse click event."""
        payload = encode_mouse_event(x, y, 0x01, MOUSE_CLICK)
        msg = encode_message(MSG_MOUSE_EVENT, payload, self.seq.next())
        self.writer.write(msg)
        await self.writer.drain()
        print(f"\n  >> Sent CLICK at ({x}, {y})")

    async def send_scroll(self, direction=0, amount=3):
        """Send a scroll event. direction: 0=down, 1=up."""
        payload = encode_scroll_event(direction, amount)
        msg = encode_message(MSG_SCROLL_EVENT, payload, self.seq.next())
        self.writer.write(msg)
        await self.writer.drain()
        dir_name = "DOWN" if direction == 0 else "UP"
        print(f"\n  >> Sent SCROLL {dir_name} x{amount}")

    async def send_navigate(self, url):
        """Send a navigation request."""
        payload = encode_navigate(url)
        msg = encode_message(MSG_NAVIGATE, payload, self.seq.next())
        self.writer.write(msg)
        await self.writer.drain()
        print(f"\n  >> Sent NAVIGATE to {url}")

    async def send_nav_action(self, action):
        """Send a navigation action (back/forward/reload)."""
        payload = encode_nav_action(action)
        msg = encode_message(MSG_NAV_ACTION, payload, self.seq.next())
        self.writer.write(msg)
        await self.writer.drain()

    def print_stats(self):
        """Print session statistics."""
        elapsed = time.time() - self.start_time
        print("\n" + "=" * 50)
        print("TEST CLIENT STATISTICS")
        print("=" * 50)
        print(f"Duration:            {elapsed:.1f}s")
        print(f"Frames received:     {self.frames_received}")
        print(f"Total tiles:         {self.total_tiles_received}")
        print(f"Interaction maps:    {self.interaction_maps_received}")
        print(f"Total data received: {self.total_bytes_received:,} bytes")
        if elapsed > 0:
            print(f"Avg data rate:       {self.total_bytes_received/elapsed/1024:.1f} KB/s")
            print(f"Avg frame rate:      {self.frames_received/elapsed:.1f} FPS")

    async def _read_header(self):
        """Read and decode a message header."""
        data = await self.reader.readexactly(HEADER_SIZE)
        return decode_header(data)

    async def close(self):
        """Close the connection."""
        if self.writer:
            self.writer.close()
            try:
                await self.writer.wait_closed()
            except Exception:
                pass


async def main():
    parser = argparse.ArgumentParser(description='RetroSurf Test Client')
    parser.add_argument('--host', default='127.0.0.1', help='Server host')
    parser.add_argument('--port', type=int, default=8086, help='Server port')
    parser.add_argument('--duration', type=int, default=15,
                        help='Test duration in seconds')
    parser.add_argument('--url', type=str, default=None,
                        help='URL to navigate to after connection')
    args = parser.parse_args()

    client = TestClient(args.host, args.port)

    try:
        await client.connect()

        # If a URL was specified, navigate to it
        if args.url:
            await client.send_navigate(args.url)
            await asyncio.sleep(1)

        # Receive initial frame + interaction map
        print("--- Receiving initial content ---")
        await client.receive_loop(duration=5)

        # Save the initial framebuffer
        client.save_framebuffer('client_01_initial.png')

        # Test scrolling
        print("\n--- Testing scroll ---")
        await client.send_scroll(direction=0, amount=5)
        await client.receive_loop(duration=3)
        client.save_framebuffer('client_02_after_scroll.png')

        # Test clicking the search box (approximate position for Google)
        print("\n--- Testing click ---")
        await client.send_click(320, 260)
        await client.receive_loop(duration=3)
        client.save_framebuffer('client_03_after_click.png')

        # Navigate to a different page
        if not args.url:
            print("\n--- Testing navigation ---")
            await client.send_navigate('https://en.wikipedia.org')
            await client.receive_loop(duration=5)
            client.save_framebuffer('client_04_wikipedia.png')

        # Print final stats
        client.print_stats()

    except ConnectionRefusedError:
        print(f"ERROR: Could not connect to {args.host}:{args.port}")
        print("Is the server running? Start it with: python server.py")
    except Exception as e:
        print(f"ERROR: {e}")
        import traceback
        traceback.print_exc()
    finally:
        await client.close()


if __name__ == '__main__':
    asyncio.run(main())
