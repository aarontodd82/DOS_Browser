/*
 * RetroSurf Protocol - Binary message encoding/decoding
 *
 * Mirrors pi_server/protocol.py exactly.
 * All multi-byte values are little-endian (native on x86).
 */

#ifndef RETROSURF_PROTOCOL_H
#define RETROSURF_PROTOCOL_H

#include <stdint.h>

/* --- Message Header (8 bytes) ---
 *   uint8_t  msg_type
 *   uint8_t  flags
 *   uint16_t payload_len
 *   uint16_t sequence
 *   int16_t  reserved
 */
#define HEADER_SIZE 8

/* --- Flags --- */
#define FLAG_COMPRESSED  0x01
#define FLAG_CONTINUED   0x02
#define FLAG_URGENT      0x04

/* --- Client -> Server Message Types --- */
#define MSG_CLIENT_HELLO  0x01
#define MSG_MOUSE_EVENT   0x10
#define MSG_KEY_EVENT     0x11
#define MSG_SCROLL_EVENT  0x12
#define MSG_TEXT_INPUT    0x13
#define MSG_NAVIGATE      0x14
#define MSG_NAV_ACTION    0x15
#define MSG_NATIVE_CLICK  0x16
#define MSG_ACK           0xF0
#define MSG_KEEPALIVE     0xF1

/* --- Server -> Client Message Types --- */
#define MSG_SERVER_HELLO    0x81
#define MSG_PALETTE         0x82
#define MSG_FRAME_FULL      0x83
#define MSG_FRAME_DELTA     0x84
#define MSG_INTERACTION_MAP 0x85
#define MSG_CURSOR_SHAPE    0x86
#define MSG_STATUS          0x87
#define MSG_INPUT_STATE     0x88
#define MSG_NATIVE_CONTENT  0x89
#define MSG_NATIVE_IMAGE    0x8A
#define MSG_MODE_SWITCH     0x8B
#define MSG_GLYPH_CACHE     0x8C
#define MSG_KEEPALIVE_ACK   0xF1

/* --- YouTube Mode Message Types --- */
/* Client -> Server */
#define MSG_YT_CONTROL    0x17
#define MSG_YT_ACK        0xF2

/* Server -> Client */
#define MSG_YT_START        0x90
#define MSG_YT_FRAME        0x91
#define MSG_YT_AUDIO        0x92   /* Phase 2 */
#define MSG_YT_EOF          0x93
#define MSG_YT_SEEK_RESULT  0x94   /* Phase 3 */

/* YouTube control actions */
#define YT_ACTION_PAUSE     0
#define YT_ACTION_RESUME    1
#define YT_ACTION_SEEK_FWD  2
#define YT_ACTION_SEEK_BACK 3
#define YT_ACTION_STOP      4

/* --- Nav Actions --- */
#define NAV_BACK         0
#define NAV_FORWARD      1
#define NAV_RELOAD       2
#define NAV_STOP         3
#define NAV_TOGGLE_MODE  4

/* --- Element Types --- */
#define ELEM_LINK              0x00
#define ELEM_BUTTON            0x01
#define ELEM_TEXT_INPUT        0x02
#define ELEM_TEXT_AREA         0x03
#define ELEM_PASSWORD          0x04
#define ELEM_CHECKBOX          0x05
#define ELEM_RADIO             0x06
#define ELEM_SELECT            0x07
#define ELEM_CONTENTEDITABLE   0x08
#define ELEM_CUSTOM_WIDGET     0x09

/* --- Mouse Event Types --- */
#define MOUSE_MOVE     0
#define MOUSE_CLICK    1
#define MOUSE_RELEASE  2
#define MOUSE_DBLCLICK 3

/* --- Cursor Types --- */
#define CURSOR_ARROW   0
#define CURSOR_HAND    1
#define CURSOR_TEXT    2
#define CURSOR_WAIT   3
#define CURSOR_CUSTOM  4

/* --- Packed Structures --- */
/* All structs are packed and little-endian (native x86) */

#pragma pack(push, 1)

typedef struct {
    uint8_t  msg_type;
    uint8_t  flags;
    uint16_t payload_len;
    uint16_t sequence;
    int16_t  reserved;
} msg_header_t;

typedef struct {
    uint16_t protocol_version;
    uint16_t screen_width;
    uint16_t screen_height;
    uint8_t  color_depth;
    uint8_t  tile_size;
    uint16_t chrome_height;
    uint16_t max_recv_buffer_kb;
    uint32_t session_id;
} client_hello_t;

typedef struct {
    uint16_t protocol_version;
    uint32_t session_id;
    uint16_t content_width;
    uint16_t content_height;
    uint8_t  tile_size;
    uint8_t  server_flags;
} server_hello_t;

typedef struct {
    uint16_t x;
    uint16_t y;
    uint8_t  buttons;
    uint8_t  event_type;
} mouse_event_t;

typedef struct {
    uint8_t scancode;
    uint8_t ascii;
    uint8_t modifiers;
    uint8_t event_type;
} key_event_t;

typedef struct {
    uint8_t direction;
    uint8_t amount;
} scroll_event_t;

/* Frame delta header - precedes tile entries */
typedef struct {
    uint16_t tile_count;
} frame_header_t;

/* Individual tile entry header in frame delta */
typedef struct {
    uint16_t tile_index;
    uint16_t comp_size;
    /* followed by comp_size bytes of compressed tile data */
} tile_entry_t;

/* YouTube start info (14 bytes + title) */
typedef struct {
    uint16_t video_w;
    uint16_t video_h;
    uint8_t  fps;
    uint16_t audio_rate;
    uint8_t  audio_bits;
    uint32_t duration_sec;
    uint16_t title_len;
    /* followed by title_len bytes of UTF-8 title */
} yt_start_t;

/* YouTube frame chunk header (10 bytes + block entries) */
typedef struct {
    uint32_t frame_num;
    uint32_t timestamp_ms;
    uint16_t block_count;
    /* followed by block entries: bx(u8) by(u8) comp_size(u16) rle_data */
} yt_frame_header_t;

/* Interaction map element */
typedef struct {
    uint16_t id;
    uint16_t x;
    uint16_t y;
    uint16_t w;
    uint16_t h;
    uint8_t  type;
    uint8_t  flags;
    uint8_t  font_size;
    uint8_t  text_color;
    uint8_t  bg_color;
    uint16_t value_len;
    /* followed by value_len bytes of UTF-8 text */
} interaction_elem_t;

#pragma pack(pop)

/* --- Sequence Counter --- */
extern uint16_t proto_sequence;

static inline uint16_t proto_next_seq(void) {
    return proto_sequence++;
}

/* --- Encoding Functions --- */

/* Encode a message header into buf (must be >= HEADER_SIZE bytes).
 * Returns HEADER_SIZE. */
int proto_encode_header(uint8_t *buf, uint8_t msg_type, uint8_t flags,
                        uint16_t payload_len, uint16_t sequence);

/* Encode CLIENT_HELLO payload into buf.
 * Returns payload size (16 bytes). */
int proto_encode_client_hello(uint8_t *buf, uint16_t screen_width,
                              uint16_t screen_height, uint8_t color_depth,
                              uint8_t tile_size, uint16_t chrome_height,
                              uint16_t max_recv_buffer_kb, uint32_t session_id);

/* Encode MOUSE_EVENT payload into buf.
 * Returns payload size (6 bytes). */
int proto_encode_mouse_event(uint8_t *buf, uint16_t x, uint16_t y,
                             uint8_t buttons, uint8_t event_type);

/* Encode KEY_EVENT payload into buf.
 * Returns payload size (4 bytes). */
int proto_encode_key_event(uint8_t *buf, uint8_t scancode, uint8_t ascii,
                           uint8_t modifiers, uint8_t event_type);

/* Encode SCROLL_EVENT payload into buf.
 * Returns payload size (2 bytes). */
int proto_encode_scroll_event(uint8_t *buf, uint8_t direction, uint8_t amount);

/* Encode NAVIGATE payload into buf (URL string, no null terminator).
 * Returns payload size. */
int proto_encode_navigate(uint8_t *buf, const char *url);

/* Encode NAV_ACTION payload into buf.
 * Returns payload size (1 byte). */
int proto_encode_nav_action(uint8_t *buf, uint8_t action);

/* Encode NATIVE_CLICK payload into buf.
 * Returns payload size (2 bytes). */
int proto_encode_native_click(uint8_t *buf, uint16_t link_id);

/* --- Decoding Functions --- */

/* Decode a message header from buf.
 * Returns 0 on success. */
int proto_decode_header(const uint8_t *buf, msg_header_t *header);

/* Decode SERVER_HELLO payload from buf. */
int proto_decode_server_hello(const uint8_t *buf, server_hello_t *hello);

#endif /* RETROSURF_PROTOCOL_H */
