"""Native content encoder for RetroSurf.

Converts the pre-positioned draw commands from content_extractor.py into
a binary command stream that the DOS client can render as a flat display
list. Also handles downloading and dithering images for native display.
"""

import struct
import io
import asyncio

import numpy as np
from PIL import Image

from image_pipeline import rle_compress, apply_ordered_dither
from palette import build_256_palette, build_palette_lut, apply_lut

# Command tags (must match native.h on the client)
CMD_TEXT = 0x01
CMD_LINK_RECT = 0x02
CMD_IMAGE = 0x03
CMD_RECT = 0x04
CMD_END = 0xFF

# Text flags
TEXT_BOLD = 0x01
TEXT_ITALIC = 0x02
TEXT_UNDERLINE = 0x04


def encode_content(content_dict):
    """Convert positioned draw commands into binary command stream.

    Args:
        content_dict: dict from content_extractor.extract_content()
            with keys: bg_color, nodes, link_count, image_count, doc_height

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

            # Encode text as ASCII (DOS can't display Unicode)
            text_bytes = text.encode('ascii', errors='replace')
            if len(text_bytes) > 60000:
                text_bytes = text_bytes[:60000]

            stream.append(CMD_TEXT)
            stream.extend(struct.pack('<HH',
                                      node.get('x', 0),
                                      node.get('y', 0)))
            stream.append(node.get('color', 0) & 0xFF)
            stream.append(node.get('font', 1) & 0xFF)
            stream.append(node.get('flags', 0) & 0xFF)
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

        elif ntype == 'rect':
            stream.append(CMD_RECT)
            stream.extend(struct.pack('<HHHH',
                                      node.get('x', 0),
                                      node.get('y', 0),
                                      node.get('w', 0),
                                      node.get('h', 0)))
            stream.append(node.get('color', 108) & 0xFF)

    # End of stream
    stream.append(CMD_END)

    return bytes(stream), link_table


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
                                max_width=600, max_height=400):
    """Download an image via the browser, dither it, and RLE-compress.

    Args:
        page: Playwright page object (for downloading via browser context)
        image_src: image URL
        image_id: unique image identifier
        max_width: maximum image width
        max_height: maximum image height

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
        img = Image.open(io.BytesIO(raw_bytes)).convert('RGB')

        # Resize to fit constraints while maintaining aspect ratio
        w, h = img.size
        if w > max_width:
            h = int(h * max_width / w)
            w = max_width
        if h > max_height:
            w = int(w * max_height / h)
            h = max_height
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


def build_native_payload(content_dict, viewport_width=640):
    """Build the complete MSG_NATIVE_CONTENT payload.

    Args:
        content_dict: from content_extractor.extract_content()
        viewport_width: unused (kept for API compat)

    Returns:
        tuple of (payload_bytes, link_table)
    """
    command_stream, link_table = encode_content(content_dict)

    from protocol import encode_native_content
    payload = encode_native_content(
        bg_color=content_dict.get('bg_color', 215),
        link_count=content_dict.get('link_count', 0),
        image_count=content_dict.get('image_count', 0),
        content_height=content_dict.get('doc_height', 0),
        command_stream=command_stream,
    )

    return payload, link_table
