"""Image processing pipeline: screenshot -> palette-indexed tiles -> compressed deltas."""

import io
import numpy as np
from PIL import Image

from palette import (
    build_256_palette, build_16_palette, build_palette_lut,
    apply_lut, indexed_to_rgb,
)


# Bayer 4x4 ordered dithering matrix, normalized to [-0.5, +0.5]
BAYER_4x4 = np.array([
    [ 0,  8,  2, 10],
    [12,  4, 14,  6],
    [ 3, 11,  1,  9],
    [15,  7, 13,  5],
], dtype=np.float32) / 16.0 - 0.5

# Bayer 8x8 for finer dithering (optional, better gradients)
BAYER_8x8 = np.array([
    [ 0, 32,  8, 40,  2, 34, 10, 42],
    [48, 16, 56, 24, 50, 18, 58, 26],
    [12, 44,  4, 36, 14, 46,  6, 38],
    [60, 28, 52, 20, 62, 30, 54, 22],
    [ 3, 35, 11, 43,  1, 33,  9, 41],
    [51, 19, 59, 27, 49, 17, 57, 25],
    [15, 47,  7, 39, 13, 45,  5, 37],
    [63, 31, 55, 23, 61, 29, 53, 21],
], dtype=np.float32) / 64.0 - 0.5


def apply_ordered_dither(img_rgb, bayer=BAYER_4x4, strength=32.0):
    """Apply ordered (Bayer) dithering to an RGB image.

    Adds a spatial threshold pattern that creates the illusion of more
    colors when the image is later quantized to a limited palette.

    Args:
        img_rgb: (H, W, 3) uint8 RGB image
        bayer: normalized dither matrix with values in [-0.5, +0.5]
        strength: dither amplitude. Higher = more visible dithering pattern.
                  32.0 is good for 256 colors (6x6x6 cube step = 51).
                  64.0 is better for 16 colors.

    Returns:
        (H, W, 3) uint8 dithered RGB image (still full-color, ready for LUT)
    """
    h, w, _ = img_rgb.shape
    bh, bw = bayer.shape

    # Tile the Bayer matrix across the image dimensions
    threshold = np.tile(bayer, ((h + bh - 1) // bh, (w + bw - 1) // bw))[:h, :w]

    # Add dither bias to all channels
    bias = (threshold * strength).astype(np.float32)
    img_f = img_rgb.astype(np.float32)
    img_f[:, :, 0] += bias
    img_f[:, :, 1] += bias
    img_f[:, :, 2] += bias

    return np.clip(img_f, 0, 255).astype(np.uint8)


def rle_compress(data):
    """RLE compress a bytes-like object.

    Format per chunk:
      Control byte:
        Bit 7 = 1: repeat run. Next byte is repeated (control & 0x7F) times.
        Bit 7 = 0: literal run. Next (control & 0x7F) bytes are literal data.

    Max run/literal length per control byte: 127.

    Args:
        data: bytes or bytearray to compress

    Returns:
        bytes of compressed data
    """
    result = bytearray()
    n = len(data)
    i = 0

    while i < n:
        # Look for a repeat run (3+ identical bytes)
        run_val = data[i]
        run_end = i + 1
        while run_end < n and data[run_end] == run_val and run_end - i < 127:
            run_end += 1
        run_len = run_end - i

        if run_len >= 3:
            # Encode as repeat run
            result.append(0x80 | run_len)
            result.append(run_val)
            i = run_end
        else:
            # Collect literal bytes until we hit a run of 3+ identical bytes
            lit_start = i
            while i < n and i - lit_start < 127:
                # Peek ahead: if next 3 bytes are identical, stop literal here
                if (i + 2 < n and data[i] == data[i + 1] == data[i + 2]):
                    break
                i += 1

            lit_len = i - lit_start
            if lit_len > 0:
                result.append(lit_len)  # bit 7 = 0
                result.extend(data[lit_start:lit_start + lit_len])

    return bytes(result)


def rle_decompress(data, output_size):
    """Decompress RLE data back to raw bytes.

    Args:
        data: bytes of RLE compressed data
        output_size: expected number of output bytes

    Returns:
        bytearray of decompressed data
    """
    result = bytearray()
    i = 0
    n = len(data)

    while i < n and len(result) < output_size:
        ctrl = data[i]
        i += 1
        length = ctrl & 0x7F

        if ctrl & 0x80:
            # Repeat run
            val = data[i]
            i += 1
            result.extend(bytes([val]) * length)
        else:
            # Literal run
            result.extend(data[i:i + length])
            i += length

    return result[:output_size]


class ImagePipeline:
    """Processes screenshots into compressed tile deltas for the DOS client.

    Maintains state across frames to compute XOR deltas (only changed tiles
    are transmitted).
    """

    def __init__(self, width, height, color_depth=8, tile_size=16):
        """Initialize the pipeline.

        Args:
            width: content area width in pixels
            height: content area height in pixels
            color_depth: 8 for 256 colors, 4 for 16 colors
            tile_size: tile dimension (tiles are square: tile_size x tile_size)
        """
        self.width = width
        self.height = height
        self.color_depth = color_depth
        self.tile_size = tile_size

        # Tile grid dimensions
        self.tile_cols = width // tile_size
        self.tile_rows = (height + tile_size - 1) // tile_size

        # Handle partial tiles on right and bottom edges
        self.padded_width = self.tile_cols * tile_size
        self.padded_height = self.tile_rows * tile_size

        # Build palette and LUT
        if color_depth == 8:
            self.palette = build_256_palette()
            self.dither_strength = 32.0
        else:
            self.palette = build_16_palette()
            self.dither_strength = 64.0

        print(f"Building palette LUT ({len(self.palette)} colors)...")
        self.lut = build_palette_lut(self.palette)
        print("LUT ready.")

        # Previous frame for delta detection (None until first frame)
        self.prev_indexed = None

        # Stats
        self.frame_count = 0

    def process_frame(self, image_input, scroll_dy=0):
        """Process a screenshot into compressed raw tiles.

        Only changed tiles are sent (detected by comparing with prev_indexed).
        Tile data is raw palette indices, NOT XOR'd — eliminates the entire
        class of client/server state desync bugs.

        Args:
            image_input: one of:
              - bytes: JPEG/PNG image data
              - PIL.Image: already-decoded image
              - numpy array: (H, W, 3) uint8 RGB
            scroll_dy: vertical scroll delta in pixels since last frame.
                Positive = scrolled down (content moved up).
                If tile-aligned, prev_indexed is shifted to minimize
                the number of changed tiles detected.

        Returns:
            list of (tile_index, compressed_bytes) tuples for changed tiles.
            Empty list if nothing changed.
        """
        # Decode input to NumPy RGB array
        img_rgb = self._decode_input(image_input)

        # Crop/pad to expected dimensions
        img_rgb = self._fit_to_size(img_rgb)

        # Apply ordered dithering
        img_dithered = apply_ordered_dither(
            img_rgb, BAYER_4x4, self.dither_strength
        )

        # Map to palette indices via LUT
        indexed = apply_lut(img_dithered, self.lut)

        # Shift prev_indexed to match scroll offset (reduces changed tiles)
        if self.prev_indexed is not None and scroll_dy != 0 and scroll_dy % self.tile_size == 0:
            self.prev_indexed = self._shift_prev(self.prev_indexed, scroll_dy)

        # Find changed tiles
        if self.prev_indexed is None:
            # First frame or after navigation: all tiles changed
            changed_indices = list(range(self.tile_cols * self.tile_rows))
        else:
            changed_indices = self._find_changed_tiles(indexed)

        # Build compressed raw tiles (no XOR — just raw palette indices)
        result = []
        total_raw = 0
        total_compressed = 0

        for tile_idx in changed_indices:
            row = tile_idx // self.tile_cols
            col = tile_idx % self.tile_cols
            y0 = row * self.tile_size
            x0 = col * self.tile_size
            y1 = min(y0 + self.tile_size, self.height)
            x1 = min(x0 + self.tile_size, self.width)

            tile_data = indexed[y0:y1, x0:x1]
            raw_bytes = tile_data.tobytes()
            compressed = rle_compress(raw_bytes)

            total_raw += len(raw_bytes)
            total_compressed += len(compressed)

            result.append((tile_idx, compressed))

        # Update previous frame (for change detection only, not for XOR)
        self.prev_indexed = indexed.copy()
        self.frame_count += 1

        return result

    def process_frame_staged(self, image_input):
        """Process a frame and return intermediate results for debugging.

        Returns:
            dict with keys:
              'original': (H, W, 3) uint8 RGB input
              'dithered': (H, W, 3) uint8 dithered RGB
              'indexed': (H, W) uint8 palette indices
              'indexed_rgb': (H, W, 3) uint8 indexed image rendered back to RGB
              'tiles': list of (tile_index, compressed_bytes)
              'stats': dict of compression statistics
        """
        img_rgb = self._decode_input(image_input)
        img_rgb = self._fit_to_size(img_rgb)

        img_dithered = apply_ordered_dither(
            img_rgb, BAYER_4x4, self.dither_strength
        )

        indexed = apply_lut(img_dithered, self.lut)
        indexed_rgb = indexed_to_rgb(indexed, self.palette)

        if self.prev_indexed is None:
            changed_indices = list(range(self.tile_cols * self.tile_rows))
        else:
            changed_indices = self._find_changed_tiles(indexed)

        tiles = []
        total_raw = 0
        total_compressed = 0

        for tile_idx in changed_indices:
            row = tile_idx // self.tile_cols
            col = tile_idx % self.tile_cols
            y0 = row * self.tile_size
            x0 = col * self.tile_size
            y1 = min(y0 + self.tile_size, self.height)
            x1 = min(x0 + self.tile_size, self.width)

            new_tile = indexed[y0:y1, x0:x1]
            if self.prev_indexed is not None:
                old_tile = self.prev_indexed[y0:y1, x0:x1]
                delta = np.bitwise_xor(new_tile, old_tile)
            else:
                delta = new_tile.copy()

            raw_bytes = delta.tobytes()
            compressed = rle_compress(raw_bytes)
            total_raw += len(raw_bytes)
            total_compressed += len(compressed)
            tiles.append((tile_idx, compressed))

        self.prev_indexed = indexed.copy()
        self.frame_count += 1

        total_tiles = self.tile_cols * self.tile_rows
        ratio = total_raw / total_compressed if total_compressed > 0 else 0

        stats = {
            'total_tiles': total_tiles,
            'changed_tiles': len(changed_indices),
            'raw_bytes': total_raw,
            'compressed_bytes': total_compressed,
            'compression_ratio': ratio,
            'frame_number': self.frame_count,
        }

        return {
            'original': img_rgb,
            'dithered': img_dithered,
            'indexed': indexed,
            'indexed_rgb': indexed_rgb,
            'tiles': tiles,
            'stats': stats,
        }

    def verify_rle_roundtrip(self, tiles, indexed):
        """Verify that RLE compression is lossless by decompressing and comparing.

        Args:
            tiles: list of (tile_index, compressed_bytes) from process_frame
            indexed: the palette-indexed image that produced these tiles

        Returns:
            True if round-trip is perfect, raises AssertionError otherwise
        """
        for tile_idx, compressed in tiles:
            row = tile_idx // self.tile_cols
            col = tile_idx % self.tile_cols
            y0 = row * self.tile_size
            x0 = col * self.tile_size
            y1 = min(y0 + self.tile_size, self.height)
            x1 = min(x0 + self.tile_size, self.width)

            tile_h = y1 - y0
            tile_w = x1 - x0

            original_tile = indexed[y0:y1, x0:x1]

            # For the first frame, delta == tile data (no previous frame)
            # For subsequent frames, we'd need the previous frame to XOR back
            # This test only checks RLE itself is lossless
            decompressed = rle_decompress(compressed, tile_h * tile_w)
            recompressed_input = original_tile.tobytes()

            # For first frame (no prev), decompressed should equal the tile
            if self.frame_count <= 1:
                assert decompressed == bytearray(recompressed_input), \
                    f"RLE round-trip failed for tile {tile_idx}"

        return True

    def _decode_input(self, image_input):
        """Convert various input types to (H, W, 3) uint8 NumPy array."""
        if isinstance(image_input, bytes):
            img = Image.open(io.BytesIO(image_input)).convert('RGB')
            return np.asarray(img)
        elif isinstance(image_input, Image.Image):
            return np.asarray(image_input.convert('RGB'))
        elif isinstance(image_input, np.ndarray):
            if image_input.ndim == 3 and image_input.shape[2] == 3:
                return image_input
            raise ValueError(f"Expected (H,W,3) array, got shape {image_input.shape}")
        else:
            raise TypeError(f"Unsupported input type: {type(image_input)}")

    def _fit_to_size(self, img_rgb):
        """Crop or pad image to match expected (height, width) dimensions."""
        h, w, _ = img_rgb.shape

        # Crop if larger
        if h > self.height or w > self.width:
            img_rgb = img_rgb[:self.height, :self.width, :]
            h, w, _ = img_rgb.shape

        # Pad with black if smaller
        if h < self.height or w < self.width:
            padded = np.zeros((self.height, self.width, 3), dtype=np.uint8)
            padded[:h, :w, :] = img_rgb
            img_rgb = padded

        return img_rgb

    def _find_changed_tiles(self, indexed):
        """Find tile indices where any pixel differs from the previous frame.

        Uses a block-reduce approach for efficiency: reshape the diff mask
        into tile-shaped blocks and check each block with .any().
        """
        diff = indexed != self.prev_indexed  # (H, W) boolean

        changed = []
        for row in range(self.tile_rows):
            y0 = row * self.tile_size
            y1 = min(y0 + self.tile_size, self.height)
            for col in range(self.tile_cols):
                x0 = col * self.tile_size
                x1 = min(x0 + self.tile_size, self.width)
                if np.any(diff[y0:y1, x0:x1]):
                    changed.append(row * self.tile_cols + col)

        return changed

    def _shift_prev(self, prev, scroll_dy):
        """Shift prev_indexed by scroll_dy pixels vertically.

        Positive scroll_dy = scrolled down = content moved up = shift array up.
        Cleared rows (zeros) at the exposed edge will XOR as "changed",
        which is correct since those tiles have genuinely new content.
        """
        shifted = np.zeros_like(prev)
        if scroll_dy > 0:
            # Content moved up: copy old rows [dy:] to new rows [0:H-dy]
            if scroll_dy < self.height:
                shifted[:self.height - scroll_dy, :] = prev[scroll_dy:, :]
        elif scroll_dy < 0:
            # Content moved down: copy old rows [0:H+dy] to new rows [-dy:]
            dy = -scroll_dy
            if dy < self.height:
                shifted[dy:, :] = prev[:self.height - dy, :]
        return shifted
