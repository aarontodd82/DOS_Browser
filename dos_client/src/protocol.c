/*
 * RetroSurf Protocol - Binary message encoding/decoding
 */

#include <string.h>
#include "protocol.h"

/* Global sequence counter */
uint16_t proto_sequence = 0;

int proto_encode_header(uint8_t *buf, uint8_t msg_type, uint8_t flags,
                        uint16_t payload_len, uint16_t sequence)
{
    msg_header_t *hdr = (msg_header_t *)buf;
    hdr->msg_type    = msg_type;
    hdr->flags       = flags;
    hdr->payload_len = payload_len;
    hdr->sequence    = sequence;
    hdr->reserved    = 0;
    return HEADER_SIZE;
}

int proto_encode_client_hello(uint8_t *buf, uint16_t screen_width,
                              uint16_t screen_height, uint8_t color_depth,
                              uint8_t tile_size, uint16_t chrome_height,
                              uint16_t max_recv_buffer_kb, uint32_t session_id)
{
    client_hello_t *hello = (client_hello_t *)buf;
    hello->protocol_version  = 1;
    hello->screen_width      = screen_width;
    hello->screen_height     = screen_height;
    hello->color_depth       = color_depth;
    hello->tile_size         = tile_size;
    hello->chrome_height     = chrome_height;
    hello->max_recv_buffer_kb = max_recv_buffer_kb;
    hello->session_id        = session_id;
    return sizeof(client_hello_t);
}

int proto_encode_mouse_event(uint8_t *buf, uint16_t x, uint16_t y,
                             uint8_t buttons, uint8_t event_type)
{
    mouse_event_t *evt = (mouse_event_t *)buf;
    evt->x          = x;
    evt->y          = y;
    evt->buttons    = buttons;
    evt->event_type = event_type;
    return sizeof(mouse_event_t);
}

int proto_encode_key_event(uint8_t *buf, uint8_t scancode, uint8_t ascii,
                           uint8_t modifiers, uint8_t event_type)
{
    key_event_t *evt = (key_event_t *)buf;
    evt->scancode   = scancode;
    evt->ascii      = ascii;
    evt->modifiers  = modifiers;
    evt->event_type = event_type;
    return sizeof(key_event_t);
}

int proto_encode_scroll_event(uint8_t *buf, uint8_t direction, uint8_t amount)
{
    scroll_event_t *evt = (scroll_event_t *)buf;
    evt->direction = direction;
    evt->amount    = amount;
    return sizeof(scroll_event_t);
}

int proto_encode_navigate(uint8_t *buf, const char *url)
{
    int len = strlen(url);
    memcpy(buf, url, len);
    return len;
}

int proto_encode_nav_action(uint8_t *buf, uint8_t action)
{
    buf[0] = action;
    return 1;
}

int proto_decode_header(const uint8_t *buf, msg_header_t *header)
{
    const msg_header_t *src = (const msg_header_t *)buf;
    header->msg_type    = src->msg_type;
    header->flags       = src->flags;
    header->payload_len = src->payload_len;
    header->sequence    = src->sequence;
    header->reserved    = src->reserved;
    return 0;
}

int proto_decode_server_hello(const uint8_t *buf, server_hello_t *hello)
{
    const server_hello_t *src = (const server_hello_t *)buf;
    hello->protocol_version = src->protocol_version;
    hello->session_id       = src->session_id;
    hello->content_width    = src->content_width;
    hello->content_height   = src->content_height;
    hello->tile_size        = src->tile_size;
    hello->server_flags     = src->server_flags;
    return 0;
}
