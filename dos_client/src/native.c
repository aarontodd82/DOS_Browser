/*
 * RetroSurf Native Renderer v3 — Proportional fonts + backgrounds
 *
 * Server sends a glyph cache (proportional bitmap font data) and a
 * command stream with backgrounds, borders, text, images, link rects.
 * The client is a dumb renderer: look up glyphs from cache, blit at
 * the server-provided (x, y) positions.  No word-wrap, no layout.
 *
 * v3 changes:
 *   - Server-generated glyph cache with per-char advance widths
 *   - Per-element background rects (painter's algorithm)
 *   - Table border rects
 *   - Image scaling (nearest-neighbor) to match browser display size
 *   - Background tile support
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
/* RLE decompression — uses uint32_t for src_len to handle large      */
/* image data that may exceed 64KB after multi-chunk accumulation.     */
/* ------------------------------------------------------------------ */
static int rle_decompress_native(const uint8_t *src, uint32_t src_len,
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

    ctx->glyph_data_pool = (uint8_t *)malloc(GLYPH_DATA_POOL);
    if (!ctx->glyph_data_pool) {
        printf("ERROR: native glyph_data_pool malloc failed\n");
        free(ctx->content_buf);
        free(ctx->image_pool);
        ctx->content_buf = NULL;
        ctx->image_pool = NULL;
        return -1;
    }

    ctx->bg_tile_pool = (uint8_t *)malloc(BG_TILE_POOL);
    if (!ctx->bg_tile_pool) {
        printf("WARNING: bg_tile_pool malloc failed (non-fatal)\n");
    }

    ctx->img_recv_buf = (uint8_t *)malloc(IMG_RECV_BUF_SIZE);
    if (!ctx->img_recv_buf) {
        printf("WARNING: img_recv_buf malloc failed (non-fatal)\n");
    }
    ctx->img_recv_len = 0;
    ctx->img_recv_capacity = ctx->img_recv_buf ? IMG_RECV_BUF_SIZE : 0;

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
    ctx->scroll_pending_dy = 0;
    ctx->needs_redraw = 1;
    ctx->link_count = 0;
    ctx->image_count = 0;
    ctx->image_pool_used = 0;
    ctx->bg_color = 215;  /* white */
    ctx->hdr_parsed = 0;
    ctx->hdr_size = 9;
    ctx->hdr_link_count = 0;
    ctx->hdr_image_count = 0;
    ctx->hdr_content_height = 0;

    /* Keep glyph cache across page loads (font data reusable) */
    /* But reset bg tiles */
    ctx->bg_tile_count = 0;
    ctx->bg_tile_pool_used = 0;
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
    if (ctx->glyph_data_pool) {
        free(ctx->glyph_data_pool);
        ctx->glyph_data_pool = NULL;
    }
    if (ctx->bg_tile_pool) {
        free(ctx->bg_tile_pool);
        ctx->bg_tile_pool = NULL;
    }
    if (ctx->img_recv_buf) {
        free(ctx->img_recv_buf);
        ctx->img_recv_buf = NULL;
    }
}

/* ------------------------------------------------------------------ */
/* Glyph cache parsing                                                */
/* ------------------------------------------------------------------ */

void native_parse_glyph_cache(NativeCtx *ctx, const uint8_t *payload,
                               uint16_t len, uint8_t flags)
{
    uint32_t off = 0;
    uint8_t variant_count, i;

    /* Reset glyph cache for new page */
    ctx->glyph_variant_count = 0;
    ctx->glyph_data_used = 0;
    ctx->glyph_cache_ready = 0;
    memset(ctx->glyph_variants, 0, sizeof(ctx->glyph_variants));

    if (len < 1) return;

    variant_count = payload[off++];
    if (variant_count > MAX_GLYPH_VARIANTS)
        variant_count = MAX_GLYPH_VARIANTS;

    for (i = 0; i < variant_count && off + 4 <= len; i++) {
        GlyphVariant *var = &ctx->glyph_variants[i];
        uint8_t glyph_count, g;

        var->variant_id   = payload[off++];
        var->line_height  = payload[off++];
        var->baseline     = payload[off++];
        glyph_count       = payload[off++];

        memset(var->glyph_loaded, 0, sizeof(var->glyph_loaded));

        for (g = 0; g < glyph_count && off + 6 <= len; g++) {
            uint8_t char_code = payload[off++];
            uint8_t advance   = payload[off++];
            uint8_t bmp_w     = payload[off++];
            uint8_t bmp_h     = payload[off++];
            int8_t  bmp_xoff  = (int8_t)payload[off++];
            int8_t  bmp_yoff  = (int8_t)payload[off++];

            uint16_t row_bytes = (bmp_w + 7) / 8;
            uint32_t data_size = (uint32_t)row_bytes * bmp_h;

            if (char_code >= 32 && char_code < 128) {
                int idx = char_code - 32;
                GlyphInfo *gi = &var->glyphs[idx];
                gi->advance  = advance;
                gi->bmp_w    = bmp_w;
                gi->bmp_h    = bmp_h;
                gi->bmp_xoff = bmp_xoff;
                gi->bmp_yoff = bmp_yoff;

                if (data_size > 0 && off + data_size <= len &&
                    ctx->glyph_data_used + data_size <= GLYPH_DATA_POOL) {
                    gi->bitmap = ctx->glyph_data_pool + ctx->glyph_data_used;
                    memcpy(gi->bitmap, payload + off, data_size);
                    ctx->glyph_data_used += data_size;
                } else {
                    gi->bitmap = NULL;
                }
                var->glyph_loaded[idx] = 1;
            }

            off += data_size;
        }

        ctx->glyph_variant_count++;
    }

    ctx->glyph_cache_ready = 1;
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

    /* If this is the last chunk, parse the 9-byte header */
    if (!(flags & FLAG_CONTINUED)) {
        if (ctx->content_len >= 9) {
            ctx->bg_color = ctx->content_buf[0];
            ctx->hdr_link_count = ctx->content_buf[1] |
                                  ((uint16_t)ctx->content_buf[2] << 8);
            ctx->hdr_image_count = ctx->content_buf[3] |
                                   ((uint16_t)ctx->content_buf[4] << 8);
            ctx->hdr_content_height = ctx->content_buf[5] |
                                      ((uint16_t)ctx->content_buf[6] << 8);
            /* initial_scroll_y for fragment anchor navigation */
            ctx->scroll_y = (int32_t)(ctx->content_buf[7] |
                            ((uint16_t)ctx->content_buf[8] << 8));
            ctx->hdr_size = 9;
            ctx->hdr_parsed = 1;
        } else if (ctx->content_len >= 7) {
            /* Legacy 7-byte header (no initial_scroll_y) */
            ctx->bg_color = ctx->content_buf[0];
            ctx->hdr_link_count = ctx->content_buf[1] |
                                  ((uint16_t)ctx->content_buf[2] << 8);
            ctx->hdr_image_count = ctx->content_buf[3] |
                                   ((uint16_t)ctx->content_buf[4] << 8);
            ctx->hdr_content_height = ctx->content_buf[5] |
                                      ((uint16_t)ctx->content_buf[6] << 8);
            ctx->scroll_y = 0;
            ctx->hdr_size = 7;
            ctx->hdr_parsed = 1;
        }
        ctx->needs_redraw = 1;
    }
}

static void native_commit_image(NativeCtx *ctx, const uint8_t *data,
                                uint32_t data_len)
{
    /* Parse the accumulated image payload:
     * image_id(u16) width(u16) height(u16) comp_size(u32) [rle] */
    uint16_t image_id, width, height;
    uint32_t comp_size, pixel_count, rle_avail;
    NativeImage *img;

    if (data_len < 10) return;

    image_id  = data[0] | ((uint16_t)data[1] << 8);
    width     = data[2] | ((uint16_t)data[3] << 8);
    height    = data[4] | ((uint16_t)data[5] << 8);
    comp_size = data[6] | ((uint32_t)data[7] << 8) |
                ((uint32_t)data[8] << 16) | ((uint32_t)data[9] << 24);

    if (ctx->image_count >= MAX_NATIVE_IMAGES) return;

    /* Sanity checks to prevent overflow */
    if (width == 0 || height == 0) return;
    if (width > 2000 || height > 2000) return;

    pixel_count = (uint32_t)width * height;
    if (pixel_count > NATIVE_IMAGE_POOL) return;
    if (ctx->image_pool_used + pixel_count > NATIVE_IMAGE_POOL) return;

    /* Decompress into image pool using uint32_t-safe decompressor */
    img = &ctx->images[ctx->image_count];
    img->image_id = image_id;
    img->width = width;
    img->height = height;
    img->pixels = ctx->image_pool + ctx->image_pool_used;

    rle_avail = data_len - 10;
    if (rle_avail > comp_size) rle_avail = comp_size;

    rle_decompress_native(data + 10, rle_avail,
                          img->pixels, pixel_count);

    ctx->image_pool_used += pixel_count;
    ctx->image_count++;
}

void native_parse_image(NativeCtx *ctx, const uint8_t *payload,
                        uint16_t len, uint8_t flags)
{
    if (flags & FLAG_CONTINUED) {
        /* More chunks coming — accumulate into img_recv_buf */
        if (ctx->img_recv_buf &&
            ctx->img_recv_len + len <= ctx->img_recv_capacity) {
            memcpy(ctx->img_recv_buf + ctx->img_recv_len, payload, len);
            ctx->img_recv_len += len;
        }
        return;
    }

    /* Final (or only) chunk */
    if (ctx->img_recv_len > 0) {
        /* Multi-chunk: append final chunk and commit */
        if (ctx->img_recv_buf &&
            ctx->img_recv_len + len <= ctx->img_recv_capacity) {
            memcpy(ctx->img_recv_buf + ctx->img_recv_len, payload, len);
            ctx->img_recv_len += len;
            native_commit_image(ctx, ctx->img_recv_buf, ctx->img_recv_len);
        }
        ctx->img_recv_len = 0;
    } else {
        /* Single-chunk image — process directly */
        native_commit_image(ctx, payload, len);
    }
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
/* Glyph rendering helpers                                            */
/* ------------------------------------------------------------------ */

/* Get the glyph variant for a given variant_id */
static GlyphVariant *get_variant(NativeCtx *ctx, uint8_t variant_id)
{
    uint8_t i;
    for (i = 0; i < ctx->glyph_variant_count; i++) {
        if (ctx->glyph_variants[i].variant_id == variant_id)
            return &ctx->glyph_variants[i];
    }
    /* Fallback: return first variant or NULL */
    if (ctx->glyph_variant_count > 0)
        return &ctx->glyph_variants[0];
    return NULL;
}

/* Blit a 1-bit glyph bitmap to the backbuffer */
static void blit_glyph(uint8_t *buf, int stride, int screen_w,
                        int x, int y, const GlyphInfo *gi,
                        uint8_t color, int clip_top, int clip_bottom)
{
    int row, col, row_bytes;
    if (!gi->bitmap || gi->bmp_w == 0 || gi->bmp_h == 0)
        return;

    row_bytes = (gi->bmp_w + 7) / 8;

    for (row = 0; row < gi->bmp_h; row++) {
        int dy = y + gi->bmp_yoff + row;
        if (dy < clip_top) continue;
        if (dy >= clip_bottom) break;
        for (col = 0; col < gi->bmp_w; col++) {
            int dx = x + gi->bmp_xoff + col;
            if (dx < 0 || dx >= screen_w) continue;
            {
                int byte_idx = row * row_bytes + col / 8;
                int bit_idx = 7 - (col % 8);
                if (gi->bitmap[byte_idx] & (1 << bit_idx)) {
                    buf[dy * stride + dx] = color;
                }
            }
        }
    }
}

/* Draw underline for a text span — positioned just below the baseline */
static void draw_underline(uint8_t *buf, int stride, int screen_w,
                            int x, int w, int y, int baseline,
                            uint8_t color, int clip_top, int clip_bottom)
{
    int uy = y + baseline + 1;
    int ux;
    if (uy < clip_top || uy >= clip_bottom) return;
    for (ux = x; ux < x + w && ux < screen_w; ux++) {
        if (ux >= 0)
            buf[uy * stride + ux] = color;
    }
}

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

/* ------------------------------------------------------------------ */
/* Rendering — pre-positioned display list v3                         */
/*                                                                    */
/* native_walk_and_render() is the unified command stream walker.     */
/* It handles both full renders and partial strip renders (scroll).   */
/* Parameters:                                                        */
/*   rebuild_links: 1 = populate link_rects, 0 = skip CMD_LINK_RECT  */
/*   draw_top/draw_bottom: restrict pixel drawing to this strip       */
/*     (screen coords). Set both to 0 for "draw everything visible".  */
/* ------------------------------------------------------------------ */

static void native_walk_and_render(NativeCtx *ctx, VideoConfig *vc,
                                    int rebuild_links,
                                    int draw_top, int draw_bottom)
{
    uint32_t cmd_offset;
    uint8_t *buf;
    int stride, scroll_y;
    int clip_top, clip_bottom;
    int use_strip;

    buf = vc->backbuffer;
    stride = vc->width;
    scroll_y = ctx->scroll_y;

    clip_top = (int)ctx->content_top;
    clip_bottom = clip_top + (int)ctx->viewport_h;

    /* If draw_top == draw_bottom, draw everything visible (full mode).
     * Otherwise restrict drawing to the strip [draw_top, draw_bottom). */
    use_strip = (draw_top < draw_bottom);

    /* Walk command stream (skip header) */
    cmd_offset = ctx->hdr_size;

    while (cmd_offset < ctx->content_len) {
        uint8_t tag = ctx->content_buf[cmd_offset++];

        if (tag == CMD_END) break;

        switch (tag) {
        case CMD_TEXT: {
            uint16_t x, y, text_len, text_width;
            uint8_t color, font_id, flags;
            const uint8_t *text;
            int sy, dx, j;
            int eff_clip_top, eff_clip_bottom;

            if (cmd_offset + 11 > ctx->content_len) goto done;
            x = READ_U16(ctx->content_buf, cmd_offset); cmd_offset += 2;
            y = READ_U16(ctx->content_buf, cmd_offset); cmd_offset += 2;
            color = ctx->content_buf[cmd_offset++];
            font_id = ctx->content_buf[cmd_offset++];
            flags = ctx->content_buf[cmd_offset++];
            text_width = READ_U16(ctx->content_buf, cmd_offset);
            cmd_offset += 2;
            text_len = READ_U16(ctx->content_buf, cmd_offset);
            cmd_offset += 2;

            if (cmd_offset + text_len > ctx->content_len) goto done;
            text = ctx->content_buf + cmd_offset;
            cmd_offset += text_len;

            sy = (int)y - scroll_y + clip_top;

            /* Effective clip bounds (narrowed to strip if partial) */
            eff_clip_top = clip_top;
            eff_clip_bottom = clip_bottom;
            if (use_strip) {
                if (eff_clip_top < draw_top) eff_clip_top = draw_top;
                if (eff_clip_bottom > draw_bottom) eff_clip_bottom = draw_bottom;
            }

            /* Use glyph cache if available */
            if (ctx->glyph_cache_ready) {
                GlyphVariant *var = get_variant(ctx, font_id);
                int line_h, text_w;
                uint32_t sum_advances, scale_fp;

                if (!var) break;
                line_h = var->line_height;

                /* Quick visibility check against effective clip */
                if (sy + line_h <= eff_clip_top || sy >= eff_clip_bottom)
                    break;

                /* Compute scale factor to match Chrome's text width */
                sum_advances = 0;
                for (j = 0; j < text_len; j++) {
                    uint8_t ch = text[j];
                    if (ch >= 32 && ch < 128 &&
                        var->glyph_loaded[ch - 32]) {
                        sum_advances += var->glyphs[ch - 32].advance;
                    } else {
                        sum_advances += 6;
                    }
                }
                if (text_width > 0 && sum_advances > 0) {
                    scale_fp = ((uint32_t)text_width << 8) / sum_advances;
                    if (scale_fp < 128) scale_fp = 128;
                    if (scale_fp > 512) scale_fp = 512;
                } else {
                    scale_fp = 256;
                }

                /* Render glyphs with scaled advances */
                {
                uint32_t accum_x = (uint32_t)x << 8;
                text_w = 0;
                for (j = 0; j < text_len; j++) {
                    uint8_t ch = text[j];
                    uint16_t raw_adv;
                    dx = (int)(accum_x >> 8);
                    if (ch >= 32 && ch < 128) {
                        int idx = ch - 32;
                        if (var->glyph_loaded[idx]) {
                            GlyphInfo *gi = &var->glyphs[idx];
                            if (dx + gi->bmp_w + gi->bmp_xoff > 0 &&
                                dx < (int)vc->width) {
                                blit_glyph(buf, stride, vc->width,
                                           dx, sy, gi, color,
                                           eff_clip_top, eff_clip_bottom);
                            }
                            raw_adv = gi->advance;
                        } else {
                            raw_adv = 6;
                        }
                    } else {
                        raw_adv = 6;
                    }
                    accum_x += (uint32_t)raw_adv * scale_fp;
                }
                text_w = (int)((accum_x >> 8) - x);
                }

                if (flags & TEXT_UNDERLINE) {
                    draw_underline(buf, stride, vc->width,
                                   (int)x, text_w, sy,
                                   var->baseline,
                                   color, eff_clip_top, eff_clip_bottom);
                }
                if (flags & TEXT_STRIKETHROUGH) {
                    int sty = sy + line_h / 2;
                    int stx;
                    if (sty >= eff_clip_top && sty < eff_clip_bottom) {
                        for (stx = (int)x;
                             stx < (int)x + text_w && stx < (int)vc->width;
                             stx++) {
                            if (stx >= 0)
                                buf[sty * stride + stx] = color;
                        }
                    }
                }
            } else {
                /* Fallback: BIOS ROM fonts */
                int font_size, char_w, char_h;

                if (font_id <= 0) font_size = 0;
                else if (font_id <= 2) font_size = 1;
                else font_size = 2;

                char_w = font_char_width(font_size);
                char_h = font_char_height(font_size);

                if (sy + char_h <= eff_clip_top || sy >= eff_clip_bottom)
                    break;

                dx = (int)x;
                for (j = 0; j < text_len; j++) {
                    if (dx + char_w > (int)vc->width) break;
                    if (dx >= 0) {
                        font_draw_char(buf, stride, dx, sy,
                                       text[j], color, 255, font_size);
                        if (flags & TEXT_BOLD) {
                            font_draw_char(buf, stride, dx + 1, sy,
                                           text[j], color, 255, font_size);
                        }
                        if (flags & TEXT_UNDERLINE) {
                            int uy = sy + char_h - 1;
                            int ux;
                            if (uy >= eff_clip_top && uy < eff_clip_bottom) {
                                for (ux = dx; ux < dx + char_w &&
                                     ux < (int)vc->width; ux++) {
                                    buf[uy * stride + ux] = color;
                                }
                            }
                        }
                        if (flags & TEXT_STRIKETHROUGH) {
                            int sty = sy + char_h / 2;
                            int stx;
                            if (sty >= eff_clip_top && sty < eff_clip_bottom) {
                                for (stx = dx; stx < dx + char_w &&
                                     stx < (int)vc->width; stx++) {
                                    buf[sty * stride + stx] = color;
                                }
                            }
                        }
                    }
                    dx += char_w;
                }
            }
            break;
        }

        case CMD_LINK_RECT: {
            uint16_t link_id, lx, ly, lw, lh;

            if (cmd_offset + 10 > ctx->content_len) goto done;
            link_id = READ_U16(ctx->content_buf, cmd_offset);
            cmd_offset += 2;
            lx = READ_U16(ctx->content_buf, cmd_offset); cmd_offset += 2;
            ly = READ_U16(ctx->content_buf, cmd_offset); cmd_offset += 2;
            lw = READ_U16(ctx->content_buf, cmd_offset); cmd_offset += 2;
            lh = READ_U16(ctx->content_buf, cmd_offset); cmd_offset += 2;

            if (rebuild_links)
                add_link_rect(ctx, link_id, lx, ly, lw, lh);
            break;
        }

        case CMD_IMAGE: {
            uint16_t image_id, ix, iy, iw, ih;
            NativeImage *img;
            int sy;
            int eff_clip_top, eff_clip_bottom;

            if (cmd_offset + 10 > ctx->content_len) goto done;
            image_id = READ_U16(ctx->content_buf, cmd_offset);
            cmd_offset += 2;
            ix = READ_U16(ctx->content_buf, cmd_offset); cmd_offset += 2;
            iy = READ_U16(ctx->content_buf, cmd_offset); cmd_offset += 2;
            iw = READ_U16(ctx->content_buf, cmd_offset); cmd_offset += 2;
            ih = READ_U16(ctx->content_buf, cmd_offset); cmd_offset += 2;

            img = find_image(ctx, image_id);
            sy = (int)iy - scroll_y + clip_top;

            eff_clip_top = clip_top;
            eff_clip_bottom = clip_bottom;
            if (use_strip) {
                if (eff_clip_top < draw_top) eff_clip_top = draw_top;
                if (eff_clip_bottom > draw_bottom) eff_clip_bottom = draw_bottom;
                /* Skip if entirely outside strip */
                if (sy + (int)ih <= eff_clip_top || sy >= eff_clip_bottom)
                    break;
            }

            if (img && img->pixels && iw > 0 && ih > 0) {
                int row, col;
                for (row = 0; row < (int)ih; row++) {
                    int dy = sy + row;
                    int src_row;
                    if (dy < eff_clip_top) continue;
                    if (dy >= eff_clip_bottom) break;
                    src_row = row * img->height / ih;
                    if (src_row >= img->height) src_row = img->height - 1;
                    for (col = 0; col < (int)iw; col++) {
                        int dx = (int)ix + col;
                        int src_col;
                        if (dx < 0) continue;
                        if (dx >= (int)vc->width) break;
                        src_col = col * img->width / iw;
                        if (src_col >= img->width) src_col = img->width - 1;
                        buf[dy * stride + dx] =
                            img->pixels[src_row * img->width + src_col];
                    }
                }
            }
            break;
        }

        case CMD_RECT: {
            uint16_t rx, ry, rw, rh;
            uint8_t rcolor;
            int sy, row;
            int eff_clip_top, eff_clip_bottom;

            if (cmd_offset + 9 > ctx->content_len) goto done;
            rx = READ_U16(ctx->content_buf, cmd_offset); cmd_offset += 2;
            ry = READ_U16(ctx->content_buf, cmd_offset); cmd_offset += 2;
            rw = READ_U16(ctx->content_buf, cmd_offset); cmd_offset += 2;
            rh = READ_U16(ctx->content_buf, cmd_offset); cmd_offset += 2;
            rcolor = ctx->content_buf[cmd_offset++];

            sy = (int)ry - scroll_y + clip_top;

            eff_clip_top = clip_top;
            eff_clip_bottom = clip_bottom;
            if (use_strip) {
                if (eff_clip_top < draw_top) eff_clip_top = draw_top;
                if (eff_clip_bottom > draw_bottom) eff_clip_bottom = draw_bottom;
                if (sy + (int)rh <= eff_clip_top || sy >= eff_clip_bottom)
                    break;
            }

            for (row = 0; row < (int)rh; row++) {
                int dy = sy + row;
                int start_x, end_x;
                if (dy < eff_clip_top) continue;
                if (dy >= eff_clip_bottom) break;
                start_x = (int)rx;
                end_x = start_x + (int)rw;
                if (start_x < 0) start_x = 0;
                if (end_x > (int)vc->width) end_x = (int)vc->width;
                if (start_x < end_x) {
                    memset(buf + dy * stride + start_x, rcolor,
                           end_x - start_x);
                }
            }
            break;
        }

        case CMD_BG_TILE: {
            uint16_t tx, ty, area_w, area_h, tile_w, tile_h, comp_size;
            uint8_t repeat_mode;
            int sy;
            int eff_clip_top, eff_clip_bottom;

            if (cmd_offset + 15 > ctx->content_len) goto done;
            tx = READ_U16(ctx->content_buf, cmd_offset); cmd_offset += 2;
            ty = READ_U16(ctx->content_buf, cmd_offset); cmd_offset += 2;
            area_w = READ_U16(ctx->content_buf, cmd_offset); cmd_offset += 2;
            area_h = READ_U16(ctx->content_buf, cmd_offset); cmd_offset += 2;
            tile_w = READ_U16(ctx->content_buf, cmd_offset); cmd_offset += 2;
            tile_h = READ_U16(ctx->content_buf, cmd_offset); cmd_offset += 2;
            repeat_mode = ctx->content_buf[cmd_offset++];
            comp_size = READ_U16(ctx->content_buf, cmd_offset); cmd_offset += 2;

            if (cmd_offset + comp_size > ctx->content_len) goto done;

            sy = (int)ty - scroll_y + clip_top;

            eff_clip_top = clip_top;
            eff_clip_bottom = clip_bottom;
            if (use_strip) {
                if (eff_clip_top < draw_top) eff_clip_top = draw_top;
                if (eff_clip_bottom > draw_bottom) eff_clip_bottom = draw_bottom;
                if (sy + (int)area_h <= eff_clip_top || sy >= eff_clip_bottom) {
                    cmd_offset += comp_size;
                    break;
                }
            }

            if (ctx->bg_tile_pool && tile_w > 0 && tile_h > 0 &&
                (uint32_t)tile_w * tile_h <= BG_TILE_POOL) {
                uint8_t *tile_pix = ctx->bg_tile_pool;
                int row, col;
                int render_w, render_h;

                rle_decompress_native(ctx->content_buf + cmd_offset,
                                      (uint32_t)comp_size, tile_pix,
                                      (uint32_t)tile_w * tile_h);

                render_w = (int)area_w;
                render_h = (int)area_h;
                if (repeat_mode == 3) {
                    if (render_w > (int)tile_w) render_w = (int)tile_w;
                    if (render_h > (int)tile_h) render_h = (int)tile_h;
                } else if (repeat_mode == 1) {
                    if (render_h > (int)tile_h) render_h = (int)tile_h;
                } else if (repeat_mode == 2) {
                    if (render_w > (int)tile_w) render_w = (int)tile_w;
                }

                for (row = 0; row < render_h; row++) {
                    int dy = sy + row;
                    int tile_row;
                    if (dy < eff_clip_top) continue;
                    if (dy >= eff_clip_bottom) break;
                    tile_row = row % tile_h;
                    for (col = 0; col < render_w; col++) {
                        int dx = (int)tx + col;
                        int tile_col;
                        if (dx < 0) continue;
                        if (dx >= (int)vc->width) break;
                        tile_col = col % tile_w;
                        buf[dy * stride + dx] =
                            tile_pix[tile_row * tile_w + tile_col];
                    }
                }
            }
            cmd_offset += comp_size;
            break;
        }

        default:
            goto done;
        }
    }

done:
    return;
}

/* ------------------------------------------------------------------ */
/* Full render — clears viewport, rebuilds links, draws everything    */
/* ------------------------------------------------------------------ */

static void native_render_full(NativeCtx *ctx, VideoConfig *vc)
{
    ctx->link_count = 0;

    /* Clear content area to background color (not scrollbar) */
    video_fill_rect(vc, 0, ctx->content_top,
                    ctx->viewport_w, ctx->viewport_h, ctx->bg_color);

    /* Walk entire command stream, rebuild links, draw all visible */
    native_walk_and_render(ctx, vc, 1, 0, 0);

    /* Mark entire content area dirty */
    video_mark_dirty(vc, 0, ctx->content_top,
                     ctx->viewport_w, ctx->viewport_h);
}

/* ------------------------------------------------------------------ */
/* Scroll render — shift backbuffer, redraw only the exposed strip    */
/* ------------------------------------------------------------------ */

static void native_render_scroll(NativeCtx *ctx, VideoConfig *vc, int16_t dy)
{
    int clip_top = (int)ctx->content_top;
    int abs_dy = (dy < 0) ? -dy : dy;
    int strip_top, strip_bottom;
    /* Shift area is exactly the content area (status bar is below) */
    int shift_bottom = (int)ctx->content_top + (int)ctx->viewport_h;

    /* 1. Shift visible content area (excludes status bar) */
    video_shift_content(vc, dy, ctx->bg_color);

    /* 2. Determine the exposed strip (within shift area) */
    if (dy > 0) {
        /* Scrolled down: new content exposed at bottom */
        strip_top = shift_bottom - abs_dy;
        strip_bottom = shift_bottom;
    } else {
        /* Scrolled up: new content exposed at top */
        strip_top = clip_top;
        strip_bottom = clip_top + abs_dy;
    }

    /* 3. Clear the strip to bg_color (content width only) */
    if (strip_top < strip_bottom) {
        video_fill_rect(vc, 0, strip_top, ctx->viewport_w,
                        strip_bottom - strip_top, ctx->bg_color);
    }

    /* 4. Walk commands, draw only those intersecting the strip.
     *    Links are NOT rebuilt — they're in document space and
     *    don't change when we scroll. */
    native_walk_and_render(ctx, vc, 0, strip_top, strip_bottom);

    /* 5. Mark content area dirty for VGA flush (shifted rows changed) */
    video_mark_dirty(vc, 0, ctx->content_top,
                     ctx->viewport_w, ctx->viewport_h);
}

/* ------------------------------------------------------------------ */
/* Public render entry point — dispatches full vs scroll              */
/* ------------------------------------------------------------------ */

void native_render(NativeCtx *ctx, VideoConfig *vc)
{
    if (!ctx->hdr_parsed || !ctx->content_buf) return;

    /* Compute max_scroll_y and clamp BEFORE rendering, so initial_scroll_y
     * from fragment anchors is properly bounded on first render. */
    ctx->max_scroll_y = (int32_t)ctx->hdr_content_height -
                        (int32_t)ctx->viewport_h;
    if (ctx->max_scroll_y < 0) ctx->max_scroll_y = 0;
    if (ctx->scroll_y > ctx->max_scroll_y)
        ctx->scroll_y = ctx->max_scroll_y;
    if (ctx->scroll_y < 0) ctx->scroll_y = 0;

    ctx->needs_redraw = 0;

    if (ctx->scroll_pending_dy != 0) {
        int16_t dy = ctx->scroll_pending_dy;
        int abs_dy = (dy < 0) ? -dy : dy;
        ctx->scroll_pending_dy = 0;

        /* Use partial scroll if delta < visible content height */
        {
        int shift_h = (int)ctx->viewport_h;
        if (shift_h > 0 && abs_dy < shift_h) {
            native_render_scroll(ctx, vc, dy);
            return;
        }
        }
        /* else: scroll >= viewport, fall through to full render */
    }

    native_render_full(ctx, vc);
}

/* ------------------------------------------------------------------ */
/* Scrolling                                                          */
/* ------------------------------------------------------------------ */

void native_scroll(NativeCtx *ctx, int32_t delta_y)
{
    int32_t new_scroll = ctx->scroll_y + delta_y;
    int32_t actual_delta;
    if (new_scroll < 0) new_scroll = 0;
    if (new_scroll > ctx->max_scroll_y)
        new_scroll = ctx->max_scroll_y;
    actual_delta = new_scroll - ctx->scroll_y;
    if (actual_delta != 0) {
        ctx->scroll_y = new_scroll;
        ctx->scroll_pending_dy += (int16_t)actual_delta;
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
    int i;

    /* Iterate in reverse so later, more-specific rects win.
     * Image map <area> rects are emitted after the parent <a>
     * link_rect, so reverse order gives them priority. */
    for (i = (int)ctx->link_count - 1; i >= 0; i--) {
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
