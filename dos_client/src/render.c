/*
 * RetroSurf Rendering - Tile decompression and framebuffer compositing
 *
 * RLE format (from DESIGN.md Section 4.6):
 *   Control byte bit 7: 0=literal, 1=repeat
 *   Control byte bits 6-0: length (1-127)
 *   Literal: next <length> bytes are pixel values
 *   Repeat: next 1 byte repeated <length> times
 *
 * Tiles are raw palette-indexed pixels (no XOR delta).
 * Only changed tiles are sent by the server.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "render.h"

/* Temporary buffer for tile decoding */
static uint8_t tile_temp[TILE_PIXELS];

/* RLE decode compressed data into output buffer.
 * Returns number of bytes written to output. */
int rle_decode(const uint8_t *src, uint16_t comp_size,
               uint8_t *dst, int max_out)
{
    const uint8_t *end = src + comp_size;
    uint8_t *dst_start = dst;
    uint8_t *dst_end = dst + max_out;

    while (src < end && dst < dst_end) {
        uint8_t ctrl = *src++;
        uint8_t len = ctrl & 0x7F;

        if (len == 0) continue;

        if (ctrl & 0x80) {
            /* Repeat run */
            uint8_t val;
            if (src >= end) break;
            val = *src++;
            if (dst + len > dst_end) len = dst_end - dst;
            memset(dst, val, len);
            dst += len;
        } else {
            /* Literal run */
            if (src + len > end) len = end - src;
            if (dst + len > dst_end) len = dst_end - dst;
            memcpy(dst, src, len);
            src += len;
            dst += len;
        }
    }

    return dst - dst_start;
}

/* Decode a single tile: RLE decompress into tile_temp. */
static void decode_tile(const uint8_t *compressed, uint16_t comp_size)
{
    memset(tile_temp, 0, TILE_PIXELS);
    rle_decode(compressed, comp_size, tile_temp, TILE_PIXELS);
}

/* Blit a 16x16 tile from tile data to the backbuffer at (dst_x, dst_y) */
static void blit_tile(const uint8_t *tile, VideoConfig *vc,
                      uint16_t dst_x, uint16_t dst_y)
{
    int row;
    int max_rows = TILE_SIZE;
    int max_cols = TILE_SIZE;

    /* Clip to content area bounds (not screen edge) */
    {
        uint16_t content_bottom = vc->chrome_height + vc->content_height;
        if (dst_y + max_rows > content_bottom)
            max_rows = content_bottom - dst_y;
        if (dst_x + max_cols > vc->content_width)
            max_cols = vc->content_width - dst_x;
    }

    for (row = 0; row < max_rows; row++) {
        memcpy(vc->backbuffer + (dst_y + row) * vc->width + dst_x,
               tile + row * TILE_SIZE,
               max_cols);
    }
}

/* ---- Public API ---- */

int render_init(RenderContext *rc, VideoConfig *vc)
{
    rc->cols = vc->tile_cols;
    rc->rows = vc->tile_rows;
    rc->total = vc->tile_total;

    printf("Render: %ux%u tiles\n", rc->cols, rc->rows);

    return 0;
}

void render_apply_frame(RenderContext *rc, VideoConfig *vc,
                        const uint8_t *payload, uint16_t payload_len)
{
    uint16_t tile_count;
    const uint8_t *ptr;
    int i;

    if (payload_len < 2) return;

    /* Read tile count */
    memcpy(&tile_count, payload, 2);
    ptr = payload + 2;

    for (i = 0; i < tile_count; i++) {
        uint16_t tile_index, comp_size;
        uint16_t tile_col, tile_row;
        uint16_t dst_x, dst_y;

        /* Read tile entry header */
        if (ptr + 4 > payload + payload_len) break;
        memcpy(&tile_index, ptr, 2);
        ptr += 2;
        memcpy(&comp_size, ptr, 2);
        ptr += 2;

        /* Skip unchanged tiles */
        if (comp_size == 0) continue;

        /* Bounds check */
        if (tile_index >= rc->total) {
            ptr += comp_size;
            continue;
        }
        if (ptr + comp_size > payload + payload_len) break;

        /* Decode tile (RLE decompress — no XOR) */
        decode_tile(ptr, comp_size);

        /* Calculate screen position (content area starts below chrome) */
        tile_col = tile_index % rc->cols;
        tile_row = tile_index / rc->cols;
        dst_x = tile_col * TILE_SIZE;
        dst_y = tile_row * TILE_SIZE + vc->chrome_height;

        /* Blit tile to backbuffer */
        blit_tile(tile_temp, vc, dst_x, dst_y);

        /* Mark dirty for VGA flush */
        video_mark_dirty(vc, dst_x, dst_y, TILE_SIZE, TILE_SIZE);

        ptr += comp_size;
    }
}

void render_shutdown(RenderContext *rc)
{
    (void)rc;
}
