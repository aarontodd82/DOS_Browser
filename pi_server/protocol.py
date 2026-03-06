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

# --- Nav Actions ---
NAV_BACK = 0
NAV_FORWARD = 1
NAV_RELOAD = 2
NAV_STOP = 3

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

def encode_header(msg_type, flags, payload_len, sequence):
    """Encode an 8-byte message header."""
    return struct.pack(HEADER_FORMAT, msg_type, flags, payload_len, sequence, 0)


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


def encode_message(msg_type, payload, sequence, flags=0):
    """Encode a complete message (header + payload).

    Args:
        msg_type: message type constant
        payload: bytes payload (or empty bytes)
        sequence: sequence number
        flags: flag bits

    Returns:
        bytes of the complete message
    """
    header = encode_header(msg_type, flags, len(payload), sequence)
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


def encode_interaction_map(elements, page_scroll_y=0):
    """Encode INTERACTION_MAP payload.

    Args:
        elements: list of element dicts
        page_scroll_y: current page scroll position

    Returns:
        bytes of the interaction map payload
    """
    parts = [struct.pack('<HH', len(elements), page_scroll_y)]
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
