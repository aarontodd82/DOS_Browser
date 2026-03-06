/*
 * RetroSurf Video Subsystem - VESA/VGA mode setting and framebuffer
 *
 * Handles mode detection, backbuffer management, palette setting,
 * dirty rect tracking, and VGA flush (LFB or banked).
 */

#ifndef RETROSURF_VIDEO_H
#define RETROSURF_VIDEO_H

#include <stdint.h>

#define MAX_DIRTY_RECTS 64
#define CHROME_HEIGHT_640 24
#define CHROME_HEIGHT_800 28

typedef struct {
    uint16_t x, y, w, h;
} DirtyRect;

typedef struct {
    uint16_t width;              /* Screen width (640 or 800) */
    uint16_t height;             /* Screen height (480 or 600) */
    uint16_t content_width;      /* = width */
    uint16_t content_height;     /* = height - chrome_height - status_height */
    uint16_t chrome_height;      /* Pixels reserved for top chrome bar */
    uint16_t status_height;      /* Pixels reserved for bottom status bar */
    uint8_t  bpp;                /* 8 (256 color) or 4 (16 color) */
    uint16_t bytes_per_line;     /* From VBE mode info */
    uint8_t  has_lfb;            /* 1 if linear framebuffer available */
    uint32_t lfb_phys_addr;      /* Physical address of LFB */
    uint8_t  *lfb_ptr;           /* Mapped LFB pointer (near pointer) */
    uint8_t  *backbuffer;        /* System RAM backbuffer (malloc'd) */
    uint16_t vesa_mode;          /* VESA mode number (0 if standard VGA) */
    uint16_t bank_granularity;   /* Bank granularity in KB (banked modes) */
    uint16_t tile_cols;          /* Tiles per row in content area */
    uint16_t tile_rows;          /* Tiles per column in content area */
    uint16_t tile_total;         /* Total tiles in content area */

    /* Dirty rect tracking */
    DirtyRect dirty_rects[MAX_DIRTY_RECTS];
    int       dirty_count;
    int       full_flush;        /* If set, flush entire screen */
} VideoConfig;

/* Initialize video subsystem - auto-detect and set best mode.
 * preferred_mode: VIDMODE_xxx from config.h (0 = auto)
 * Returns 0 on success, -1 on failure. */
int video_init(VideoConfig *vc, uint8_t preferred_mode);

/* Set the VGA palette.
 * rgb: array of R,G,B bytes (8-bit each), count entries.
 * VGA DAC takes 6-bit values, so we shift >> 2. */
void video_set_palette(const uint8_t *rgb, int count);

/* Mark a rectangle as dirty (needs flush to VGA). */
void video_mark_dirty(VideoConfig *vc, uint16_t x, uint16_t y,
                      uint16_t w, uint16_t h);

/* Flush only dirty rectangles from backbuffer to VGA. */
void video_flush_dirty(VideoConfig *vc);

/* Flush entire backbuffer to VGA. */
void video_flush_full(VideoConfig *vc);

/* Fill a rectangle in the backbuffer with a solid color. */
void video_fill_rect(VideoConfig *vc, uint16_t x, uint16_t y,
                     uint16_t w, uint16_t h, uint8_t color);

/* Restore text mode and free resources. */
void video_shutdown(VideoConfig *vc);

#endif /* RETROSURF_VIDEO_H */
