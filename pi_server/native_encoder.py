"""Native content encoder for RetroSurf v3.

Converts the pre-positioned draw commands from content_extractor.py into
a binary command stream. Also generates glyph caches for proportional
font rendering, and handles image downloading/dithering.

New in v3:
  - Server-generated glyph cache (proportional bitmap fonts like Win 3.1)
  - CMD_BG_TILE for tiled background images
  - Per-element backgrounds and table borders (encoded as CMD_RECT)
  - Font variant mapping (size/family/bold -> variant_id)
"""

import struct
import io
import os
import asyncio

import numpy as np
from PIL import Image, ImageFont, ImageDraw

from image_pipeline import rle_compress, apply_ordered_dither
from palette import build_256_palette, build_palette_lut, apply_lut

# Command tags (must match native.h on the client)
CMD_TEXT = 0x01
CMD_LINK_RECT = 0x02
CMD_IMAGE = 0x03
CMD_RECT = 0x04
CMD_BG_TILE = 0x06
CMD_END = 0xFF

# Text flags
TEXT_BOLD = 0x01
TEXT_ITALIC = 0x02
TEXT_UNDERLINE = 0x04
TEXT_STRIKETHROUGH = 0x08

# --- Non-ASCII transliteration table ---
# Maps Unicode/Latin-1 characters to ASCII approximations so that
# accented text on non-English pages remains readable on DOS.
_TRANSLITERATE = {
    # Latin-1 accented vowels
    '\u00c0': 'A', '\u00c1': 'A', '\u00c2': 'A', '\u00c3': 'A',
    '\u00c4': 'A', '\u00c5': 'A', '\u00c6': 'AE', '\u00c7': 'C',
    '\u00c8': 'E', '\u00c9': 'E', '\u00ca': 'E', '\u00cb': 'E',
    '\u00cc': 'I', '\u00cd': 'I', '\u00ce': 'I', '\u00cf': 'I',
    '\u00d0': 'D', '\u00d1': 'N', '\u00d2': 'O', '\u00d3': 'O',
    '\u00d4': 'O', '\u00d5': 'O', '\u00d6': 'O', '\u00d8': 'O',
    '\u00d9': 'U', '\u00da': 'U', '\u00db': 'U', '\u00dc': 'U',
    '\u00dd': 'Y', '\u00de': 'TH', '\u00df': 'ss',
    '\u00e0': 'a', '\u00e1': 'a', '\u00e2': 'a', '\u00e3': 'a',
    '\u00e4': 'a', '\u00e5': 'a', '\u00e6': 'ae', '\u00e7': 'c',
    '\u00e8': 'e', '\u00e9': 'e', '\u00ea': 'e', '\u00eb': 'e',
    '\u00ec': 'i', '\u00ed': 'i', '\u00ee': 'i', '\u00ef': 'i',
    '\u00f0': 'd', '\u00f1': 'n', '\u00f2': 'o', '\u00f3': 'o',
    '\u00f4': 'o', '\u00f5': 'o', '\u00f6': 'o', '\u00f8': 'o',
    '\u00f9': 'u', '\u00fa': 'u', '\u00fb': 'u', '\u00fc': 'u',
    '\u00fd': 'y', '\u00fe': 'th', '\u00ff': 'y',
    # Common symbols
    '\u00a0': ' ', '\u00a1': '!', '\u00a2': 'c', '\u00a3': 'L',
    '\u00a5': 'Y', '\u00a6': '|', '\u00a7': 'S', '\u00a9': '(c)',
    '\u00ab': '<<', '\u00ac': '-', '\u00ad': '-', '\u00ae': '(R)',
    '\u00b0': 'o', '\u00b1': '+/-', '\u00b2': '2', '\u00b3': '3',
    '\u00b4': "'", '\u00b5': 'u', '\u00b7': '.', '\u00b9': '1',
    '\u00bb': '>>', '\u00bc': '1/4', '\u00bd': '1/2',
    '\u00be': '3/4', '\u00bf': '?',
    '\u00d7': 'x', '\u00f7': '/',
    # Common Unicode punctuation
    '\u2013': '-', '\u2014': '--', '\u2018': "'", '\u2019': "'",
    '\u201a': ',', '\u201c': '"', '\u201d': '"', '\u201e': '"',
    '\u2022': '*', '\u2026': '...', '\u2122': '(TM)',
    '\u2020': '+', '\u2021': '++', '\u20ac': 'EUR',
    '\u2039': '<', '\u203a': '>',
    # Arrows and math
    '\u2190': '<-', '\u2192': '->', '\u2191': '^', '\u2193': 'v',
    '\u2260': '!=', '\u2264': '<=', '\u2265': '>=',
    '\u221e': 'oo', '\u2248': '~=',
}


def _transliterate(text):
    """Convert Unicode text to ASCII-safe representation.

    Maps accented characters, symbols, and common Unicode punctuation
    to their closest ASCII equivalents. Unknown characters become '?'.
    """
    result = []
    for ch in text:
        if ord(ch) < 128:
            result.append(ch)
        else:
            mapped = _TRANSLITERATE.get(ch)
            if mapped:
                result.append(mapped)
            else:
                result.append('?')
    return ''.join(result)

# --- Font discovery ---

# Common TTF paths by platform
_FONT_PATHS = {
    'sans': [
        # Windows
        'C:/Windows/Fonts/arial.ttf',
        'C:/Windows/Fonts/Arial.ttf',
        # Linux
        '/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf',
        '/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf',
        '/usr/share/fonts/TTF/DejaVuSans.ttf',
    ],
    'sans-bold': [
        'C:/Windows/Fonts/arialbd.ttf',
        'C:/Windows/Fonts/Arialbd.ttf',
        '/usr/share/fonts/truetype/liberation/LiberationSans-Bold.ttf',
        '/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf',
        '/usr/share/fonts/TTF/DejaVuSans-Bold.ttf',
    ],
    'serif': [
        'C:/Windows/Fonts/times.ttf',
        'C:/Windows/Fonts/Times.ttf',
        'C:/Windows/Fonts/timesnr.ttf',
        '/usr/share/fonts/truetype/liberation/LiberationSerif-Regular.ttf',
        '/usr/share/fonts/truetype/dejavu/DejaVuSerif.ttf',
    ],
    'serif-bold': [
        'C:/Windows/Fonts/timesbd.ttf',
        'C:/Windows/Fonts/Timesbd.ttf',
        '/usr/share/fonts/truetype/liberation/LiberationSerif-Bold.ttf',
        '/usr/share/fonts/truetype/dejavu/DejaVuSerif-Bold.ttf',
    ],
    'mono': [
        'C:/Windows/Fonts/cour.ttf',
        'C:/Windows/Fonts/Cour.ttf',
        '/usr/share/fonts/truetype/liberation/LiberationMono-Regular.ttf',
        '/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf',
    ],
    'mono-bold': [
        'C:/Windows/Fonts/courbd.ttf',
        'C:/Windows/Fonts/Courbd.ttf',
        '/usr/share/fonts/truetype/liberation/LiberationMono-Bold.ttf',
        '/usr/share/fonts/truetype/dejavu/DejaVuSansMono-Bold.ttf',
    ],
    'sans-italic': [
        'C:/Windows/Fonts/ariali.ttf',
        'C:/Windows/Fonts/Ariali.ttf',
        '/usr/share/fonts/truetype/liberation/LiberationSans-Italic.ttf',
        '/usr/share/fonts/truetype/dejavu/DejaVuSans-Oblique.ttf',
    ],
    'sans-bold-italic': [
        'C:/Windows/Fonts/arialbi.ttf',
        'C:/Windows/Fonts/Arialbi.ttf',
        '/usr/share/fonts/truetype/liberation/LiberationSans-BoldItalic.ttf',
        '/usr/share/fonts/truetype/dejavu/DejaVuSans-BoldOblique.ttf',
    ],
    'serif-italic': [
        'C:/Windows/Fonts/timesi.ttf',
        'C:/Windows/Fonts/Timesi.ttf',
        '/usr/share/fonts/truetype/liberation/LiberationSerif-Italic.ttf',
        '/usr/share/fonts/truetype/dejavu/DejaVuSerif-Italic.ttf',
    ],
    'serif-bold-italic': [
        'C:/Windows/Fonts/timesbi.ttf',
        'C:/Windows/Fonts/Timesbi.ttf',
        '/usr/share/fonts/truetype/liberation/LiberationSerif-BoldItalic.ttf',
        '/usr/share/fonts/truetype/dejavu/DejaVuSerif-BoldItalic.ttf',
    ],
    'mono-italic': [
        'C:/Windows/Fonts/couri.ttf',
        'C:/Windows/Fonts/Couri.ttf',
        '/usr/share/fonts/truetype/liberation/LiberationMono-Italic.ttf',
        '/usr/share/fonts/truetype/dejavu/DejaVuSansMono-Oblique.ttf',
    ],
    'mono-bold-italic': [
        'C:/Windows/Fonts/courbi.ttf',
        'C:/Windows/Fonts/Courbi.ttf',
        '/usr/share/fonts/truetype/liberation/LiberationMono-BoldItalic.ttf',
        '/usr/share/fonts/truetype/dejavu/DejaVuSansMono-BoldOblique.ttf',
    ],
}

_font_cache = {}  # key -> ImageFont


def _find_font(family, bold, italic=False):
    """Find a TTF font file for the given family/bold/italic combo."""
    # Build key like 'sans-bold-italic', 'serif-italic', 'mono', etc.
    key = family
    if bold and italic:
        key += '-bold-italic'
    elif bold:
        key += '-bold'
    elif italic:
        key += '-italic'

    if key in _font_cache:
        return _font_cache[key]

    paths = _FONT_PATHS.get(key, [])
    # Fallback chain: bold-italic -> bold -> italic -> regular
    if not paths or not any(os.path.exists(p) for p in paths):
        if bold and italic:
            paths = _FONT_PATHS.get(family + '-bold', [])
        elif italic:
            paths = _FONT_PATHS.get(family, [])
        elif bold:
            paths = _FONT_PATHS.get(family, [])
    if not paths or not any(os.path.exists(p) for p in paths):
        paths = _FONT_PATHS.get(family, [])

    for p in paths:
        if os.path.exists(p):
            _font_cache[key] = p
            return p

    # Last resort: try Pillow's default
    _font_cache[key] = None
    return None


def render_glyph_cache(font_variants):
    """Render glyph bitmaps for all font variants.

    Args:
        font_variants: set of (size_px, family, bold, italic) tuples
            (also accepts legacy 3-tuples without italic)

    Returns:
        tuple of (variants_list, variant_map)
        - variants_list: list of dicts for encode_glyph_cache()
        - variant_map: dict mapping (size, family, bold, italic) -> variant_id
    """
    variants_list = []
    variant_map = {}

    # Normalize to 4-tuples (handle legacy 3-tuples)
    normalized = set()
    for v in font_variants:
        if len(v) == 3:
            normalized.add((v[0], v[1], v[2], False))
        else:
            normalized.add(v)

    for idx, (size_px, family, bold, italic) in enumerate(
            sorted(normalized)):
        if idx >= 8:  # Cap at 8 variants
            break

        variant_id = idx
        variant_map[(size_px, family, bold, italic)] = variant_id

        font_path = _find_font(family, bold, italic)
        try:
            if font_path:
                font = ImageFont.truetype(font_path, size=size_px)
            else:
                # Pillow default font (bitmap, not great but works)
                font = ImageFont.load_default()
        except Exception:
            font = ImageFont.load_default()

        # Get font metrics using a reference string with ascenders + descenders
        ref_img = Image.new('L', (200, 100), 0)
        ref_draw = ImageDraw.Draw(ref_img)
        ref_bbox = ref_draw.textbbox((0, 0), 'Hgpqy', font=font)
        y_origin = ref_bbox[1]  # top of visual area (internal leading)
        line_height = ref_bbox[3] - ref_bbox[1]

        # Baseline: bottom of ascender chars relative to visual top
        asc_bbox = ref_draw.textbbox((0, 0), 'HXdfl', font=font)
        baseline = asc_bbox[3] - y_origin

        glyphs = []
        # Render printable ASCII (32-126)
        for char_code in range(32, 127):
            ch = chr(char_code)

            # Get advance width — must use getlength, NOT bbox width.
            # bbox width is the tight glyph bounding box and doesn't
            # include side bearings. getlength is the true advance.
            try:
                advance = font.getlength(ch)
            except AttributeError:
                bbox = font.getbbox(ch)
                advance = (bbox[2] - bbox[0]) if bbox else size_px // 3
            advance = max(1, int(round(advance)))

            # Get glyph bounding box for bitmap rendering
            glyph_bbox = ref_draw.textbbox((0, 0), ch, font=font)
            bmp_w = glyph_bbox[2] - glyph_bbox[0]
            bmp_h = glyph_bbox[3] - glyph_bbox[1]

            if bmp_w <= 0 or bmp_h <= 0:
                # Whitespace character — no bitmap, just advance
                glyphs.append({
                    'char_code': char_code,
                    'advance': advance,
                    'bmp_w': 0,
                    'bmp_h': 0,
                    'bmp_xoff': 0,
                    'bmp_yoff': 0,
                    'bitmap': b'',
                })
                continue

            # Cap bitmap dimensions
            bmp_w = min(bmp_w, 48)
            bmp_h = min(bmp_h, 48)

            glyph_img = Image.new('L', (bmp_w, bmp_h), 0)
            glyph_draw = ImageDraw.Draw(glyph_img)
            glyph_draw.text((-glyph_bbox[0], -glyph_bbox[1]),
                            ch, fill=255, font=font)

            # Pack to 1-bit, MSB first.
            # Use threshold of 80 (not 127) to include more of the
            # anti-aliased fringe pixels — prevents skeletal/jagged text.
            row_bytes = (bmp_w + 7) // 8
            packed = bytearray(row_bytes * bmp_h)
            pixels = glyph_img.tobytes()
            for y in range(bmp_h):
                for x in range(bmp_w):
                    if pixels[y * bmp_w + x] > 80:
                        byte_idx = y * row_bytes + x // 8
                        bit_idx = 7 - (x % 8)
                        packed[byte_idx] |= (1 << bit_idx)

            # Y offset relative to visual top of line (subtract y_origin)
            # so that the browser's line-top coordinate maps directly to
            # the DOS rendering position.
            glyphs.append({
                'char_code': char_code,
                'advance': advance,
                'bmp_w': bmp_w,
                'bmp_h': bmp_h,
                'bmp_xoff': int(glyph_bbox[0]),
                'bmp_yoff': int(glyph_bbox[1] - y_origin),
                'bitmap': bytes(packed),
            })

        variants_list.append({
            'variant_id': variant_id,
            'line_height': min(255, max(1, int(line_height))),
            'baseline': min(255, max(1, int(baseline))),
            'glyphs': glyphs,
        })

    return variants_list, variant_map


def encode_content(content_dict, variant_map):
    """Convert positioned draw commands into binary command stream.

    Args:
        content_dict: dict from content_extractor.extract_content()
        variant_map: dict mapping (size, family, bold) -> variant_id

    Returns:
        tuple of (command_stream_bytes, link_table)
        where link_table is [(link_id, href_url), ...]
    """
    stream = bytearray()
    link_table = []
    seen_links = set()

    for node in content_dict.get('nodes', []):
        ntype = node.get('type')

        if ntype == 'text':
            text = node.get('text', '')
            if not text:
                continue

            # Transliterate non-ASCII to ASCII approximations
            text = _transliterate(text)
            text_bytes = text.encode('ascii', errors='replace')
            if len(text_bytes) > 60000:
                text_bytes = text_bytes[:60000]

            # Map font to variant_id (4-tuple with italic)
            key = (node.get('font_size', 14),
                   node.get('font_family', 'sans'),
                   node.get('font_bold', False),
                   node.get('font_italic', False))
            font_id = variant_map.get(key, 0)

            # If exact variant not found, find closest
            if key not in variant_map and variant_map:
                best_id = 0
                best_dist = 9999
                for vk, vid in variant_map.items():
                    dist = abs(vk[0] - key[0])
                    if vk[1] != key[1]:
                        dist += 5
                    if vk[2] != key[2]:
                        dist += 3
                    if len(vk) > 3 and vk[3] != key[3]:
                        dist += 2
                    if dist < best_dist:
                        best_dist = dist
                        best_id = vid
                font_id = best_id

            text_width = min(65535, node.get('text_width', 0))

            stream.append(CMD_TEXT)
            stream.extend(struct.pack('<HH',
                                      node.get('x', 0),
                                      node.get('y', 0)))
            stream.append(node.get('color', 0) & 0xFF)
            stream.append(font_id & 0xFF)
            stream.append(node.get('flags', 0) & 0xFF)
            stream.extend(struct.pack('<H', text_width))
            stream.extend(struct.pack('<H', len(text_bytes)))
            stream.extend(text_bytes)

        elif ntype == 'link_rect':
            link_id = node.get('link_id', 0)
            href = node.get('href', '')
            if link_id not in seen_links:
                link_table.append((link_id, href))
                seen_links.add(link_id)

            stream.append(CMD_LINK_RECT)
            stream.extend(struct.pack('<HHHHH',
                                      link_id,
                                      node.get('x', 0),
                                      node.get('y', 0),
                                      node.get('w', 0),
                                      node.get('h', 0)))

        elif ntype == 'image':
            stream.append(CMD_IMAGE)
            stream.extend(struct.pack('<HHHHH',
                                      node.get('image_id', 0),
                                      node.get('x', 0),
                                      node.get('y', 0),
                                      node.get('width', 0),
                                      node.get('height', 0)))

        elif ntype in ('rect', 'border', 'bg_rect'):
            stream.append(CMD_RECT)
            stream.extend(struct.pack('<HHHH',
                                      node.get('x', 0),
                                      node.get('y', 0),
                                      node.get('w', 0),
                                      node.get('h', 0)))
            stream.append(node.get('color', 108) & 0xFF)

        elif ntype == 'bg_image':
            # Unprocessed bg_image (download failed or over cap) — skip
            pass

        elif ntype == 'bg_tile':
            # Processed bg tile with embedded RLE data
            rle_data = node.get('rle_data', b'')
            repeat = node.get('repeat', 'repeat').strip()
            # repeat mode: 0=repeat both, 1=repeat-x, 2=repeat-y, 3=no-repeat
            # Handle both single-value and two-value CSS syntax
            if repeat in ('no-repeat', 'no-repeat no-repeat'):
                repeat_mode = 3
            elif repeat in ('repeat-x', 'repeat no-repeat'):
                repeat_mode = 1
            elif repeat in ('repeat-y', 'no-repeat repeat'):
                repeat_mode = 2
            else:
                repeat_mode = 0

            stream.append(CMD_BG_TILE)
            stream.extend(struct.pack('<HHHHHH',
                                      node.get('x', 0),
                                      node.get('y', 0),
                                      node.get('w', 0),
                                      node.get('h', 0),
                                      node.get('tile_w', 0),
                                      node.get('tile_h', 0)))
            stream.append(repeat_mode)
            stream.extend(struct.pack('<H', len(rle_data)))
            stream.extend(rle_data)

    # End of stream
    stream.append(CMD_END)

    return bytes(stream), link_table


def palette_index_to_rgb(idx):
    """Convert a 6x6x6 palette index back to (r, g, b).

    Reverse of _rgb_to_palette_index in content_extractor.py.
    """
    _STEPS = [0, 51, 102, 153, 204, 255]
    if idx < 0 or idx > 215:
        return (255, 255, 255)  # fallback white
    b = idx % 6
    g = (idx // 6) % 6
    r = idx // 36
    return (_STEPS[r], _STEPS[g], _STEPS[b])


# Shared palette/LUT for image dithering (built once)
_palette = None
_lut = None


def _get_palette_lut():
    """Get or build the shared palette LUT for image dithering."""
    global _palette, _lut
    if _lut is None:
        _palette = build_256_palette()
        _lut = build_palette_lut(_palette)
    return _palette, _lut


async def process_native_image(page, image_src, image_id,
                                display_width=0, display_height=0,
                                max_width=600, max_height=800,
                                max_pixels=120000,
                                bg_rgb=(255, 255, 255)):
    """Download an image via the browser, dither it, and RLE-compress.

    Args:
        page: Playwright page object (for downloading via browser context)
        image_src: image URL
        image_id: unique image identifier
        display_width: browser's display width for this image
        display_height: browser's display height for this image
        max_width: absolute maximum width
        max_height: absolute maximum height
        max_pixels: maximum pixel count (w*h) to fit in DOS image pool
        bg_rgb: (r,g,b) tuple for compositing transparent images

    Returns:
        tuple of (image_id, width, height, rle_data) or None on failure
    """
    try:
        # Download image data via browser (handles cookies, relative URLs)
        img_data = await page.evaluate('''async (src) => {
            try {
                const resp = await fetch(src);
                const blob = await resp.blob();
                const reader = new FileReader();
                return await new Promise((resolve, reject) => {
                    reader.onload = () => {
                        const base64 = reader.result.split(',')[1];
                        resolve(base64);
                    };
                    reader.onerror = reject;
                    reader.readAsDataURL(blob);
                });
            } catch(e) {
                return null;
            }
        }''', image_src)

        if not img_data:
            return None

        import base64
        raw_bytes = base64.b64decode(img_data)
        img = Image.open(io.BytesIO(raw_bytes))

        # Handle transparency (GIF89a, PNG alpha) — composite on bg
        if img.mode in ('RGBA', 'LA') or \
                (img.mode == 'P' and 'transparency' in img.info):
            img = img.convert('RGBA')
            bg = Image.new('RGB', img.size, bg_rgb)
            bg.paste(img, mask=img.split()[3])  # alpha channel
            img = bg
        else:
            img = img.convert('RGB')

        # Use browser's display dimensions if provided
        if display_width > 0 and display_height > 0:
            w, h = display_width, display_height
        else:
            w, h = img.size

        # Clamp to max dimensions
        if w > max_width:
            h = int(h * max_width / w)
            w = max_width
        if h > max_height:
            w = int(w * max_height / h)
            h = max_height

        # Clamp to pixel budget (preserving aspect ratio)
        if w * h > max_pixels and w > 0 and h > 0:
            import math
            scale = math.sqrt(max_pixels / (w * h))
            w = max(1, int(w * scale))
            h = max(1, int(h * scale))

        w = max(1, w)
        h = max(1, h)

        # Resize to target dimensions
        if (w, h) != img.size:
            img = img.resize((w, h), Image.LANCZOS)

        # Convert to numpy, dither, apply palette LUT
        img_rgb = np.array(img, dtype=np.uint8)
        dithered = apply_ordered_dither(img_rgb, strength=32.0)
        _, lut = _get_palette_lut()
        indexed = apply_lut(dithered, lut)

        # RLE compress the indexed pixels
        pixel_data = indexed.tobytes()
        rle_data = rle_compress(pixel_data)

        return (image_id, w, h, rle_data)

    except Exception as e:
        print(f"[NativeEncoder] Image processing failed for {image_src}: {e}")
        return None


async def process_bg_tile(page, image_src, max_tile=64,
                          bg_rgb=(255, 255, 255)):
    """Download a background tile image, dither it, and RLE-compress.

    Args:
        page: Playwright page object
        image_src: tile image URL
        max_tile: max tile dimension (width or height)

    Returns:
        tuple of (width, height, rle_data) or None on failure
    """
    try:
        img_data = await page.evaluate('''async (src) => {
            try {
                const resp = await fetch(src);
                const blob = await resp.blob();
                const reader = new FileReader();
                return await new Promise((resolve, reject) => {
                    reader.onload = () => {
                        const base64 = reader.result.split(',')[1];
                        resolve(base64);
                    };
                    reader.onerror = reject;
                    reader.readAsDataURL(blob);
                });
            } catch(e) {
                return null;
            }
        }''', image_src)

        if not img_data:
            return None

        import base64
        raw_bytes = base64.b64decode(img_data)
        img = Image.open(io.BytesIO(raw_bytes))

        # Handle transparency (GIF89a, PNG alpha) — composite on bg
        if img.mode in ('RGBA', 'LA') or \
                (img.mode == 'P' and 'transparency' in img.info):
            img = img.convert('RGBA')
            bg = Image.new('RGB', img.size, bg_rgb)
            bg.paste(img, mask=img.split()[3])
            img = bg
        else:
            img = img.convert('RGB')

        w, h = img.size
        # Clamp tile to max dimensions
        if w > max_tile:
            h = int(h * max_tile / w)
            w = max_tile
        if h > max_tile:
            w = int(w * max_tile / h)
            h = max_tile
        w = max(1, w)
        h = max(1, h)

        if (w, h) != img.size:
            img = img.resize((w, h), Image.LANCZOS)

        img_rgb = np.array(img, dtype=np.uint8)
        dithered = apply_ordered_dither(img_rgb, strength=32.0)
        _, lut = _get_palette_lut()
        indexed = apply_lut(dithered, lut)

        rle_data = rle_compress(indexed.tobytes())
        return (w, h, rle_data)

    except Exception as e:
        print(f"[NativeEncoder] BG tile failed for {image_src}: {e}")
        return None


def build_native_payload(content_dict, variant_map, viewport_width=640):
    """Build the complete MSG_NATIVE_CONTENT payload.

    Args:
        content_dict: from content_extractor.extract_content()
        variant_map: from render_glyph_cache()
        viewport_width: unused (kept for API compat)

    Returns:
        tuple of (payload_bytes, link_table)
    """
    command_stream, link_table = encode_content(content_dict, variant_map)

    from protocol import encode_native_content
    payload = encode_native_content(
        bg_color=content_dict.get('bg_color', 215),
        link_count=content_dict.get('link_count', 0),
        image_count=content_dict.get('image_count', 0),
        content_height=content_dict.get('doc_height', 0),
        initial_scroll_y=content_dict.get('initial_scroll_y', 0),
        command_stream=command_stream,
    )

    return payload, link_table


def generate_alt_placeholder(alt_text, width, height, image_id):
    """Generate a placeholder image with alt text for failed downloads.

    Creates a small gray box with "[alt text]" rendered inside it,
    dithered to the palette. Used when an image fails to download.

    Args:
        alt_text: the image's alt attribute value
        width: display width from the command stream
        height: display height from the command stream
        image_id: unique image identifier

    Returns:
        tuple of (image_id, width, height, rle_data) or None
    """
    w = max(16, min(width, 300))
    h = max(16, min(height, 100))

    img = Image.new('RGB', (w, h), (220, 220, 220))
    draw = ImageDraw.Draw(img)

    # Draw 1px border
    draw.rectangle([0, 0, w - 1, h - 1], outline=(180, 180, 180))

    # Draw alt text (truncated, in brackets)
    label = f'[{alt_text[:40]}]' if alt_text else '[image]'
    try:
        font = ImageFont.load_default()
    except Exception:
        font = None
    draw.text((3, 3), label, fill=(100, 100, 100), font=font)

    # Dither and compress
    img_rgb = np.array(img, dtype=np.uint8)
    dithered = apply_ordered_dither(img_rgb, strength=32.0)
    _, lut = _get_palette_lut()
    indexed = apply_lut(dithered, lut)
    rle_data = rle_compress(indexed.tobytes())

    return (image_id, w, h, rle_data)
