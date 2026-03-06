/*
 * RetroSurf Software Cursor
 *
 * Built-in cursor shapes: arrow, hand, ibeam, wait.
 * Each is a 16x16 bitmap with palette index 0 = transparent.
 * Uses palette indices 15 (white) and 0 (black) for the cursor.
 */

#include <string.h>
#include "cursor.h"

/* Built-in arrow cursor (16x16) */
/* Each row: hex bytes, 0=transparent, 15=white, 0x00=black-outline */
static const uint8_t arrow_shape[16 * 16] = {
    15, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    15,15, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    15,15,15, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    15,15,15,15, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    15,15,15,15,15, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    15,15,15,15,15,15, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    15,15,15,15,15,15,15, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    15,15,15,15,15,15,15,15, 0, 0, 0, 0, 0, 0, 0, 0,
    15,15,15,15,15,15,15,15,15, 0, 0, 0, 0, 0, 0, 0,
    15,15,15,15,15,15,15,15,15,15, 0, 0, 0, 0, 0, 0,
    15,15,15,15,15,15, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    15,15, 0,15,15, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    15, 0, 0, 0,15,15, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
     0, 0, 0, 0,15,15, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
     0, 0, 0, 0, 0,15,15, 0, 0, 0, 0, 0, 0, 0, 0, 0,
     0, 0, 0, 0, 0,15,15, 0, 0, 0, 0, 0, 0, 0, 0, 0,
};

/* Built-in hand cursor (16x16) - pointing finger */
static const uint8_t hand_shape[16 * 16] = {
     0, 0, 0, 0, 0,15,15, 0, 0, 0, 0, 0, 0, 0, 0, 0,
     0, 0, 0, 0,15, 8,15,15, 0, 0, 0, 0, 0, 0, 0, 0,
     0, 0, 0, 0,15, 8,15,15, 0, 0, 0, 0, 0, 0, 0, 0,
     0, 0, 0, 0,15, 8, 8,15, 0, 0, 0, 0, 0, 0, 0, 0,
     0, 0, 0, 0,15, 8, 8,15,15,15, 0, 0, 0, 0, 0, 0,
     0, 0, 0, 0,15, 8, 8,15, 8,15,15,15, 0, 0, 0, 0,
    15,15, 0,15,15, 8, 8,15, 8, 8,15, 8,15, 0, 0, 0,
    15, 8,15,15, 8, 8, 8, 8, 8, 8,15, 8,15, 0, 0, 0,
    15, 8, 8,15, 8, 8, 8, 8, 8, 8, 8, 8,15, 0, 0, 0,
     0,15, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8,15, 0, 0, 0,
     0, 0,15, 8, 8, 8, 8, 8, 8, 8, 8, 8,15, 0, 0, 0,
     0, 0,15, 8, 8, 8, 8, 8, 8, 8, 8,15, 0, 0, 0, 0,
     0, 0, 0,15, 8, 8, 8, 8, 8, 8, 8,15, 0, 0, 0, 0,
     0, 0, 0,15, 8, 8, 8, 8, 8, 8,15, 0, 0, 0, 0, 0,
     0, 0, 0, 0,15, 8, 8, 8, 8,15, 0, 0, 0, 0, 0, 0,
     0, 0, 0, 0,15,15,15,15,15,15, 0, 0, 0, 0, 0, 0,
};

/* Built-in I-beam cursor (16x16) - text selection */
static const uint8_t ibeam_shape[16 * 16] = {
     0, 0,15,15,15,15,15, 0, 0, 0, 0, 0, 0, 0, 0, 0,
     0, 0, 0, 0,15, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
     0, 0, 0, 0,15, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
     0, 0, 0, 0,15, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
     0, 0, 0, 0,15, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
     0, 0, 0, 0,15, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
     0, 0, 0, 0,15, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
     0, 0, 0, 0,15, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
     0, 0, 0, 0,15, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
     0, 0, 0, 0,15, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
     0, 0, 0, 0,15, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
     0, 0, 0, 0,15, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
     0, 0, 0, 0,15, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
     0, 0, 0, 0,15, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
     0, 0, 0, 0,15, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
     0, 0,15,15,15,15,15, 0, 0, 0, 0, 0, 0, 0, 0, 0,
};

/* Built-in wait cursor (16x16) - hourglass */
static const uint8_t wait_shape[16 * 16] = {
     0,15,15,15,15,15,15,15,15, 0, 0, 0, 0, 0, 0, 0,
     0,15, 8, 8, 8, 8, 8, 8,15, 0, 0, 0, 0, 0, 0, 0,
     0, 0,15, 8, 8, 8, 8,15, 0, 0, 0, 0, 0, 0, 0, 0,
     0, 0, 0,15, 8, 8,15, 0, 0, 0, 0, 0, 0, 0, 0, 0,
     0, 0, 0, 0,15, 8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
     0, 0, 0, 0,15,15, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
     0, 0, 0, 0,15,15, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
     0, 0, 0,15, 8, 8,15, 0, 0, 0, 0, 0, 0, 0, 0, 0,
     0, 0,15, 8, 8, 8, 8,15, 0, 0, 0, 0, 0, 0, 0, 0,
     0,15, 8, 8, 8, 8, 8, 8,15, 0, 0, 0, 0, 0, 0, 0,
     0,15,15,15,15,15,15,15,15, 0, 0, 0, 0, 0, 0, 0,
     0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
     0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
     0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
     0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
     0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
};

/* Shape table */
static const struct {
    const uint8_t *data;
    uint8_t w, h, hx, hy;
} builtin_shapes[] = {
    { arrow_shape, 16, 16, 0, 0 },    /* Arrow */
    { hand_shape,  16, 16, 5, 0 },    /* Hand (hotspot at finger tip) */
    { ibeam_shape, 16, 16, 4, 8 },    /* I-beam (hotspot at center) */
    { wait_shape,  16, 16, 4, 5 },    /* Wait/hourglass */
};

void cursor_init(SoftCursor *c)
{
    memset(c, 0, sizeof(SoftCursor));
    c->save_x = -1;
    c->save_y = -1;
    cursor_set_shape(c, 0);  /* Default arrow */
}

void cursor_set_shape(SoftCursor *c, uint8_t shape)
{
    if (shape > 3) shape = 0;
    c->shape = shape;
    c->width = builtin_shapes[shape].w;
    c->height = builtin_shapes[shape].h;
    c->hotspot_x = builtin_shapes[shape].hx;
    c->hotspot_y = builtin_shapes[shape].hy;
    memcpy(c->pixels, builtin_shapes[shape].data,
           c->width * c->height);
}

void cursor_set_custom(SoftCursor *c, uint8_t w, uint8_t h,
                       uint8_t hx, uint8_t hy, const uint8_t *data)
{
    if (w > CURSOR_MAX_SIZE) w = CURSOR_MAX_SIZE;
    if (h > CURSOR_MAX_SIZE) h = CURSOR_MAX_SIZE;
    c->shape = 4;  /* Custom */
    c->width = w;
    c->height = h;
    c->hotspot_x = hx;
    c->hotspot_y = hy;
    memcpy(c->pixels, data, w * h);
}

void cursor_restore(SoftCursor *c, uint8_t *backbuffer, int stride,
                    int screen_w, int screen_h)
{
    int row, col;
    int sx, sy, sw, sh;

    if (!c->visible) return;

    sx = c->save_x;
    sy = c->save_y;
    sw = c->save_w;
    sh = c->save_h;

    if (sx < 0 || sy < 0) return;

    /* Restore saved pixels */
    for (row = 0; row < sh; row++) {
        if (sy + row >= screen_h) break;
        for (col = 0; col < sw; col++) {
            if (sx + col >= screen_w) break;
            backbuffer[(sy + row) * stride + sx + col] =
                c->save_under[row * c->width + col];
        }
    }

    c->visible = 0;
}

void cursor_save_and_draw(SoftCursor *c, uint8_t *backbuffer, int stride,
                          int screen_w, int screen_h, int x, int y)
{
    int draw_x, draw_y;
    int clip_w, clip_h;
    int row, col;

    /* Calculate top-left corner of cursor bitmap */
    draw_x = x - c->hotspot_x;
    draw_y = y - c->hotspot_y;

    /* Clip to screen bounds */
    clip_w = c->width;
    clip_h = c->height;
    if (draw_x < 0) { draw_x = 0; clip_w = c->width - (0 - (x - c->hotspot_x)); }
    if (draw_y < 0) { draw_y = 0; clip_h = c->height - (0 - (y - c->hotspot_y)); }
    if (draw_x + clip_w > screen_w) clip_w = screen_w - draw_x;
    if (draw_y + clip_h > screen_h) clip_h = screen_h - draw_y;
    if (clip_w <= 0 || clip_h <= 0) return;

    /* Save position for restore */
    c->save_x = draw_x;
    c->save_y = draw_y;
    c->save_w = clip_w;
    c->save_h = clip_h;

    /* Save pixels under cursor */
    for (row = 0; row < clip_h; row++) {
        for (col = 0; col < clip_w; col++) {
            c->save_under[row * c->width + col] =
                backbuffer[(draw_y + row) * stride + draw_x + col];
        }
    }

    /* Draw cursor (skip transparent pixels = 0) */
    {
        int src_start_x = draw_x - (x - c->hotspot_x);
        int src_start_y = draw_y - (y - c->hotspot_y);
        for (row = 0; row < clip_h; row++) {
            for (col = 0; col < clip_w; col++) {
                uint8_t pixel = c->pixels[(src_start_y + row) * c->width +
                                          src_start_x + col];
                if (pixel != 0) {
                    backbuffer[(draw_y + row) * stride + draw_x + col] = pixel;
                }
            }
        }
    }

    c->visible = 1;
}
