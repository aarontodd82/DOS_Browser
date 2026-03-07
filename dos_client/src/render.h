/*
 * RetroSurf Rendering - Tile decompression and framebuffer compositing
 *
 * Handles RLE decompression, XOR delta application, tile grid
 * management, and blitting tiles to the backbuffer.
 */

#ifndef RETROSURF_RENDER_H
#define RETROSURF_RENDER_H

#include <stdint.h>
#include "video.h"

#define TILE_SIZE    16
#define TILE_PIXELS  (TILE_SIZE * TILE_SIZE)  /* 256 bytes per tile at 8bpp */

/* Render context - manages tile grid and previous frame data */
typedef struct {
    uint16_t cols;           /* Tiles per row */
    uint16_t rows;           /* Tiles per column */
    uint16_t total;          /* Total tiles */
    uint8_t  *prev_tiles;   /* Previous frame tile data (total * TILE_PIXELS bytes) */
} RenderContext;

/* Initialize the render context for the given video config.
 * Allocates prev_tiles buffer.
 * Returns 0 on success, -1 on failure. */
int render_init(RenderContext *rc, VideoConfig *vc);

/* Apply a FRAME_FULL or FRAME_DELTA message payload.
 * Decompresses tiles, applies XOR delta, blits to backbuffer,
 * marks dirty rects. */
void render_apply_frame(RenderContext *rc, VideoConfig *vc,
                        const uint8_t *payload, uint16_t payload_len);

/* Shift prev_tiles by scroll_dy pixels (must be tile-aligned).
 * Called before applying FRAME_DELTA when header.reserved != 0.
 * Positive scroll_dy = scrolled down = content moved up = shift up. */
void render_shift_prev(RenderContext *rc, int16_t scroll_dy);

/* Reset all previous tile data (e.g., on navigation).
 * Next frame should be a FRAME_FULL. */
void render_reset(RenderContext *rc);

/* Free render context resources. */
void render_shutdown(RenderContext *rc);

#endif /* RETROSURF_RENDER_H */
