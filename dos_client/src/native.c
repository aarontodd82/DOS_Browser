/*
 * RetroSurf Native Renderer — Pre-positioned display list
 *
 * Renders pages using pre-positioned draw commands from the server.
 * Each command carries exact (x, y) document-space coordinates
 * computed by the browser's CSS layout engine. The client simply
 * blits at screen_y = doc_y - scroll_y + content_top.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "native.h"
#include "font.h"
#include "video.h"
#include "protocol.h"
#include "render.h"  /* for RLE decompression constants */

/* ------------------------------------------------------------------ */
/* RLE decompression (same format as render.c)                        */
/* ------------------------------------------------------------------ */
static int rle_decompress(const uint8_t *src, uint16_t src_len,
                          uint8_t *dst, uint32_t dst_size)
{
    uint32_t si = 0, di = 0;
    while (si < src_len && di < dst_size) {
        uint8_t ctrl = src[si++];
        if (ctrl & 0x80) {
            /* Repeat run */
            uint8_t run_len = ctrl & 0x7F;
            uint8_t val = (si < src_len) ? src[si++] : 0;
            while (run_len-- && di < dst_size)
                dst[di++] = val;
        } else {
            /* Literal run */
            uint8_t lit_len = ctrl & 0x7F;
            while (lit_len-- && si < src_len && di < dst_size)
                dst[di++] = src[si++];
        }
    }
    return (int)di;
}

/* ------------------------------------------------------------------ */
/* Initialization / cleanup                                           */
/* ------------------------------------------------------------------ */

int native_init(NativeCtx *ctx, VideoConfig *vc)
{
    memset(ctx, 0, sizeof(NativeCtx));

    ctx->content_buf = (uint8_t *)malloc(NATIVE_CONTENT_SIZE);
    if (!ctx->content_buf) {
        printf("ERROR: native content_buf malloc failed\n");
        return -1;
    }
    ctx->content_capacity = NATIVE_CONTENT_SIZE;

    ctx->image_pool = (uint8_t *)malloc(NATIVE_IMAGE_POOL);
    if (!ctx->image_pool) {
        printf("ERROR: native image_pool malloc failed\n");
        free(ctx->content_buf);
        ctx->content_buf = NULL;
        return -1;
    }

    ctx->viewport_w = vc->content_width;
    ctx->viewport_h = vc->content_height;
    ctx->content_top = vc->chrome_height;

    return 0;
}

void native_reset(NativeCtx *ctx)
{
    ctx->content_len = 0;
    ctx->scroll_y = 0;
    ctx->max_scroll_y = 0;
    ctx->needs_redraw = 1;
    ctx->link_count = 0;
    ctx->image_count = 0;
    ctx->image_pool_used = 0;
    ctx->bg_color = 215;  /* white */
    ctx->hdr_parsed = 0;
    ctx->hdr_link_count = 0;
    ctx->hdr_image_count = 0;
    ctx->hdr_content_height = 0;
}

void native_shutdown(NativeCtx *ctx)
{
    if (ctx->content_buf) {
        free(ctx->content_buf);
        ctx->content_buf = NULL;
    }
    if (ctx->image_pool) {
        free(ctx->image_pool);
        ctx->image_pool = NULL;
    }
}

/* ------------------------------------------------------------------ */
/* Message parsing                                                    */
/* ------------------------------------------------------------------ */

void native_parse_content(NativeCtx *ctx, const uint8_t *payload,
                          uint16_t len, uint8_t flags)
{
    /* Accumulate payload data */
    if (ctx->content_len + len > ctx->content_capacity) {
        /* Truncate if too large */
        len = (uint16_t)(ctx->content_capacity - ctx->content_len);
    }
    if (len > 0) {
        memcpy(ctx->content_buf + ctx->content_len, payload, len);
        ctx->content_len += len;
    }

    /* If this is the last chunk, parse the 7-byte header */
    if (!(flags & FLAG_CONTINUED)) {
        if (ctx->content_len >= 7) {
            ctx->bg_color = ctx->content_buf[0];
            ctx->hdr_link_count = ctx->content_buf[1] |
                                  ((uint16_t)ctx->content_buf[2] << 8);
            ctx->hdr_image_count = ctx->content_buf[3] |
                                   ((uint16_t)ctx->content_buf[4] << 8);
            ctx->hdr_content_height = ctx->content_buf[5] |
                                      ((uint16_t)ctx->content_buf[6] << 8);
            ctx->hdr_parsed = 1;
        }
        ctx->scroll_y = 0;
        ctx->needs_redraw = 1;
    }
}

void native_parse_image(NativeCtx *ctx, const uint8_t *payload,
                        uint16_t len)
{
    uint16_t image_id, width, height;
    uint32_t comp_size, pixel_count;
    NativeImage *img;

    if (len < 10) return;

    /* Parse header: image_id(u16) + width(u16) + height(u16) + comp_size(u32) */
    image_id  = payload[0] | ((uint16_t)payload[1] << 8);
    width     = payload[2] | ((uint16_t)payload[3] << 8);
    height    = payload[4] | ((uint16_t)payload[5] << 8);
    comp_size = payload[6] | ((uint32_t)payload[7] << 8) |
                ((uint32_t)payload[8] << 16) | ((uint32_t)payload[9] << 24);

    if (ctx->image_count >= MAX_NATIVE_IMAGES) return;

    pixel_count = (uint32_t)width * height;
    if (ctx->image_pool_used + pixel_count > NATIVE_IMAGE_POOL) return;

    /* Decompress into image pool */
    img = &ctx->images[ctx->image_count];
    img->image_id = image_id;
    img->width = width;
    img->height = height;
    img->pixels = ctx->image_pool + ctx->image_pool_used;

    rle_decompress(payload + 10, (uint16_t)comp_size,
                   img->pixels, pixel_count);

    ctx->image_pool_used += pixel_count;
    ctx->image_count++;
}

/* ------------------------------------------------------------------ */
/* Find image by ID                                                   */
/* ------------------------------------------------------------------ */
static NativeImage *find_image(NativeCtx *ctx, uint16_t image_id)
{
    uint16_t i;
    for (i = 0; i < ctx->image_count; i++) {
        if (ctx->images[i].image_id == image_id)
            return &ctx->images[i];
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Rendering helpers                                                  */
/* ------------------------------------------------------------------ */

/* Store a link rect in document space */
static void add_link_rect(NativeCtx *ctx, uint16_t link_id,
                           uint16_t x, uint16_t y,
                           uint16_t w, uint16_t h)
{
    if (ctx->link_count >= MAX_NATIVE_LINKS) return;
    {
        NativeLink *lnk = &ctx->links[ctx->link_count++];
        lnk->link_id = link_id;
        lnk->x = x;
        lnk->y = y;
        lnk->w = w;
        lnk->h = h;
    }
}

/* Draw a single character with bold support (draw twice offset by 1) */
static void draw_char_styled(uint8_t *buf, int stride,
                              int x, int y, unsigned char ch,
                              uint8_t color, uint8_t bg,
                              int font_size, uint8_t flags)
{
    font_draw_char(buf, stride, x, y, ch, color, bg, font_size);
    if (flags & TEXT_BOLD) {
        font_draw_char(buf, stride, x + 1, y, ch, color, 255, font_size);
    }
}

/*
 * Visibility helper: checks if a screen Y coordinate is within the
 * content viewport [clip_top, clip_bottom).  font_draw_char has no
 * clipping, so we must NEVER call it with an out-of-bounds Y.
 */
#define VISIBLE(sy, h, clip_top, clip_bottom) \
    ((sy) >= (clip_top) && (sy) + (h) <= (clip_bottom))

/* ------------------------------------------------------------------ */
/* Rendering — pre-positioned display list                            */
/* ------------------------------------------------------------------ */

void native_render(NativeCtx *ctx, VideoConfig *vc)
{
    uint32_t cmd_offset;
    uint8_t *buf;
    int stride, scroll_y;
    int clip_top, clip_bottom;

    if (!ctx->hdr_parsed || !ctx->content_buf) return;

    ctx->needs_redraw = 0;
    ctx->link_count = 0;

    buf = vc->backbuffer;
    stride = vc->width;
    scroll_y = ctx->scroll_y;

    clip_top = (int)ctx->content_top;
    clip_bottom = clip_top + (int)ctx->viewport_h;

    /* Clear content area to background color */
    video_fill_rect(vc, 0, ctx->content_top,
                    vc->width, ctx->viewport_h, ctx->bg_color);

    /* Walk command stream (skip 7-byte header) */
    cmd_offset = 7;

    while (cmd_offset < ctx->content_len) {
        uint8_t tag = ctx->content_buf[cmd_offset++];

        if (tag == CMD_END) break;

        switch (tag) {
        case CMD_TEXT: {
            uint16_t x, y, text_len;
            uint8_t color, font, flags;
            const uint8_t *text;
            int sy, char_w, char_h, j, dx;

            if (cmd_offset + 9 > ctx->content_len) goto done;
            x = READ_U16(ctx->content_buf, cmd_offset); cmd_offset += 2;
            y = READ_U16(ctx->content_buf, cmd_offset); cmd_offset += 2;
            color = ctx->content_buf[cmd_offset++];
            font  = ctx->content_buf[cmd_offset++];
            flags = ctx->content_buf[cmd_offset++];
            text_len = READ_U16(ctx->content_buf, cmd_offset);
            cmd_offset += 2;

            if (cmd_offset + text_len > ctx->content_len) goto done;
            text = ctx->content_buf + cmd_offset;
            cmd_offset += text_len;

            if (font > 2) font = 1;
            char_w = font_char_width(font);
            char_h = font_char_height(font);

            sy = (int)y - scroll_y + clip_top;
            if (!VISIBLE(sy, char_h, clip_top, clip_bottom))
                break;

            dx = (int)x;
            for (j = 0; j < text_len; j++) {
                if (dx + char_w > (int)vc->width) break;
                if (dx >= 0) {
                    draw_char_styled(buf, stride, dx, sy,
                                     text[j], color, 255,
                                     font, flags);
                    if (flags & TEXT_UNDERLINE) {
                        int uy = sy + char_h - 1;
                        int ux;
                        for (ux = dx;
                             ux < dx + char_w &&
                             ux < (int)vc->width; ux++) {
                            buf[uy * stride + ux] = color;
                        }
                    }
                }
                dx += char_w;
            }
            break;
        }

        case CMD_LINK_RECT: {
            uint16_t link_id, x, y, w, h;

            if (cmd_offset + 10 > ctx->content_len) goto done;
            link_id = READ_U16(ctx->content_buf, cmd_offset);
            cmd_offset += 2;
            x = READ_U16(ctx->content_buf, cmd_offset); cmd_offset += 2;
            y = READ_U16(ctx->content_buf, cmd_offset); cmd_offset += 2;
            w = READ_U16(ctx->content_buf, cmd_offset); cmd_offset += 2;
            h = READ_U16(ctx->content_buf, cmd_offset); cmd_offset += 2;

            add_link_rect(ctx, link_id, x, y, w, h);
            break;
        }

        case CMD_IMAGE: {
            uint16_t image_id, x, y, w, h;
            NativeImage *img;
            int sy;

            if (cmd_offset + 10 > ctx->content_len) goto done;
            image_id = READ_U16(ctx->content_buf, cmd_offset);
            cmd_offset += 2;
            x = READ_U16(ctx->content_buf, cmd_offset); cmd_offset += 2;
            y = READ_U16(ctx->content_buf, cmd_offset); cmd_offset += 2;
            w = READ_U16(ctx->content_buf, cmd_offset); cmd_offset += 2;
            h = READ_U16(ctx->content_buf, cmd_offset); cmd_offset += 2;

            img = find_image(ctx, image_id);
            sy = (int)y - scroll_y + clip_top;

            if (img && img->pixels) {
                int row, col;
                for (row = 0; row < img->height; row++) {
                    int dy = sy + row;
                    if (dy < clip_top) continue;
                    if (dy >= clip_bottom) break;
                    for (col = 0; col < img->width; col++) {
                        int dx = (int)x + col;
                        if (dx >= 0 && dx < (int)vc->width) {
                            buf[dy * stride + dx] =
                                img->pixels[row * img->width + col];
                        }
                    }
                }
            }
            (void)w; (void)h;  /* placeholder dims unused for now */
            break;
        }

        case CMD_RECT: {
            uint16_t x, y, w, h;
            uint8_t color;
            int sy, row, col;

            if (cmd_offset + 9 > ctx->content_len) goto done;
            x = READ_U16(ctx->content_buf, cmd_offset); cmd_offset += 2;
            y = READ_U16(ctx->content_buf, cmd_offset); cmd_offset += 2;
            w = READ_U16(ctx->content_buf, cmd_offset); cmd_offset += 2;
            h = READ_U16(ctx->content_buf, cmd_offset); cmd_offset += 2;
            color = ctx->content_buf[cmd_offset++];

            sy = (int)y - scroll_y + clip_top;

            for (row = 0; row < (int)h; row++) {
                int dy = sy + row;
                if (dy < clip_top) continue;
                if (dy >= clip_bottom) break;
                for (col = 0; col < (int)w; col++) {
                    int dx = (int)x + col;
                    if (dx >= 0 && dx < (int)vc->width) {
                        buf[dy * stride + dx] = color;
                    }
                }
            }
            break;
        }

        default:
            goto done;
        }
    }

done:
    /* max_scroll_y from header's doc_height */
    ctx->max_scroll_y = (int32_t)ctx->hdr_content_height -
                        (int32_t)ctx->viewport_h;
    if (ctx->max_scroll_y < 0) ctx->max_scroll_y = 0;

    /* Clamp current scroll */
    if (ctx->scroll_y > ctx->max_scroll_y)
        ctx->scroll_y = ctx->max_scroll_y;

    /* Mark entire content area dirty */
    video_mark_dirty(vc, 0, ctx->content_top,
                     vc->width, ctx->viewport_h);
}

/* ------------------------------------------------------------------ */
/* Scrolling                                                          */
/* ------------------------------------------------------------------ */

void native_scroll(NativeCtx *ctx, int32_t delta_y)
{
    int32_t new_scroll = ctx->scroll_y + delta_y;
    if (new_scroll < 0) new_scroll = 0;
    if (new_scroll > ctx->max_scroll_y)
        new_scroll = ctx->max_scroll_y;
    if (new_scroll != ctx->scroll_y) {
        ctx->scroll_y = new_scroll;
        ctx->needs_redraw = 1;
    }
}

/* ------------------------------------------------------------------ */
/* Hit testing (document space)                                       */
/* ------------------------------------------------------------------ */

int native_hit_test(NativeCtx *ctx, int16_t screen_x, int16_t screen_y)
{
    int doc_x = (int)screen_x;
    int doc_y = (int)screen_y - (int)ctx->content_top +
                (int)ctx->scroll_y;
    uint16_t i;

    for (i = 0; i < ctx->link_count; i++) {
        NativeLink *lnk = &ctx->links[i];
        if (doc_x >= (int)lnk->x &&
            doc_x <  (int)lnk->x + (int)lnk->w &&
            doc_y >= (int)lnk->y &&
            doc_y <  (int)lnk->y + (int)lnk->h) {
            return lnk->link_id;
        }
    }
    return -1;
}

void native_update_cursor(NativeCtx *ctx, SoftCursor *cur,
                          int16_t sx, int16_t sy)
{
    int hit = native_hit_test(ctx, sx, sy);
    if (hit >= 0) {
        cursor_set_shape(cur, CURSOR_HAND);
    } else {
        cursor_set_shape(cur, CURSOR_ARROW);
    }
}
