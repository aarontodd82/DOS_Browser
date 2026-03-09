/*
 * RetroSurf Scrollbar - Windows 3.1 style vertical scrollbar
 *
 * 16px wide with 3D beveled buttons and proportional thumb.
 * Uses the 6x6x6 RGB cube palette for grays.
 */

#include <string.h>
#include "scrollbar.h"
#include "video.h"

/* 6x6x6 palette colors: index = R*36 + G*6 + B (R,G,B = 0-5) */
#define SB_COL_FACE       172   /* (204,204,204) light gray - button/thumb face */
#define SB_COL_HIGHLIGHT  215   /* (255,255,255) white - 3D top/left edge */
#define SB_COL_SHADOW      86   /* (102,102,102) dark gray - 3D bottom/right */
#define SB_COL_DARKSHADOW   0   /* (0,0,0) black - outer edge */
#define SB_COL_TRACK      129   /* (153,153,153) medium gray - track bg */
#define SB_COL_ARROW        0   /* (0,0,0) black - arrow glyph */

/* ------------------------------------------------------------------ */
/* Draw a 3D raised rectangle (Windows 3.1 button style)              */
/* ------------------------------------------------------------------ */

static void draw_3d_rect(VideoConfig *vc, uint16_t x, uint16_t y,
                          uint16_t w, uint16_t h)
{
    uint8_t *buf = vc->backbuffer;
    uint16_t stride = vc->width;
    int r, c;

    /* Fill face */
    video_fill_rect(vc, x, y, w, h, SB_COL_FACE);

    /* Top highlight (white) */
    for (c = x; c < x + w - 1; c++)
        buf[y * stride + c] = SB_COL_HIGHLIGHT;
    /* Left highlight (white) */
    for (r = y; r < y + h - 1; r++)
        buf[r * stride + x] = SB_COL_HIGHLIGHT;

    /* Bottom shadow (dark gray) */
    for (c = x; c < x + w; c++)
        buf[(y + h - 1) * stride + c] = SB_COL_SHADOW;
    /* Right shadow (dark gray) */
    for (r = y; r < y + h; r++)
        buf[r * stride + (x + w - 1)] = SB_COL_SHADOW;

    /* Bottom-right outer pixel (black) */
    buf[(y + h - 1) * stride + (x + w - 1)] = SB_COL_DARKSHADOW;
}

/* ------------------------------------------------------------------ */
/* Draw arrow glyphs (5-row triangles)                                */
/* ------------------------------------------------------------------ */

static void draw_up_arrow(VideoConfig *vc, uint16_t cx, uint16_t cy)
{
    /* 5-row triangle centered at (cx, cy), pointing up.
     * Row 0: 1 pixel, row 1: 3, row 2: 5, row 3: 7, row 4: 9 */
    uint8_t *buf = vc->backbuffer;
    uint16_t stride = vc->width;
    int row, col;

    for (row = 0; row < 5; row++) {
        int half = row;
        int py = cy + row;
        for (col = -half; col <= half; col++) {
            int px = cx + col;
            if (px >= 0 && px < (int)vc->width && py >= 0 && py < (int)vc->height)
                buf[py * stride + px] = SB_COL_ARROW;
        }
    }
}

static void draw_down_arrow(VideoConfig *vc, uint16_t cx, uint16_t cy)
{
    uint8_t *buf = vc->backbuffer;
    uint16_t stride = vc->width;
    int row, col;

    for (row = 0; row < 5; row++) {
        int half = 4 - row;
        int py = cy + row;
        for (col = -half; col <= half; col++) {
            int px = cx + col;
            if (px >= 0 && px < (int)vc->width && py >= 0 && py < (int)vc->height)
                buf[py * stride + px] = SB_COL_ARROW;
        }
    }
}

/* ------------------------------------------------------------------ */
/* Public API                                                         */
/* ------------------------------------------------------------------ */

void scrollbar_init(ScrollbarState *sb, VideoConfig *vc)
{
    memset(sb, 0, sizeof(ScrollbarState));

    sb->x = vc->content_width;
    sb->y = vc->chrome_height;
    sb->width = vc->scrollbar_width;
    sb->height = vc->content_height;

    sb->track_y = sb->y + SCROLLBAR_ARROW_H;
    sb->track_h = sb->height - 2 * SCROLLBAR_ARROW_H;

    sb->viewport_h = vc->content_height;
    sb->doc_height = vc->content_height;  /* default: no scroll */
    sb->scroll_y = 0;

    /* Thumb fills entire track when doc == viewport */
    sb->thumb_y = 0;
    sb->thumb_h = sb->track_h;

    sb->needs_redraw = 1;
}

int scrollbar_update(ScrollbarState *sb, uint32_t doc_height,
                     uint32_t viewport_h, uint32_t scroll_y)
{
    uint16_t old_thumb_y = sb->thumb_y;
    uint16_t old_thumb_h = sb->thumb_h;

    sb->doc_height = doc_height;
    sb->viewport_h = viewport_h;
    sb->scroll_y = scroll_y;

    if (doc_height <= viewport_h || doc_height == 0) {
        /* No scrolling needed — thumb fills entire track */
        sb->thumb_h = sb->track_h;
        sb->thumb_y = 0;
    } else {
        uint32_t max_scroll = doc_height - viewport_h;

        /* Proportional thumb size */
        sb->thumb_h = (uint16_t)((uint32_t)sb->track_h * viewport_h
                                  / doc_height);
        if (sb->thumb_h < SCROLLBAR_MIN_THUMB)
            sb->thumb_h = SCROLLBAR_MIN_THUMB;
        if (sb->thumb_h > sb->track_h)
            sb->thumb_h = sb->track_h;

        /* Thumb position */
        if (scroll_y > max_scroll) scroll_y = max_scroll;
        {
            uint16_t travel = sb->track_h - sb->thumb_h;
            sb->thumb_y = (uint16_t)((uint32_t)travel * scroll_y
                                      / max_scroll);
        }
    }

    if (sb->thumb_y != old_thumb_y || sb->thumb_h != old_thumb_h) {
        sb->needs_redraw = 1;
        return 1;
    }
    return 0;
}

void scrollbar_draw(ScrollbarState *sb, VideoConfig *vc)
{
    uint16_t arrow_cx, arrow_cy;

    /* 1. Up arrow button (3D raised) */
    draw_3d_rect(vc, sb->x, sb->y, sb->width, SCROLLBAR_ARROW_H);
    arrow_cx = sb->x + sb->width / 2;
    arrow_cy = sb->y + (SCROLLBAR_ARROW_H - 5) / 2;
    draw_up_arrow(vc, arrow_cx, arrow_cy);

    /* 2. Down arrow button (3D raised) */
    {
        uint16_t down_y = sb->y + sb->height - SCROLLBAR_ARROW_H;
        draw_3d_rect(vc, sb->x, down_y, sb->width, SCROLLBAR_ARROW_H);
        arrow_cy = down_y + (SCROLLBAR_ARROW_H - 5) / 2;
        draw_down_arrow(vc, arrow_cx, arrow_cy);
    }

    /* 3. Track (medium gray background) */
    video_fill_rect(vc, sb->x, sb->track_y,
                    sb->width, sb->track_h, SB_COL_TRACK);

    /* 4. Thumb (3D raised, over track) */
    if (sb->thumb_h > 0 && sb->thumb_h < sb->track_h) {
        draw_3d_rect(vc, sb->x, sb->track_y + sb->thumb_y,
                     sb->width, sb->thumb_h);
    } else if (sb->thumb_h >= sb->track_h) {
        /* Thumb fills entire track — draw as raised face */
        draw_3d_rect(vc, sb->x, sb->track_y, sb->width, sb->track_h);
    }

    /* Mark scrollbar area dirty */
    video_mark_dirty(vc, sb->x, sb->y, sb->width, sb->height);

    sb->needs_redraw = 0;
}

int scrollbar_hit_test(ScrollbarState *sb, uint16_t mx, uint16_t my)
{
    uint16_t thumb_screen_y;

    if (!scrollbar_contains(sb, mx, my))
        return SB_HIT_NONE;

    /* Up arrow */
    if (my < sb->y + SCROLLBAR_ARROW_H)
        return SB_HIT_UP_ARROW;

    /* Down arrow */
    if (my >= sb->y + sb->height - SCROLLBAR_ARROW_H)
        return SB_HIT_DOWN_ARROW;

    /* Track area — check thumb position */
    thumb_screen_y = sb->track_y + sb->thumb_y;

    if (my < thumb_screen_y)
        return SB_HIT_TRACK_UP;

    if (my >= thumb_screen_y + sb->thumb_h)
        return SB_HIT_TRACK_DOWN;

    return SB_HIT_THUMB;
}

int scrollbar_contains(ScrollbarState *sb, uint16_t mx, uint16_t my)
{
    return (mx >= sb->x && mx < sb->x + sb->width &&
            my >= sb->y && my < sb->y + sb->height);
}
