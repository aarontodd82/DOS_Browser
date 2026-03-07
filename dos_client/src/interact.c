/*
 * RetroSurf Interaction Map - Hit testing, forwarding mode, cursor shapes
 */

#include <string.h>
#include "interact.h"

void interact_init(InteractCtx *ctx)
{
    memset(ctx, 0, sizeof(InteractCtx));
    ctx->mode = INTERACT_MODE_NONE;
    ctx->last_cursor = CURSOR_ARROW;
}

void interact_parse_map(InteractCtx *ctx, const uint8_t *payload,
                        uint16_t payload_len)
{
    uint16_t elem_count;
    uint16_t offset = 0;
    uint16_t value_len;
    int i;

    if (payload_len < 10) return;

    /* Header: element_count(u16), scroll_y(u32), scroll_height(u32) */
    elem_count = payload[0] | (payload[1] << 8);
    ctx->map.scroll_y = payload[2] | (payload[3] << 8)
                      | ((uint32_t)payload[4] << 16) | ((uint32_t)payload[5] << 24);
    ctx->map.scroll_height = payload[6] | (payload[7] << 8)
                           | ((uint32_t)payload[8] << 16) | ((uint32_t)payload[9] << 24);
    offset = 10;

    if (elem_count > MAX_INTERACT_ELEMS)
        elem_count = MAX_INTERACT_ELEMS;

    ctx->map.elem_count = elem_count;

    for (i = 0; i < elem_count && offset + 17 <= payload_len; i++) {
        InteractElem *e = &ctx->map.elems[i];

        /* Parse: id(u16), x(u16), y(u16), w(u16), h(u16),
         *        type(u8), flags(u8), font_size(u8),
         *        text_color(u8), bg_color(u8), value_len(u16) = 17 bytes */
        e->id         = payload[offset]     | (payload[offset + 1] << 8);
        e->x          = payload[offset + 2] | (payload[offset + 3] << 8);
        e->y          = payload[offset + 4] | (payload[offset + 5] << 8);
        e->w          = payload[offset + 6] | (payload[offset + 7] << 8);
        e->h          = payload[offset + 8] | (payload[offset + 9] << 8);
        e->type       = payload[offset + 10];
        e->flags      = payload[offset + 11];
        e->font_size  = payload[offset + 12];
        e->text_color = payload[offset + 13];
        e->bg_color   = payload[offset + 14];
        value_len     = payload[offset + 15] | (payload[offset + 16] << 8);
        offset += 17;

        /* Skip past value text (we don't store it in forwarding mode) */
        offset += value_len;
    }

    /* If forwarding and the element disappeared, deactivate */
    if (ctx->mode == INTERACT_MODE_FORWARD) {
        int found = 0;
        for (i = 0; i < ctx->map.elem_count; i++) {
            if (ctx->map.elems[i].id == ctx->forward_elem_id) {
                found = 1;
                break;
            }
        }
        if (!found) {
            ctx->mode = INTERACT_MODE_NONE;
            ctx->forward_elem_id = 0;
        }
    }
}

InteractElem *interact_hit_test(InteractCtx *ctx, uint16_t cx, uint16_t cy)
{
    int i;
    for (i = 0; i < ctx->map.elem_count; i++) {
        InteractElem *e = &ctx->map.elems[i];
        if (cx >= e->x && cx < e->x + e->w &&
            cy >= e->y && cy < e->y + e->h) {
            return e;
        }
    }
    return NULL;
}

void interact_handle_click(InteractCtx *ctx, InteractElem *elem,
                           net_context_t *net, uint16_t cx, uint16_t cy,
                           uint8_t buttons)
{
    uint8_t mbuf[6];
    int mlen;

    switch (elem->type) {
    case ELEM_TEXT_INPUT:
    case ELEM_TEXT_AREA:
    case ELEM_PASSWORD:
    case ELEM_CONTENTEDITABLE:
    case ELEM_CUSTOM_WIDGET:
        /* Enter forwarding mode - keys go to server */
        ctx->mode = INTERACT_MODE_FORWARD;
        ctx->forward_elem_id = elem->id;
        /* Send click so Chromium focuses the element */
        mlen = proto_encode_mouse_event(mbuf, cx, cy, buttons, MOUSE_CLICK);
        net_send_message(net, MSG_MOUSE_EVENT, mbuf, mlen);
        break;

    default:
        /* Links, buttons, checkboxes, etc. - just send click */
        mlen = proto_encode_mouse_event(mbuf, cx, cy, buttons, MOUSE_CLICK);
        net_send_message(net, MSG_MOUSE_EVENT, mbuf, mlen);
        break;
    }
}

void interact_handle_miss(InteractCtx *ctx, net_context_t *net,
                          uint16_t cx, uint16_t cy, uint8_t buttons)
{
    uint8_t mbuf[6];
    int mlen;

    /* Deactivate forwarding mode */
    interact_deactivate(ctx);

    /* Send click to server */
    mlen = proto_encode_mouse_event(mbuf, cx, cy, buttons, MOUSE_CLICK);
    net_send_message(net, MSG_MOUSE_EVENT, mbuf, mlen);
}

void interact_deactivate(InteractCtx *ctx)
{
    ctx->mode = INTERACT_MODE_NONE;
    ctx->forward_elem_id = 0;
}

/* Return appropriate cursor shape for an element type */
static uint8_t cursor_for_type(uint8_t type)
{
    switch (type) {
    case ELEM_LINK:
    case ELEM_BUTTON:
        return CURSOR_HAND;
    case ELEM_TEXT_INPUT:
    case ELEM_TEXT_AREA:
    case ELEM_PASSWORD:
    case ELEM_CONTENTEDITABLE:
        return CURSOR_TEXT;
    default:
        return CURSOR_ARROW;
    }
}

void interact_update_cursor(InteractCtx *ctx, SoftCursor *cur,
                            uint16_t cx, uint16_t cy)
{
    uint8_t shape;

    /* 0xFFFF = force arrow (mouse not in content area) */
    if (cx == 0xFFFF || cy == 0xFFFF) {
        shape = CURSOR_ARROW;
    } else {
        InteractElem *elem = interact_hit_test(ctx, cx, cy);
        shape = elem ? cursor_for_type(elem->type) : CURSOR_ARROW;
    }

    /* Only update if shape changed */
    if (shape != ctx->last_cursor) {
        cursor_set_shape(cur, shape);
        ctx->last_cursor = shape;
    }
}
