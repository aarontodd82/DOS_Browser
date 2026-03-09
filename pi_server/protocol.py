"""Binary protocol encoding/decoding for RetroSurf client-server communication.

All multi-byte values are little-endian. Message format:
  8-byte header + variable-length payload.
"""

import struct

# --- Message Header ---
# uint8  msg_type
# uint8  flags
# uint16 payload_len
# uint16 sequence
# uint16 reserved
HEADER_FORMAT = '<BBHHh'
HEADER_SIZE = struct.calcsize(HEADER_FORMAT)  # 8 bytes

# --- Flags ---
FLAG_COMPRESSED = 0x01
FLAG_CONTINUED = 0x02
FLAG_URGENT = 0x04

# --- Client -> Server Message Types ---
MSG_CLIENT_HELLO = 0x01
MSG_MOUSE_EVENT = 0x10
MSG_KEY_EVENT = 0x11
MSG_SCROLL_EVENT = 0x12
MSG_TEXT_INPUT = 0x13
MSG_NAVIGATE = 0x14
MSG_NAV_ACTION = 0x15
MSG_ACK = 0xF0
MSG_KEEPALIVE = 0xF1

# --- Server -> Client Message Types ---
MSG_SERVER_HELLO = 0x81
MSG_PALETTE = 0x82
MSG_FRAME_FULL = 0x83
MSG_FRAME_DELTA = 0x84
MSG_INTERACTION_MAP = 0x85
MSG_CURSOR_SHAPE = 0x86
MSG_STATUS = 0x87
MSG_INPUT_STATE = 0x88
MSG_KEEPALIVE_ACK = 0xF1

# --- Native Rendering Message Types ---
# Client -> Server
MSG_NATIVE_CLICK = 0x16

# Server -> Client
MSG_NATIVE_CONTENT = 0x89
MSG_NATIVE_IMAGE = 0x8A
MSG_MODE_SWITCH = 0x8B
MSG_GLYPH_CACHE = 0x8C

# --- YouTube Mode Message Types ---
# Client -> Server
MSG_YT_CONTROL = 0x17
MSG_YT_ACK = 0xF2

# Server -> Client
MSG_YT_START = 0x90
MSG_YT_FRAME = 0x91
MSG_YT_AUDIO = 0x92   # Phase 2
MSG_YT_EOF = 0x93
MSG_YT_SEEK_RESULT = 0x94  # Phase 3

# YouTube control actions
YT_ACTION_PAUSE = 0
YT_ACTION_RESUME = 1
YT_ACTION_SEEK_FWD = 2
YT_ACTION_SEEK_BACK = 3
YT_ACTION_STOP = 4

# --- Nav Actions ---
NAV_BACK = 0
NAV_FORWARD = 1
NAV_RELOAD = 2
NAV_STOP = 3
NAV_TOGGLE_MODE = 4

# --- Element Types ---
ELEM_LINK = 0x00
ELEM_BUTTON = 0x01
ELEM_TEXT_INPUT = 0x02
ELEM_TEXT_AREA = 0x03
ELEM_PASSWORD = 0x04
ELEM_CHECKBOX = 0x05
ELEM_RADIO = 0x06
ELEM_SELECT = 0x07
ELEM_CONTENTEDITABLE = 0x08
ELEM_CUSTOM_WIDGET = 0x09

# --- Mouse Event Types ---
MOUSE_MOVE = 0
MOUSE_CLICK = 1
MOUSE_RELEASE = 2
MOUSE_DBLCLICK = 3

# --- Cursor Types ---
CURSOR_ARROW = 0
CURSOR_HAND = 1
CURSOR_TEXT = 2
CURSOR_WAIT = 3
CURSOR_CUSTOM = 4


class SequenceCounter:
    """Thread-safe monotonically increasing sequence number generator."""

    def __init__(self):
        self._value = 0

    def next(self):
        val = self._value
        self._value = (self._value + 1) & 0xFFFF
        return val


# --- Header Encoding/Decoding ---

def encode_header(msg_type, flags, payload_len, sequence, reserved=0):
    """Encode an 8-byte message header."""
    return struct.pack(HEADER_FORMAT, msg_type, flags, payload_len, sequence, reserved)


def decode_header(data):
    """Decode an 8-byte message header.

    Returns:
        dict with keys: msg_type, flags, payload_len, sequence
    """
    msg_type, flags, payload_len, sequence, _ = struct.unpack(HEADER_FORMAT, data[:HEADER_SIZE])
    return {
        'msg_type': msg_type,
        'flags': flags,
        'payload_len': payload_len,
        'sequence': sequence,
    }


def encode_message(msg_type, payload, sequence, flags=0, reserved=0):
    """Encode a complete message (header + payload).

    Args:
        msg_type: message type constant
        payload: bytes payload (or empty bytes)
        sequence: sequence number
        flags: flag bits
        reserved: int16 value for header reserved field (used for scroll_dy)

    Returns:
        bytes of the complete message
    """
    header = encode_header(msg_type, flags, len(payload), sequence, reserved)
    return header + payload


# --- Client -> Server Payloads ---

def encode_client_hello(protocol_version, screen_width, screen_height,
                        color_depth, tile_size, chrome_height,
                        max_recv_buffer_kb, session_id=0):
    """Encode CLIENT_HELLO payload (16 bytes)."""
    return struct.pack('<HHHBBHHI',
                       protocol_version,
                       screen_width,
                       screen_height,
                       color_depth,
                       tile_size,
                       chrome_height,
                       max_recv_buffer_kb,
                       session_id)


def decode_client_hello(data):
    """Decode CLIENT_HELLO payload."""
    fields = struct.unpack('<HHHBBHHI', data[:16])
    return {
        'protocol_version': fields[0],
        'screen_width': fields[1],
        'screen_height': fields[2],
        'color_depth': fields[3],
        'tile_size': fields[4],
        'chrome_height': fields[5],
        'max_recv_buffer_kb': fields[6],
        'session_id': fields[7],
    }


def encode_mouse_event(x, y, buttons, event_type):
    """Encode MOUSE_EVENT payload (6 bytes)."""
    return struct.pack('<HHBB', x, y, buttons, event_type)


def decode_mouse_event(data):
    """Decode MOUSE_EVENT payload."""
    x, y, buttons, event_type = struct.unpack('<HHBB', data[:6])
    return {
        'x': x,
        'y': y,
        'buttons': buttons,
        'event_type': event_type,
    }


def encode_key_event(scancode, ascii_val, modifiers, event_type):
    """Encode KEY_EVENT payload (4 bytes)."""
    return struct.pack('<BBBB', scancode, ascii_val, modifiers, event_type)


def decode_key_event(data):
    """Decode KEY_EVENT payload."""
    scancode, ascii_val, modifiers, event_type = struct.unpack('<BBBB', data[:4])
    return {
        'scancode': scancode,
        'ascii': ascii_val,
        'modifiers': modifiers,
        'event_type': event_type,
    }


def encode_scroll_event(direction, amount):
    """Encode SCROLL_EVENT payload (2 bytes)."""
    return struct.pack('<BB', direction, amount)


def decode_scroll_event(data):
    """Decode SCROLL_EVENT payload."""
    direction, amount = struct.unpack('<BB', data[:2])
    return {'direction': direction, 'amount': amount}


def encode_text_input(element_id, cursor_pos, text):
    """Encode TEXT_INPUT payload (6 + len bytes)."""
    text_bytes = text.encode('utf-8')
    header = struct.pack('<HHH', element_id, cursor_pos, len(text_bytes))
    return header + text_bytes


def decode_text_input(data):
    """Decode TEXT_INPUT payload."""
    element_id, cursor_pos, text_len = struct.unpack('<HHH', data[:6])
    text = data[6:6 + text_len].decode('utf-8', errors='replace')
    return {
        'element_id': element_id,
        'cursor_pos': cursor_pos,
        'text': text,
    }


def encode_navigate(url):
    """Encode NAVIGATE payload (URL string)."""
    return url.encode('utf-8')


def decode_navigate(data):
    """Decode NAVIGATE payload."""
    return data.decode('utf-8', errors='replace')


def encode_nav_action(action):
    """Encode NAV_ACTION payload (1 byte)."""
    return struct.pack('<B', action)


def decode_nav_action(data):
    """Decode NAV_ACTION payload."""
    return struct.unpack('<B', data[:1])[0]


# --- Server -> Client Payloads ---

def encode_server_hello(protocol_version, session_id, content_width,
                        content_height, tile_size, server_flags=0):
    """Encode SERVER_HELLO payload (12 bytes)."""
    return struct.pack('<HIHHBB',
                       protocol_version,
                       session_id,
                       content_width,
                       content_height,
                       tile_size,
                       server_flags)


def decode_server_hello(data):
    """Decode SERVER_HELLO payload."""
    fields = struct.unpack('<HIHHBB', data[:12])
    return {
        'protocol_version': fields[0],
        'session_id': fields[1],
        'content_width': fields[2],
        'content_height': fields[3],
        'tile_size': fields[4],
        'server_flags': fields[5],
    }


def encode_palette(palette_bytes):
    """Encode PALETTE payload (768 bytes for 256 colors, 48 for 16).

    Args:
        palette_bytes: flat RGB bytes from palette.palette_to_rgb_bytes()
    """
    return palette_bytes


def encode_frame_delta(tiles):
    """Encode FRAME_DELTA payload from a list of compressed tiles.

    Args:
        tiles: list of (tile_index, compressed_bytes) tuples

    Returns:
        bytes of the frame delta payload
    """
    parts = [struct.pack('<H', len(tiles))]  # tile count

    for tile_index, compressed in tiles:
        parts.append(struct.pack('<HH', tile_index, len(compressed)))
        parts.append(compressed)

    return b''.join(parts)


def decode_frame_delta(data):
    """Decode FRAME_DELTA payload.

    Returns:
        list of (tile_index, compressed_bytes) tuples
    """
    tile_count = struct.unpack('<H', data[:2])[0]
    tiles = []
    offset = 2

    for _ in range(tile_count):
        tile_index, comp_size = struct.unpack('<HH', data[offset:offset + 4])
        offset += 4
        compressed = data[offset:offset + comp_size]
        offset += comp_size
        tiles.append((tile_index, compressed))

    return tiles


def encode_interaction_element(elem):
    """Encode a single interactive element entry.

    Args:
        elem: dict with keys: id, x, y, w, h, type, flags, font_size,
              text_color, bg_color, value

    Returns:
        bytes for this element entry
    """
    value_bytes = elem.get('value', '').encode('utf-8')[:500]
    header = struct.pack('<HHHHHBBBBBH',
                         elem['id'],
                         elem['x'],
                         elem['y'],
                         elem['w'],
                         elem['h'],
                         elem['type'],
                         elem.get('flags', 0),
                         elem.get('font_size', 1),
                         elem.get('text_color', 248),
                         elem.get('bg_color', 249),
                         len(value_bytes))
    return header + value_bytes


def encode_interaction_map(elements, page_scroll_y=0, page_scroll_height=0):
    """Encode INTERACTION_MAP payload.

    Args:
        elements: list of element dicts
        page_scroll_y: current page scroll position
        page_scroll_height: total document scroll height

    Returns:
        bytes of the interaction map payload
    """
    parts = [struct.pack('<HII', len(elements), page_scroll_y, page_scroll_height)]
    for elem in elements:
        parts.append(encode_interaction_element(elem))
    return b''.join(parts)


def encode_status(text):
    """Encode STATUS payload (UTF-8 text string)."""
    return text.encode('utf-8')


def encode_cursor_shape(cursor_type, hotspot_x=0, hotspot_y=0,
                        width=0, height=0, pixels=b''):
    """Encode CURSOR_SHAPE payload.

    Args:
        cursor_type: CURSOR_ARROW, CURSOR_HAND, etc.
        hotspot_x, hotspot_y: click hotspot offset
        width, height: cursor bitmap dimensions (max 16x16)
        pixels: palette-indexed pixel data (0 = transparent)
    """
    header = struct.pack('<BBBBB', cursor_type, hotspot_x, hotspot_y,
                         width, height)
    return header + pixels


# --- Native Rendering Payloads ---

def encode_native_content(bg_color, link_count, image_count, content_height,
                          command_stream, initial_scroll_y=0):
    """Encode MSG_NATIVE_CONTENT payload.

    Args:
        bg_color: palette index for page background
        link_count: number of links in the content
        image_count: number of images in the content
        content_height: total content height in pixels
        initial_scroll_y: initial scroll offset for fragment anchors
        command_stream: binary command stream bytes

    Returns:
        bytes of the complete payload (9-byte header + commands)
    """
    header = struct.pack('<BHHHH', bg_color, link_count, image_count,
                         content_height, initial_scroll_y)
    return header + command_stream


def encode_native_image(image_id, width, height, rle_data):
    """Encode MSG_NATIVE_IMAGE payload.

    Args:
        image_id: unique image identifier
        width, height: image dimensions in pixels
        rle_data: RLE-compressed palette-indexed pixel data

    Returns:
        bytes of the complete payload (may need split_large_payload)
    """
    # compressed_size is u32 to handle large images
    header = struct.pack('<HHHI', image_id, width, height, len(rle_data))
    return header + rle_data


def encode_mode_switch(mode):
    """Encode MSG_MODE_SWITCH payload.

    Args:
        mode: 0=screenshot, 1=native

    Returns:
        1-byte payload
    """
    return struct.pack('<B', mode)


def encode_glyph_cache(variants):
    """Encode MSG_GLYPH_CACHE payload.

    Args:
        variants: list of dicts, each with:
            variant_id (int): 0-7
            line_height (int): pixel height of font line
            baseline (int): baseline offset from top
            glyphs: list of dicts with:
                char_code (int): ASCII code
                advance (int): proportional advance width
                bmp_w (int): bitmap width
                bmp_h (int): bitmap height
                bmp_xoff (int): x offset from cursor (signed)
                bmp_yoff (int): y offset from top (signed)
                bitmap (bytes): packed 1-bit data, MSB first

    Returns:
        bytes payload
    """
    parts = [struct.pack('<B', len(variants))]
    for var in variants:
        glyphs = var['glyphs']
        parts.append(struct.pack('<BBBB',
                                 var['variant_id'],
                                 var['line_height'],
                                 var['baseline'],
                                 len(glyphs)))
        for g in glyphs:
            parts.append(struct.pack('<BBBBbb',
                                     g['char_code'],
                                     g['advance'],
                                     g['bmp_w'],
                                     g['bmp_h'],
                                     g['bmp_xoff'],
                                     g['bmp_yoff']))
            parts.append(g['bitmap'])
    return b''.join(parts)


# --- YouTube Payloads ---

def encode_yt_start(video_w, video_h, fps, audio_rate, audio_bits,
                    duration_sec, title):
    """Encode MSG_YT_START payload.

    Args:
        video_w, video_h: video dimensions (320x200)
        fps: target frame rate
        audio_rate: audio sample rate (0 for silent)
        audio_bits: audio bit depth (0 for silent)
        duration_sec: video duration in seconds
        title: video title string (max 79 chars)

    Returns:
        bytes payload (14 + title_len bytes)
    """
    title_bytes = title.encode('utf-8')[:79]
    return struct.pack('<HHBHBIH',
                       video_w, video_h, fps,
                       audio_rate, audio_bits,
                       duration_sec, len(title_bytes)) + title_bytes


def encode_yt_frame_chunk(frame_num, timestamp_ms, blocks):
    """Encode a YouTube frame chunk payload.

    Each chunk is self-contained with its own block_count.
    Multiple chunks form a complete frame via FLAG_CONTINUED.

    Args:
        frame_num: frame sequence number
        timestamp_ms: playback timestamp in milliseconds
        blocks: list of (bx, by, rle_bytes) tuples

    Returns:
        bytes payload
    """
    parts = [struct.pack('<IIH', frame_num, timestamp_ms, len(blocks))]
    for bx, by, rle_data in blocks:
        parts.append(struct.pack('<BBH', bx, by, len(rle_data)))
        parts.append(rle_data)
    return b''.join(parts)


def decode_yt_control(data):
    """Decode MSG_YT_CONTROL payload."""
    return {'action': data[0]}


def decode_yt_ack(data):
    """Decode MSG_YT_ACK payload."""
    audio_buffer_ms = struct.unpack('<H', data[:2])[0]
    return {'audio_buffer_ms': audio_buffer_ms}


def decode_native_click(data):
    """Decode MSG_NATIVE_CLICK payload.

    Returns:
        dict with 'link_id'
    """
    link_id = struct.unpack('<H', data[:2])[0]
    return {'link_id': link_id}


# --- Message Splitting ---

def split_large_payload(msg_type, payload, sequence_counter, max_payload=60000):
    """Split a large payload into multiple messages with CONTINUED flag.

    Args:
        msg_type: message type for all fragments
        payload: full payload bytes
        sequence_counter: SequenceCounter instance
        max_payload: maximum payload size per message

    Returns:
        list of complete message bytes (header + payload each)
    """
    if len(payload) <= max_payload:
        return [encode_message(msg_type, payload, sequence_counter.next())]

    messages = []
    offset = 0
    while offset < len(payload):
        chunk = payload[offset:offset + max_payload]
        offset += len(chunk)

        flags = 0
        if offset < len(payload):
            flags |= FLAG_CONTINUED

        msg = encode_message(msg_type, chunk, sequence_counter.next(), flags)
        messages.append(msg)

    return messages
