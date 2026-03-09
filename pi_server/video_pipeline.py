"""Video frame processing pipeline for YouTube mode.

Dithers RGB frames to the 6x6x6 palette, detects changed 8x8 blocks,
and RLE-compresses them for transmission to the DOS client.

Reuses palette LUT and dithering from the existing image pipeline.
"""

import numpy as np
from image_pipeline import apply_ordered_dither, rle_compress, BAYER_4x4
from palette import apply_lut


class VideoPipeline:
    """Processes raw RGB video frames into block-delta RLE data."""

    def __init__(self, width, height, palette, lut):
        """Initialize the video pipeline.

        Args:
            width: frame width (320)
            height: frame height (200)
            palette: (256, 3) uint8 RGB palette array (shared from ImagePipeline)
            lut: precomputed palette LUT (shared from ImagePipeline)
        """
        self.width = width
        self.height = height
        self.palette = palette
        self.lut = lut
        self.prev_indexed = None
        self.block_size = 8
        self.blocks_x = width // 8   # 40
        self.blocks_y = height // 8   # 25

    def process_frame(self, rgb_bytes):
        """Process a raw RGB24 frame into block-delta RLE.

        Args:
            rgb_bytes: bytes of width*height*3 RGB24 data from ffmpeg

        Returns:
            list of (bx, by, rle_bytes) for changed blocks
        """
        # Convert raw bytes to numpy array
        img = np.frombuffer(rgb_bytes, dtype=np.uint8).reshape(
            self.height, self.width, 3)

        # Bayer dither (same strength as screenshot pipeline)
        dithered = apply_ordered_dither(img, BAYER_4x4, 32.0)

        # Quantize to palette indices via LUT
        indexed = apply_lut(dithered, self.lut)

        # Find changed 8x8 blocks
        blocks = []
        bs = self.block_size

        for by in range(self.blocks_y):
            for bx in range(self.blocks_x):
                y0 = by * bs
                x0 = bx * bs

                block = indexed[y0:y0 + bs, x0:x0 + bs]

                if self.prev_indexed is not None:
                    prev_block = self.prev_indexed[y0:y0 + bs, x0:x0 + bs]
                    if np.array_equal(block, prev_block):
                        continue

                # RLE compress this block (row-major bytes)
                rle = rle_compress(block.tobytes())
                blocks.append((bx, by, rle))

        self.prev_indexed = indexed.copy()
        return blocks

    def reset(self):
        """Clear previous frame (forces full send on next frame)."""
        self.prev_indexed = None
