/*
 * RetroSurf Native Renderer - Pre-positioned display list
 *
 * Renders pages natively using pre-positioned draw commands from
 * the server. The server extracts exact (x, y) coordinates from
 * the browser's CSS layout. The DOS client is a dumb renderer —
 * just blit at given coordinates, no word-wrap or layout.
 */

#ifndef RETROSURF_NATIVE_H
#define RETROSURF_NATIVE_H

#include <stdint.h>
#include "video.h"
#include "cursor.h"

/* Command tags (must match native_encoder.py) */
#define CMD_TEXT        0x01
#define CMD_LINK_RECT   0x02
#define CMD_IMAGE       0x03
#define CMD_RECT        0x04
#define CMD_END         0xFF

/* Text flags */
#define TEXT_BOLD       0x01
#define TEXT_ITALIC     0x02
#define TEXT_UNDERLINE  0x04

/* Maximum native links and images */
#define MAX_NATIVE_LINKS  512
#define MAX_NATIVE_IMAGES 64

/* Content buffer size (200KB) */
#define NATIVE_CONTENT_SIZE  204800

/* Image pool size (512KB) */
#define NATIVE_IMAGE_POOL    524288

/* Read little-endian uint16 from buffer */
#define READ_U16(buf, off) \
    ((uint16_t)(buf)[(off)] | ((uint16_t)(buf)[(off)+1] << 8))

/* Link bounding rectangle (document space) */
typedef struct {
    uint16_t link_id;
    uint16_t x, y, w, h;
} NativeLink;

/* Pre-dithered image */
typedef struct {
    uint16_t image_id;
    uint16_t width;
    uint16_t height;
    uint8_t  *pixels;           /* Points into image_pool */
} NativeImage;

/* Native rendering context */
typedef struct {
    uint8_t  *content_buf;      /* malloc'd command stream */
    uint32_t  content_len;      /* Bytes of command data received */
    uint32_t  content_capacity; /* Size of content_buf */
    uint8_t   bg_color;
    int32_t   scroll_y;         /* Current scroll offset (pixels) */
    int32_t   max_scroll_y;     /* Maximum scroll offset */
    uint8_t   needs_redraw;

    NativeLink links[MAX_NATIVE_LINKS];
    uint16_t   link_count;      /* Links found during render pass */

    NativeImage images[MAX_NATIVE_IMAGES];
    uint16_t    image_count;
    uint8_t    *image_pool;     /* malloc'd pool for decoded images */
    uint32_t    image_pool_used;

    uint16_t  viewport_w;
    uint16_t  viewport_h;
    uint16_t  content_top;      /* Y offset of content area (chrome_height) */
    uint8_t   active;           /* 1 = native mode active */

    /* Header fields from MSG_NATIVE_CONTENT */
    uint16_t  hdr_link_count;
    uint16_t  hdr_image_count;
    uint16_t  hdr_content_height;
    uint8_t   hdr_parsed;       /* 1 = header parsed */
} NativeCtx;

/* Initialize native context, malloc buffers.
 * Returns 0 on success, -1 on failure. */
int native_init(NativeCtx *ctx, VideoConfig *vc);

/* Reset for a new page (clear content, links, images). */
void native_reset(NativeCtx *ctx);

/* Parse incoming MSG_NATIVE_CONTENT payload.
 * Accumulates data; parses header on final chunk. */
void native_parse_content(NativeCtx *ctx, const uint8_t *payload,
                          uint16_t len, uint8_t flags);

/* Parse incoming MSG_NATIVE_IMAGE payload.
 * Decompresses image into image_pool. */
void native_parse_image(NativeCtx *ctx, const uint8_t *payload,
                        uint16_t len);

/* Render the content to the backbuffer. */
void native_render(NativeCtx *ctx, VideoConfig *vc);

/* Scroll by delta_y pixels. Clamps to valid range. */
void native_scroll(NativeCtx *ctx, int32_t delta_y);

/* Hit test screen coordinates against link rects.
 * Returns link_id or -1 if no link hit. */
int native_hit_test(NativeCtx *ctx, int16_t screen_x, int16_t screen_y);

/* Update cursor shape: hand over links, arrow otherwise. */
void native_update_cursor(NativeCtx *ctx, SoftCursor *cur,
                          int16_t sx, int16_t sy);

/* Free native context buffers. */
void native_shutdown(NativeCtx *ctx);

#endif /* RETROSURF_NATIVE_H */
