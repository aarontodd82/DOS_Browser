"""Fixed color palettes and precomputed LUT for fast RGB-to-palette mapping."""

import numpy as np


def build_256_palette():
    """Build the fixed 256-color palette.

    Layout:
      0-215:   6x6x6 RGB color cube
      216-239: 24-step grayscale ramp
      240-249: 10 web UI accent colors
      250-254: 5 DOS client chrome colors
      255:     Transparent/cursor key (magenta)
    """
    palette = np.zeros((256, 3), dtype=np.uint8)

    # 6x6x6 color cube (indices 0-215)
    idx = 0
    for r in range(6):
        for g in range(6):
            for b in range(6):
                palette[idx] = [r * 51, g * 51, b * 51]
                idx += 1

    # 24-step grayscale ramp (indices 216-239)
    for i in range(24):
        v = 8 + i * 10
        palette[216 + i] = [v, v, v]

    # 10 web UI accent colors (indices 240-249)
    web_colors = [
        [0, 102, 204],    # 240: Selection blue
        [0, 0, 238],      # 241: Link blue
        [204, 0, 0],      # 242: Error red
        [0, 153, 0],      # 243: Success green
        [255, 204, 0],    # 244: Warning yellow
        [192, 192, 192],  # 245: Border gray
        [128, 128, 128],  # 246: Shadow gray
        [240, 240, 240],  # 247: Light background
        [51, 51, 51],     # 248: Dark text
        [255, 255, 255],  # 249: White
    ]
    for i, color in enumerate(web_colors):
        palette[240 + i] = color

    # 5 DOS chrome colors (indices 250-254)
    palette[250] = [64, 64, 64]     # Chrome background
    palette[251] = [255, 255, 255]  # Chrome text
    palette[252] = [240, 240, 240]  # Address bar background
    palette[253] = [0, 0, 0]        # Address bar text
    palette[254] = [0, 120, 215]    # Highlight/focus

    # Transparent/key color (index 255)
    palette[255] = [255, 0, 255]    # Magenta

    return palette


def build_16_palette():
    """Build the standard VGA 16-color palette."""
    return np.array([
        [0,   0,   0],     # 0:  Black
        [0,   0,   170],   # 1:  Dark Blue
        [0,   170, 0],     # 2:  Dark Green
        [0,   170, 170],   # 3:  Dark Cyan
        [170, 0,   0],     # 4:  Dark Red
        [170, 0,   170],   # 5:  Dark Magenta
        [170, 85,  0],     # 6:  Brown
        [170, 170, 170],   # 7:  Light Gray
        [85,  85,  85],    # 8:  Dark Gray
        [85,  85,  255],   # 9:  Bright Blue
        [85,  255, 85],    # 10: Bright Green
        [85,  255, 255],   # 11: Bright Cyan
        [255, 85,  85],    # 12: Bright Red
        [255, 85,  255],   # 13: Bright Magenta
        [255, 255, 85],    # 14: Yellow
        [255, 255, 255],   # 15: White
    ], dtype=np.uint8)


def build_palette_lut(palette, bits=5):
    """Build a 3D lookup table for fast RGB -> palette index mapping.

    Quantizes the RGB color space into a (2^bits)^3 grid and precomputes
    the nearest palette entry for each grid cell. At runtime, mapping a
    pixel is just three right-shifts and an array lookup.

    Args:
        palette: (N, 3) uint8 array of RGB palette colors
        bits: bits per channel for LUT resolution (5 -> 32x32x32 = 32KB)

    Returns:
        (size, size, size) uint8 array where lut[r>>shift][g>>shift][b>>shift]
        gives the nearest palette index
    """
    size = 1 << bits
    step = 256 // size

    # Generate the center RGB value for each grid cell
    vals = np.arange(size, dtype=np.float32) * step + step // 2
    rr, gg, bb = np.meshgrid(vals, vals, vals, indexing='ij')
    grid = np.stack([rr, gg, bb], axis=-1).reshape(-1, 3)  # (size^3, 3)

    pal = palette.astype(np.float32)  # (N, 3)

    # Find nearest palette entry for each grid point
    # Process in chunks to keep memory usage reasonable
    lut_flat = np.empty(size ** 3, dtype=np.uint8)
    chunk_size = 2048
    for i in range(0, len(grid), chunk_size):
        chunk = grid[i:i + chunk_size]
        # Squared Euclidean distance: (chunk_size, N)
        dists = np.sum((chunk[:, None, :] - pal[None, :, :]) ** 2, axis=2)
        lut_flat[i:i + chunk_size] = np.argmin(dists, axis=1).astype(np.uint8)

    return lut_flat.reshape(size, size, size)


def apply_lut(img_rgb, lut, bits=5):
    """Map an RGB image to palette indices using the precomputed LUT.

    Args:
        img_rgb: (H, W, 3) uint8 RGB image
        lut: precomputed LUT from build_palette_lut()
        bits: must match the bits used to build the LUT

    Returns:
        (H, W) uint8 array of palette indices
    """
    shift = 8 - bits
    r = img_rgb[:, :, 0] >> shift
    g = img_rgb[:, :, 1] >> shift
    b = img_rgb[:, :, 2] >> shift
    return lut[r, g, b]


def palette_to_vga_dac(palette):
    """Convert palette to VGA DAC format (6-bit per channel, 768 bytes).

    VGA hardware uses 6-bit color values (0-63) per channel.
    This converts standard 8-bit (0-255) values to 6-bit.

    Args:
        palette: (N, 3) uint8 RGB palette

    Returns:
        bytes object of N*3 bytes (R, G, B for each entry, 6-bit values)
    """
    dac = (palette.astype(np.uint16) * 63 // 255).astype(np.uint8)
    return dac.tobytes()


def palette_to_rgb_bytes(palette):
    """Convert palette to flat RGB bytes for protocol transmission.

    Args:
        palette: (N, 3) uint8 RGB palette

    Returns:
        bytes object of N*3 bytes (R, G, B for each entry, 8-bit values)
    """
    return palette.tobytes()


def indexed_to_rgb(indexed, palette):
    """Convert a palette-indexed image back to RGB (for visualization/testing).

    Args:
        indexed: (H, W) uint8 palette index image
        palette: (N, 3) uint8 RGB palette

    Returns:
        (H, W, 3) uint8 RGB image
    """
    return palette[indexed]
