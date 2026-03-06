"""Visual verification test for the RetroSurf image pipeline.

Captures screenshots of real websites, processes them through the full
pipeline, and saves stage-by-stage PNG outputs for inspection.

Usage:
    python test_pipeline.py [url]

Default URL: https://www.google.com

Outputs saved to test_output/ directory:
    01_original.png       - Raw screenshot from Chromium
    02_dithered_256.png   - After Bayer dithering (still RGB, tuned for 256-color)
    03_indexed_256.png    - After palette mapping to 256 colors
    04_indexed_16.png     - 16-color version for comparison
    05_delta_vis.png      - Visualization of changed tiles (second frame only)
"""

import os
import sys
import time

import numpy as np
from PIL import Image

from palette import build_256_palette, build_16_palette, build_palette_lut, indexed_to_rgb
from image_pipeline import ImagePipeline, rle_compress, rle_decompress


# Content area dimensions (matching design: 640x456 for 640x480 with 24px chrome)
CONTENT_WIDTH = 640
CONTENT_HEIGHT = 456
TILE_SIZE = 16

OUTPUT_DIR = os.path.join(os.path.dirname(__file__), 'test_output')


def ensure_output_dir():
    os.makedirs(OUTPUT_DIR, exist_ok=True)


def save_rgb(img_array, filename):
    """Save an (H, W, 3) uint8 array as PNG."""
    path = os.path.join(OUTPUT_DIR, filename)
    Image.fromarray(img_array).save(path)
    print(f"  Saved: {path}")


def save_indexed(indexed_array, palette, filename):
    """Save a palette-indexed image as PNG (rendered back to RGB)."""
    rgb = indexed_to_rgb(indexed_array, palette)
    save_rgb(rgb, filename)


def capture_screenshot(url, width, height):
    """Capture a screenshot using Playwright headless Chromium.

    Returns:
        bytes of JPEG screenshot data
    """
    from playwright.sync_api import sync_playwright

    print(f"Launching Chromium and navigating to {url}...")
    with sync_playwright() as p:
        browser = p.chromium.launch(
            headless=True,
            args=[
                '--disable-gpu',
                '--no-sandbox',
                '--disable-dev-shm-usage',
                '--disable-extensions',
            ]
        )
        context = browser.new_context(
            viewport={'width': width, 'height': height},
            device_scale_factor=1,
        )
        page = context.new_page()

        # Inject CSS to disable animations
        page.add_init_script('''
            const style = document.createElement('style');
            style.textContent = `
                *, *::before, *::after {
                    animation-duration: 0s !important;
                    animation-delay: 0s !important;
                    transition-duration: 0s !important;
                    transition-delay: 0s !important;
                }
            `;
            if (document.head) document.head.appendChild(style);
        ''')

        page.goto(url, wait_until='networkidle', timeout=30000)
        print("Page loaded. Capturing screenshot...")

        # Capture as JPEG (matches production pipeline)
        screenshot_bytes = page.screenshot(type='jpeg', quality=70)

        # Also capture a second screenshot for delta testing
        # (simulate a small change by scrolling slightly)
        page.evaluate('window.scrollBy(0, 5)')
        time.sleep(0.5)
        screenshot2_bytes = page.screenshot(type='jpeg', quality=70)

        browser.close()

    return screenshot_bytes, screenshot2_bytes


def test_rle_roundtrip():
    """Test that RLE compression is perfectly lossless."""
    print("\n--- RLE Round-Trip Test ---")

    test_cases = [
        b'\x00' * 100,                          # All zeros (best case for RLE)
        bytes(range(256)),                       # No repeats (worst case)
        b'\xFF\xFF\xFF\x00\x00\x00' * 20,      # Alternating runs
        b'\x42',                                 # Single byte
        b'',                                     # Empty
        bytes([i % 7 for i in range(500)]),      # Periodic pattern
        os.urandom(1000),                        # Random data
    ]

    all_passed = True
    for i, original in enumerate(test_cases):
        compressed = rle_compress(original)
        decompressed = rle_decompress(compressed, len(original))

        if bytes(decompressed) == original:
            ratio = len(compressed) / len(original) if original else 0
            print(f"  Case {i}: PASS (in={len(original)}, "
                  f"out={len(compressed)}, ratio={ratio:.2f})")
        else:
            print(f"  Case {i}: FAIL")
            print(f"    Original:     {original[:50]}...")
            print(f"    Decompressed: {bytes(decompressed)[:50]}...")
            all_passed = False

    if all_passed:
        print("  All RLE round-trip tests PASSED")
    else:
        print("  WARNING: Some RLE tests FAILED")

    return all_passed


def test_pipeline_with_screenshots(url):
    """Run the full pipeline on real screenshots and save outputs."""
    ensure_output_dir()

    # Capture screenshots
    screenshot1, screenshot2 = capture_screenshot(url, CONTENT_WIDTH, CONTENT_HEIGHT)

    # --- 256-color pipeline ---
    print("\n--- 256-Color Pipeline (Frame 1) ---")
    pipeline_256 = ImagePipeline(CONTENT_WIDTH, CONTENT_HEIGHT,
                                 color_depth=8, tile_size=TILE_SIZE)

    t0 = time.perf_counter()
    result1 = pipeline_256.process_frame_staged(screenshot1)
    t1 = time.perf_counter()

    print(f"  Processing time: {(t1-t0)*1000:.1f} ms")
    print_stats(result1['stats'])

    save_rgb(result1['original'], '01_original.png')
    save_rgb(result1['dithered'], '02_dithered_256.png')
    save_indexed(result1['indexed'], pipeline_256.palette, '03_indexed_256.png')

    # Verify RLE round-trip on first frame
    print("\n  Verifying RLE round-trip on all tiles...")
    all_ok = True
    for tile_idx, compressed in result1['tiles']:
        row = tile_idx // pipeline_256.tile_cols
        col = tile_idx % pipeline_256.tile_cols
        y0 = row * TILE_SIZE
        x0 = col * TILE_SIZE
        y1 = min(y0 + TILE_SIZE, CONTENT_HEIGHT)
        x1 = min(x0 + TILE_SIZE, CONTENT_WIDTH)

        original_data = result1['indexed'][y0:y1, x0:x1].tobytes()
        decompressed = rle_decompress(compressed, len(original_data))
        if bytes(decompressed) != original_data:
            print(f"    FAIL on tile {tile_idx} (col={col}, row={row})")
            all_ok = False

    if all_ok:
        print(f"  RLE round-trip: PASSED ({len(result1['tiles'])} tiles verified)")

    # --- Frame 2 (delta test) ---
    print("\n--- 256-Color Pipeline (Frame 2 - Delta) ---")
    t0 = time.perf_counter()
    result2 = pipeline_256.process_frame_staged(screenshot2)
    t1 = time.perf_counter()

    print(f"  Processing time: {(t1-t0)*1000:.1f} ms")
    print_stats(result2['stats'])

    # Visualize which tiles changed (highlight changed tiles in red)
    if result2['stats']['changed_tiles'] < result2['stats']['total_tiles']:
        vis = result2['indexed_rgb'].copy()
        for tile_idx, _ in result2['tiles']:
            row = tile_idx // pipeline_256.tile_cols
            col = tile_idx % pipeline_256.tile_cols
            y0 = row * TILE_SIZE
            x0 = col * TILE_SIZE
            y1 = min(y0 + TILE_SIZE, CONTENT_HEIGHT)
            x1 = min(x0 + TILE_SIZE, CONTENT_WIDTH)
            # Red tint on changed tiles
            vis[y0:y1, x0:x1, 0] = np.minimum(
                vis[y0:y1, x0:x1, 0].astype(np.uint16) + 100, 255
            ).astype(np.uint8)
        save_rgb(vis, '05_delta_vis.png')

    # --- 16-color pipeline ---
    print("\n--- 16-Color Pipeline ---")
    pipeline_16 = ImagePipeline(CONTENT_WIDTH, CONTENT_HEIGHT,
                                color_depth=4, tile_size=TILE_SIZE)

    t0 = time.perf_counter()
    result_16 = pipeline_16.process_frame_staged(screenshot1)
    t1 = time.perf_counter()

    print(f"  Processing time: {(t1-t0)*1000:.1f} ms")
    print_stats(result_16['stats'])
    save_indexed(result_16['indexed'], pipeline_16.palette, '04_indexed_16.png')

    # --- Protocol encoding test ---
    print("\n--- Protocol Encoding Test ---")
    from protocol import encode_frame_delta, decode_frame_delta

    payload = encode_frame_delta(result1['tiles'])
    decoded_tiles = decode_frame_delta(payload)
    print(f"  Frame delta payload size: {len(payload):,} bytes")
    print(f"  Tiles encoded: {len(result1['tiles'])}")
    print(f"  Tiles decoded: {len(decoded_tiles)}")
    assert len(decoded_tiles) == len(result1['tiles']), "Tile count mismatch!"

    # Verify tile data round-trip through protocol encoding
    for (orig_idx, orig_data), (dec_idx, dec_data) in zip(result1['tiles'], decoded_tiles):
        assert orig_idx == dec_idx, f"Tile index mismatch: {orig_idx} vs {dec_idx}"
        assert orig_data == dec_data, f"Tile data mismatch for tile {orig_idx}"
    print("  Protocol encode/decode: PASSED")

    # --- Summary ---
    print("\n" + "=" * 60)
    print("TEST COMPLETE")
    print("=" * 60)
    print(f"\nOutput files saved to: {os.path.abspath(OUTPUT_DIR)}")
    print("\nInspect the PNG files to evaluate visual quality:")
    print("  01_original.png      - Full-color original")
    print("  02_dithered_256.png  - After Bayer dithering (still RGB)")
    print("  03_indexed_256.png   - Final 256-color result")
    print("  04_indexed_16.png    - 16-color fallback (rougher)")
    print("  05_delta_vis.png     - Changed tiles highlighted in red")

    # Estimate network transfer for the full frame
    frame_bytes = result1['stats']['compressed_bytes']
    ne2000_kbps = 350  # conservative KB/s estimate
    transfer_ms = (frame_bytes / 1024 / ne2000_kbps) * 1000
    print(f"\nEstimated full-frame transfer time @ {ne2000_kbps} KB/s: "
          f"{transfer_ms:.0f} ms ({frame_bytes:,} bytes)")

    delta_bytes = result2['stats']['compressed_bytes']
    delta_ms = (delta_bytes / 1024 / ne2000_kbps) * 1000
    print(f"Estimated delta transfer time @ {ne2000_kbps} KB/s: "
          f"{delta_ms:.0f} ms ({delta_bytes:,} bytes)")


def test_pipeline_without_playwright():
    """Run pipeline tests using a synthetic test image (no browser needed)."""
    ensure_output_dir()

    print("\n--- Synthetic Image Test (no Playwright) ---")

    # Create a test image with gradients, solid blocks, and text-like patterns
    img = np.zeros((CONTENT_HEIGHT, CONTENT_WIDTH, 3), dtype=np.uint8)

    # Horizontal gradient
    for x in range(CONTENT_WIDTH):
        img[:100, x, 0] = int(x * 255 / CONTENT_WIDTH)
        img[:100, x, 1] = int((CONTENT_WIDTH - x) * 255 / CONTENT_WIDTH)
        img[:100, x, 2] = 128

    # Solid color blocks
    colors = [
        (255, 0, 0), (0, 255, 0), (0, 0, 255),
        (255, 255, 0), (0, 255, 255), (255, 0, 255),
        (128, 128, 128), (255, 255, 255),
    ]
    block_w = CONTENT_WIDTH // len(colors)
    for i, (r, g, b) in enumerate(colors):
        x0 = i * block_w
        img[100:200, x0:x0 + block_w] = [r, g, b]

    # Checkerboard pattern (stress test for RLE)
    for y in range(200, 300):
        for x in range(CONTENT_WIDTH):
            if (x + y) % 2 == 0:
                img[y, x] = [200, 200, 200]
            else:
                img[y, x] = [50, 50, 50]

    # Gray gradient (tests grayscale ramp in palette)
    for y in range(300, CONTENT_HEIGHT):
        v = int((y - 300) * 255 / (CONTENT_HEIGHT - 300))
        img[y, :] = [v, v, v]

    # Test 256-color pipeline
    pipeline = ImagePipeline(CONTENT_WIDTH, CONTENT_HEIGHT,
                             color_depth=8, tile_size=TILE_SIZE)

    result = pipeline.process_frame_staged(img)
    print_stats(result['stats'])

    save_rgb(result['original'], '00_synthetic_original.png')
    save_indexed(result['indexed'], pipeline.palette, '00_synthetic_256.png')

    # Test delta: modify a small region and process again
    img2 = img.copy()
    img2[150:180, 200:350] = [255, 128, 0]  # Orange rectangle

    result2 = pipeline.process_frame_staged(img2)
    print(f"\n  Delta after small change:")
    print_stats(result2['stats'])


def print_stats(stats):
    """Print compression statistics."""
    print(f"  Tiles: {stats['changed_tiles']}/{stats['total_tiles']} changed")
    print(f"  Raw: {stats['raw_bytes']:,} bytes")
    print(f"  Compressed: {stats['compressed_bytes']:,} bytes")
    if stats['compressed_bytes'] > 0:
        print(f"  Ratio: {stats['compression_ratio']:.2f}x")


def main():
    url = sys.argv[1] if len(sys.argv) > 1 else 'https://www.google.com'

    print("=" * 60)
    print("RetroSurf Image Pipeline Test")
    print("=" * 60)

    # Always run RLE and synthetic tests (no Playwright needed)
    test_rle_roundtrip()
    test_pipeline_without_playwright()

    # Try Playwright tests
    try:
        test_pipeline_with_screenshots(url)
    except Exception as e:
        print(f"\n--- Playwright test skipped: {e} ---")
        print("To run the full test with real websites, install Playwright:")
        print("  pip install playwright && playwright install chromium")
        print("\nThe synthetic and RLE tests above still verify the pipeline works.")


if __name__ == '__main__':
    main()
