/*
 * RetroSurf Scrollbar - Windows 3.1 style vertical scrollbar
 *
 * 16px wide, sits at the right edge of the content area.
 * Up/down arrow buttons, proportional thumb, track area.
 */

#ifndef RETROSURF_SCROLLBAR_H
#define RETROSURF_SCROLLBAR_H

#include <stdint.h>
#include "video.h"

#define SCROLLBAR_ARROW_H  16   /* Arrow button height (matches width) */
#define SCROLLBAR_MIN_THUMB 12  /* Minimum thumb height in pixels */

/* Hit test results */
#define SB_HIT_NONE        0
#define SB_HIT_UP_ARROW    1
#define SB_HIT_DOWN_ARROW  2
#define SB_HIT_TRACK_UP    3   /* Track above thumb = page up */
#define SB_HIT_TRACK_DOWN  4   /* Track below thumb = page down */
#define SB_HIT_THUMB       5

typedef struct {
    /* Position on screen */
    uint16_t x;              /* Left edge (= content_width) */
    uint16_t y;              /* Top edge (= chrome_height) */
    uint16_t width;          /* 16px */
    uint16_t height;         /* = content_height */

    /* Document state */
    uint32_t doc_height;     /* Total document height in pixels */
    uint32_t viewport_h;    /* Visible viewport height */
    uint32_t scroll_y;       /* Current scroll position */

    /* Computed thumb geometry (relative to track) */
    uint16_t track_y;       /* Screen Y of track top (after up arrow) */
    uint16_t track_h;       /* Track height (between arrows) */
    uint16_t thumb_y;       /* Thumb offset within track */
    uint16_t thumb_h;       /* Thumb height */

    uint8_t  needs_redraw;
} ScrollbarState;

/* Initialize scrollbar layout from video config. */
void scrollbar_init(ScrollbarState *sb, VideoConfig *vc);

/* Update state. Returns 1 if visual changed (needs redraw). */
int scrollbar_update(ScrollbarState *sb, uint32_t doc_height,
                     uint32_t viewport_h, uint32_t scroll_y);

/* Draw entire scrollbar to backbuffer. */
void scrollbar_draw(ScrollbarState *sb, VideoConfig *vc);

/* Hit test at screen coords. Returns SB_HIT_xxx. */
int scrollbar_hit_test(ScrollbarState *sb, uint16_t mx, uint16_t my);

/* Quick bounds check: is (mx,my) inside the scrollbar area? */
int scrollbar_contains(ScrollbarState *sb, uint16_t mx, uint16_t my);

#endif /* RETROSURF_SCROLLBAR_H */
