/*
 * RetroSurf Rendering - Tile decompression and framebuffer compositing
 *
 * Handles RLE decompression and blitting tiles to the backbuffer.
 * Tiles are raw palette-indexed pixels (no XOR delta encoding).
 */

#ifndef RETROSURF_RENDER_H
#define RETROSURF_RENDER_H

#include <stdint.h>
#include "video.h"

#define TILE_SIZE    16
#define TILE_PIXELS  (TILE_SIZE * TILE_SIZE)  /* 256 bytes per tile at 8bpp */

/* Render context - manages tile grid info */
typedef struct {
    uint16_t cols;           /* Tiles per row */
    uint16_t rows;           /* Tiles per column */
    uint16_t total;          /* Total tiles */
} RenderContext;

/* Initialize the render context for the given video config.
 * Returns 0 on success, -1 on failure. */
int render_init(RenderContext *rc, VideoConfig *vc);

/* Apply a FRAME_FULL or FRAME_DELTA message payload.
 * Decompresses tiles and blits to backbuffer, marks dirty rects. */
void render_apply_frame(RenderContext *rc, VideoConfig *vc,
                        const uint8_t *payload, uint16_t payload_len);

/* Free render context resources. */
void render_shutdown(RenderContext *rc);

#endif /* RETROSURF_RENDER_H */
