/*
 * RetroSurf YouTube Mode - Video playback in Mode 13h
 *
 * Receives block-delta RLE video frames from the server and blits
 * them directly to VGA Mode 13h (320x200x256).
 *
 * Phase 2: Sound Blaster audio via DMA double-buffering.
 * Audio samples arrive as MSG_YT_AUDIO messages interleaved with
 * video frames. Samples are fed into a ring buffer; DMA plays them
 * continuously via auto-init mode.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <conio.h>
#include <dpmi.h>
#include <go32.h>
#include <sys/farptr.h>
#include <sys/nearptr.h>
#include <tcp.h>

#include "youtube.h"
#include "protocol.h"
#include "network.h"
#include "video.h"
#include "font.h"
#include "input.h"
#include "render.h"  /* for rle_decode */
#include "sbdma.h"

/* YouTube context (local to this module) */
typedef struct {
    uint8_t  state;
    uint16_t video_w, video_h;
    uint8_t  fps;
    uint16_t audio_rate;
    uint8_t  audio_bits;
    uint32_t duration_ms;
    uint32_t current_ms;
    char     title[80];

    /* Software framebuffer (320x200 in extended memory) */
    uint8_t  *framebuf;

    /* Direct VGA pointer (nearptr) for delta block writes */
    uint8_t  *vga_base;

    /* Dirty block tracking — marks which blocks need VGA update */
    uint8_t  dirty[YT_BLOCKS_X * YT_BLOCKS_Y];

    /* Audio state */
    int      has_audio;      /* Server indicated audio available */
    int      sb_available;   /* Sound Blaster detected */
    int      sb_started;     /* DMA playback running */
    uint32_t audio_msgs;     /* Debug: count of audio messages received */
    uint16_t last_frame_num; /* Last complete video frame_num (for ACK) */
    clock_t  last_display;   /* Clock tick of last VGA frame display */

    /* Progress bar layout (computed by yt_draw_ui, used by mouse click) */
    int      track_x1, track_x2;
} YTContext;

/* ---- Palette setup (6x6x6 cube, same as main.c) ---- */

static void yt_set_palette(void)
{
    uint8_t pal[768];
    int r, g, b, i;

    memset(pal, 0, sizeof(pal));

    /* 6x6x6 RGB cube (indices 0-215) */
    for (r = 0; r < 6; r++)
        for (g = 0; g < 6; g++)
            for (b = 0; b < 6; b++) {
                i = r * 36 + g * 6 + b;
                pal[i * 3 + 0] = (uint8_t)(r * 51);
                pal[i * 3 + 1] = (uint8_t)(g * 51);
                pal[i * 3 + 2] = (uint8_t)(b * 51);
            }

    /* Grayscale ramp (indices 216-239) */
    for (i = 216; i < 240; i++) {
        uint8_t v = (uint8_t)(8 + (i - 216) * 10);
        pal[i * 3 + 0] = v;
        pal[i * 3 + 1] = v;
        pal[i * 3 + 2] = v;
    }

    /* Web UI accent colors (indices 240-249) */
    pal[240*3+0] =   0; pal[240*3+1] = 102; pal[240*3+2] = 204;
    pal[241*3+0] =   0; pal[241*3+1] =   0; pal[241*3+2] = 238;
    pal[242*3+0] = 204; pal[242*3+1] =   0; pal[242*3+2] =   0;
    pal[243*3+0] =   0; pal[243*3+1] = 153; pal[243*3+2] =   0;
    pal[244*3+0] = 255; pal[244*3+1] = 204; pal[244*3+2] =   0;
    pal[245*3+0] = 192; pal[245*3+1] = 192; pal[245*3+2] = 192;
    pal[246*3+0] = 128; pal[246*3+1] = 128; pal[246*3+2] = 128;
    pal[247*3+0] = 240; pal[247*3+1] = 240; pal[247*3+2] = 240;
    pal[248*3+0] =  51; pal[248*3+1] =  51; pal[248*3+2] =  51;
    pal[249*3+0] = 255; pal[249*3+1] = 255; pal[249*3+2] = 255;

    /* DOS chrome colors (indices 250-254) */
    pal[250*3+0] =  64; pal[250*3+1] =  64; pal[250*3+2] =  64;
    pal[251*3+0] = 255; pal[251*3+1] = 255; pal[251*3+2] = 255;
    pal[252*3+0] = 240; pal[252*3+1] = 240; pal[252*3+2] = 240;
    pal[253*3+0] =   0; pal[253*3+1] =   0; pal[253*3+2] =   0;
    pal[254*3+0] =   0; pal[254*3+1] = 120; pal[254*3+2] = 215;

    /* 255 = magenta key color */
    pal[255*3+0] = 255; pal[255*3+1] =   0; pal[255*3+2] = 255;

    video_set_palette(pal, 256);
}

/* Palette indices for UI (also used by cursor) */
#define UI_BLACK       0     /* 0,0,0 */
#define UI_DARK_GRAY   216   /* grayscale ramp, ~dark gray */
#define UI_RED         180   /* 5*36+0*6+0 = red (255,0,0) */
#define UI_WHITE       215   /* 5*36+5*6+5 = white (255,255,255) */

/* ---- Simple mouse cursor for Mode 13h ---- */

/* 8x8 arrow cursor bitmap (1=white, 2=black outline, 0=transparent) */
static const uint8_t yt_cursor[8][8] = {
    {2,0,0,0,0,0,0,0},
    {2,2,0,0,0,0,0,0},
    {2,1,2,0,0,0,0,0},
    {2,1,1,2,0,0,0,0},
    {2,1,1,1,2,0,0,0},
    {2,1,1,1,1,2,0,0},
    {2,1,2,2,0,0,0,0},
    {2,0,0,2,0,0,0,0},
};

/* Save-under buffer for cursor restore */
static uint8_t yt_cursor_save[8 * 8];
static int yt_cursor_saved_x = -1, yt_cursor_saved_y = -1;

static void yt_cursor_restore(YTContext *ctx)
{
    int row, cx, cy;
    if (yt_cursor_saved_x < 0) return;
    cx = yt_cursor_saved_x;
    cy = yt_cursor_saved_y;
    for (row = 0; row < 8; row++) {
        int y = cy + row;
        if (y >= 0 && y < YT_VIDEO_H) {
            memcpy(ctx->framebuf + y * YT_VIDEO_W + cx,
                   yt_cursor_save + row * 8, 8);
        }
    }
}

static void yt_cursor_draw(YTContext *ctx, int mx, int my)
{
    int row, col;
    /* Clamp position */
    if (mx > YT_VIDEO_W - 8) mx = YT_VIDEO_W - 8;
    if (my > YT_VIDEO_H - 8) my = YT_VIDEO_H - 8;
    if (mx < 0) mx = 0;
    if (my < 0) my = 0;

    /* Save under */
    for (row = 0; row < 8; row++) {
        int y = my + row;
        if (y >= 0 && y < YT_VIDEO_H) {
            memcpy(yt_cursor_save + row * 8,
                   ctx->framebuf + y * YT_VIDEO_W + mx, 8);
        }
    }
    yt_cursor_saved_x = mx;
    yt_cursor_saved_y = my;

    /* Draw cursor */
    for (row = 0; row < 8; row++) {
        int y = my + row;
        if (y < 0 || y >= YT_VIDEO_H) continue;
        for (col = 0; col < 8; col++) {
            int x = mx + col;
            if (x < 0 || x >= YT_VIDEO_W) continue;
            if (yt_cursor[row][col] == 1)
                ctx->framebuf[y * YT_VIDEO_W + x] = UI_WHITE;
            else if (yt_cursor[row][col] == 2)
                ctx->framebuf[y * YT_VIDEO_W + x] = UI_BLACK;
        }
    }
}

/* ---- VGA Mode 13h helpers ---- */

static void yt_flush_frame(YTContext *ctx)
{
    /* Copy 64000 bytes from system RAM framebuffer to VGA */
    dosmemput(ctx->framebuf, YT_FRAMEBUF_SIZE, 0xA0000);
}

/* Copy only dirty blocks from framebuf to VGA, then clear dirty flags */
static void yt_flush_dirty(YTContext *ctx)
{
    int bx, by, row;

    if (ctx->vga_base) {
        /* Fast path: nearptr direct copy */
        for (by = 0; by < YT_BLOCKS_Y; by++) {
            for (bx = 0; bx < YT_BLOCKS_X; bx++) {
                if (ctx->dirty[by * YT_BLOCKS_X + bx]) {
                    uint16_t off = (uint16_t)by * 8 * YT_VIDEO_W
                                 + (uint16_t)bx * 8;
                    uint8_t *src = ctx->framebuf + off;
                    uint8_t *dst = ctx->vga_base + off;
                    for (row = 0; row < YT_BLOCK_SIZE; row++) {
                        memcpy(dst, src, YT_BLOCK_SIZE);
                        src += YT_VIDEO_W;
                        dst += YT_VIDEO_W;
                    }
                    ctx->dirty[by * YT_BLOCKS_X + bx] = 0;
                }
            }
        }
    } else {
        /* Fallback: dosmemput full frame */
        yt_flush_frame(ctx);
        memset(ctx->dirty, 0, sizeof(ctx->dirty));
    }
}

static void yt_draw_loading(YTContext *ctx)
{
    const char *msg = "Loading video...";
    int x, y;

    memset(ctx->framebuf, 0, YT_FRAMEBUF_SIZE);

    /* Center the text */
    x = (YT_VIDEO_W - font_string_width(msg, FONT_MEDIUM)) / 2;
    y = (YT_VIDEO_H - font_char_height(FONT_MEDIUM)) / 2;

    font_draw_string(ctx->framebuf, YT_VIDEO_W, x, y,
                     msg, 215, 0, FONT_MEDIUM);

    yt_flush_frame(ctx);
}

/* ---- Format time as "M:SS" or "H:MM:SS" ---- */

static void yt_format_time(uint32_t ms, char *buf, int bufsize)
{
    uint32_t total_sec = ms / 1000;
    uint32_t h = total_sec / 3600;
    uint32_t m = (total_sec % 3600) / 60;
    uint32_t s = total_sec % 60;

    if (h > 0) {
        sprintf(buf, "%lu:%02lu:%02lu", (unsigned long)h,
                (unsigned long)m, (unsigned long)s);
    } else {
        sprintf(buf, "%lu:%02lu", (unsigned long)m, (unsigned long)s);
    }
    (void)bufsize;
}

/* ---- Player UI overlay (bottom 12 pixels, y=188..199) ---- */

static void yt_draw_ui(YTContext *yt)
{
    char elapsed_buf[16], total_buf[16];
    int elapsed_w, total_w;
    int track_x1, track_x2, track_w, fill_w;
    int bar_y = 190;
    int bar_h = 6;
    int x, row;

    /* Format elapsed and total time */
    yt_format_time(yt->current_ms, elapsed_buf, sizeof(elapsed_buf));
    yt_format_time(yt->duration_ms, total_buf, sizeof(total_buf));

    elapsed_w = font_string_width(elapsed_buf, FONT_SMALL);
    total_w = font_string_width(total_buf, FONT_SMALL);

    /* 2px separator line at y=188..189 */
    for (row = 188; row < 190; row++) {
        memset(yt->framebuf + row * YT_VIDEO_W, UI_BLACK, YT_VIDEO_W);
    }

    /* Progress bar background (y=190..195) */
    for (row = bar_y; row < bar_y + bar_h; row++) {
        memset(yt->framebuf + row * YT_VIDEO_W, UI_BLACK, YT_VIDEO_W);
    }

    /* Elapsed time text at left */
    font_draw_string(yt->framebuf, YT_VIDEO_W, 4, bar_y - 1,
                     elapsed_buf, UI_WHITE, UI_BLACK, FONT_SMALL);

    /* Total time text at right */
    font_draw_string(yt->framebuf, YT_VIDEO_W, YT_VIDEO_W - total_w - 4,
                     bar_y - 1, total_buf, UI_WHITE, UI_BLACK, FONT_SMALL);

    /* Progress bar track between text labels */
    track_x1 = 4 + elapsed_w + 4;
    track_x2 = YT_VIDEO_W - total_w - 8;
    track_w = track_x2 - track_x1;

    /* Save for mouse click hit testing */
    yt->track_x1 = track_x1;
    yt->track_x2 = track_x2;

    if (track_w > 10 && yt->duration_ms > 0) {
        /* Dark gray track background */
        for (row = bar_y + 1; row < bar_y + bar_h - 1; row++) {
            memset(yt->framebuf + row * YT_VIDEO_W + track_x1,
                   UI_DARK_GRAY, track_w);
        }

        /* Red filled portion */
        fill_w = (int)((uint32_t)yt->current_ms * (uint32_t)track_w
                       / yt->duration_ms);
        if (fill_w > track_w) fill_w = track_w;
        if (fill_w > 0) {
            for (row = bar_y + 1; row < bar_y + bar_h - 1; row++) {
                memset(yt->framebuf + row * YT_VIDEO_W + track_x1,
                       UI_RED, fill_w);
            }
        }
    }

    /* Bottom area (y=196..199) black */
    for (row = 196; row < 200; row++) {
        memset(yt->framebuf + row * YT_VIDEO_W, UI_BLACK, YT_VIDEO_W);
    }

    /* Mark dirty blocks for rows y=184..199 (by=23 and by=24) */
    for (x = 0; x < YT_BLOCKS_X; x++) {
        yt->dirty[23 * YT_BLOCKS_X + x] = 1;
        yt->dirty[24 * YT_BLOCKS_X + x] = 1;
    }
}

static void yt_draw_pause(YTContext *yt)
{
    const char *text = "PAUSED";
    int tw, th, bw, bh, bx, by, total_h;
    int row;
    int ttw = 0, tty = 0;

    tw = font_string_width(text, FONT_MEDIUM);
    th = font_char_height(FONT_MEDIUM);

    /* Calculate total height including title */
    total_h = th + 12;
    if (yt->title[0]) {
        ttw = font_string_width(yt->title, FONT_SMALL);
        total_h += 4 + font_char_height(FONT_SMALL);
    }

    /* Width covers both "PAUSED" and title */
    bw = tw + 16;
    if (yt->title[0] && ttw + 16 > bw)
        bw = ttw + 16;

    bh = total_h;
    bx = (YT_VIDEO_W - bw) / 2;
    by = (180 - bh) / 2;  /* Center in video area above UI */

    /* Dark rectangle background — covers BOTH text and title */
    for (row = by; row < by + bh; row++) {
        if (row >= 0 && row < 188)
            memset(yt->framebuf + row * YT_VIDEO_W + bx, UI_BLACK, bw);
    }

    /* Draw "PAUSED" centered */
    font_draw_string(yt->framebuf, YT_VIDEO_W,
                     bx + (bw - tw) / 2, by + 6,
                     text, UI_WHITE, UI_BLACK, FONT_MEDIUM);

    /* Draw title below, also centered within the rectangle */
    if (yt->title[0]) {
        tty = by + th + 12;
        if (tty + 8 < 188) {
            font_draw_string(yt->framebuf, YT_VIDEO_W,
                             bx + (bw - ttw) / 2, tty,
                             yt->title, UI_WHITE, UI_BLACK, FONT_SMALL);
        }
    }

    /* Mark affected blocks dirty */
    {
        int dbx1 = bx / YT_BLOCK_SIZE;
        int dbx2 = (bx + bw + YT_BLOCK_SIZE - 1) / YT_BLOCK_SIZE;
        int dby1 = by / YT_BLOCK_SIZE;
        int dby2 = (by + bh + YT_BLOCK_SIZE - 1) / YT_BLOCK_SIZE;
        int dx, dy;
        if (dbx1 < 0) dbx1 = 0;
        if (dbx2 > YT_BLOCKS_X) dbx2 = YT_BLOCKS_X;
        if (dby1 < 0) dby1 = 0;
        if (dby2 > YT_BLOCKS_Y) dby2 = YT_BLOCKS_Y;
        for (dy = dby1; dy < dby2; dy++)
            for (dx = dbx1; dx < dbx2; dx++)
                yt->dirty[dy * YT_BLOCKS_X + dx] = 1;
    }
}

/* ---- Protocol helpers ---- */

static void yt_send_control(net_context_t *ctx, uint8_t action)
{
    uint8_t buf[1];
    buf[0] = action;
    net_send_message(ctx, MSG_YT_CONTROL, buf, 1);
}

static void yt_send_seek(net_context_t *ctx, uint32_t target_ms)
{
    uint8_t buf[5];
    buf[0] = YT_ACTION_SEEK;
    buf[1] = (uint8_t)(target_ms & 0xFF);
    buf[2] = (uint8_t)((target_ms >> 8) & 0xFF);
    buf[3] = (uint8_t)((target_ms >> 16) & 0xFF);
    buf[4] = (uint8_t)((target_ms >> 24) & 0xFF);
    net_send_message(ctx, MSG_YT_CONTROL, buf, 5);
}

static void yt_send_ack(net_context_t *ctx, uint16_t audio_buffer_ms,
                        uint16_t last_frame_num)
{
    uint8_t buf[4];
    buf[0] = (uint8_t)(audio_buffer_ms & 0xFF);
    buf[1] = (uint8_t)(audio_buffer_ms >> 8);
    buf[2] = (uint8_t)(last_frame_num & 0xFF);
    buf[3] = (uint8_t)(last_frame_num >> 8);
    net_send_message(ctx, MSG_YT_ACK, buf, 4);
}

/* ---- Frame parsing ---- */

static void yt_parse_start(YTContext *yt, const uint8_t *payload, uint16_t len)
{
    uint16_t title_len;

    if (len < 14) return;

    memcpy(&yt->video_w, payload + 0, 2);
    memcpy(&yt->video_h, payload + 2, 2);
    yt->fps = payload[4];
    memcpy(&yt->audio_rate, payload + 5, 2);
    yt->audio_bits = payload[7];
    memcpy(&yt->duration_ms, payload + 8, 4);
    yt->duration_ms *= 1000;  /* convert seconds to ms */
    memcpy(&title_len, payload + 12, 2);

    if (title_len > 79) title_len = 79;
    if (14 + title_len <= len) {
        memcpy(yt->title, payload + 14, title_len);
    }
    yt->title[title_len] = '\0';

    /* Check if server is sending audio */
    yt->has_audio = (yt->audio_rate > 0 && yt->audio_bits > 0);

    printf("[YT] Start: %dx%d @%dfps audio=%uHz/%ubit has_audio=%d\n",
           yt->video_w, yt->video_h, yt->fps,
           yt->audio_rate, yt->audio_bits, yt->has_audio);
}

static void yt_apply_frame_chunk(YTContext *yt, const uint8_t *payload,
                                  uint16_t len)
{
    const uint8_t *ptr = payload;
    const uint8_t *end = payload + len;
    uint16_t block_count;
    uint8_t block_temp[YT_BLOCK_PIXELS];
    int i;

    /* Parse frame_num(4) + timestamp_ms(4) */
    if (len < 10) return;

    {
        uint32_t fn;
        memcpy(&fn, ptr, 4);
        yt->last_frame_num = (uint16_t)(fn & 0xFFFF);
    }
    memcpy(&yt->current_ms, ptr + 4, 4);  /* timestamp_ms */
    ptr += 8;

    memcpy(&block_count, ptr, 2);
    ptr += 2;

    for (i = 0; i < block_count; i++) {
        uint8_t bx, by;
        uint16_t comp_size;
        int row;
        uint8_t *dst;

        if (ptr + 4 > end) break;
        bx = *ptr++;
        by = *ptr++;
        memcpy(&comp_size, ptr, 2);
        ptr += 2;

        if (ptr + comp_size > end) break;
        if (bx >= YT_BLOCKS_X || by >= YT_BLOCKS_Y) {
            ptr += comp_size;
            continue;
        }

        /* RLE decode into temp buffer */
        rle_decode(ptr, comp_size, block_temp, YT_BLOCK_PIXELS);
        ptr += comp_size;

        /* Blit 8x8 block to framebuffer only (VGA updated separately) */
        dst = yt->framebuf + (uint16_t)by * 8 * YT_VIDEO_W
              + (uint16_t)bx * 8;
        for (row = 0; row < YT_BLOCK_SIZE; row++) {
            memcpy(dst, block_temp + row * YT_BLOCK_SIZE, YT_BLOCK_SIZE);
            dst += YT_VIDEO_W;
        }
        yt->dirty[(uint16_t)by * YT_BLOCKS_X + bx] = 1;
    }
}

/* ---- Audio message handling ---- */

static void yt_handle_audio(YTContext *yt, const uint8_t *payload,
                             uint16_t len)
{
    uint16_t sample_count;

    /* MSG_YT_AUDIO payload: timestamp_ms(4) + sample_count(2) + data[] */
    if (len < 6) return;
    if (!yt->sb_available) return;

    memcpy(&sample_count, payload + 4, 2);
    if (6 + sample_count > len) return;

    sb_feed(payload + 6, sample_count);
    yt->audio_msgs++;

    /* Start DMA playback after 3 audio chunks (~300ms buffered).
     * Can't use sb_buffer_ms() here because sample_rate isn't set
     * until sb_start() is called. */
    if (!yt->sb_started && yt->audio_msgs >= 3) {
        if (sb_start(yt->audio_rate) == 0) {
            yt->sb_started = 1;
        }
    }
}

/* ---- Main YouTube loop ---- */

int run_youtube(Config *cfg, VideoConfig *vc, net_context_t *ctx,
                uint8_t *recv_buf)
{
    YTContext yt;
    msg_header_t header;
    uint16_t payload_len;
    int quit = 0;
    int disconnected = 0;
    int got_start = 0;
    clock_t timeout;
    int rc;

    memset(&yt, 0, sizeof(yt));
    yt.state = YT_STATE_IDLE;

    /* Allocate software framebuffer */
    yt.framebuf = (uint8_t *)malloc(YT_FRAMEBUF_SIZE);
    if (!yt.framebuf) {
        printf("ERROR: Cannot allocate YouTube framebuffer\n");
        return -1;
    }
    memset(yt.framebuf, 0, YT_FRAMEBUF_SIZE);

    /* Switch to Mode 13h */
    video_set_mode_13h();
    yt_set_palette();

    /* Enable near pointer for direct VGA writes (zero-overhead block blit) */
    if (__djgpp_nearptr_enable()) {
        yt.vga_base = (uint8_t *)(0xA0000 + __djgpp_conventional_base);
    } else {
        yt.vga_base = NULL;  /* fallback: use dosmemput flush */
    }

    /* Show loading screen */
    yt_draw_loading(&yt);

    /* Wait for MSG_YT_START (with timeout) */
    timeout = clock() + 15 * CLOCKS_PER_SEC;
    while (!got_start && clock() < timeout) {
        net_poll();
        rc = net_recv_message(ctx, &header, recv_buf, &payload_len);
        if (rc < 0) { disconnected = 1; goto cleanup; }
        if (rc == 0) continue;

        if (header.msg_type == MSG_YT_START) {
            yt_parse_start(&yt, recv_buf, payload_len);
            got_start = 1;
        } else if (header.msg_type == MSG_PALETTE) {
            video_set_palette(recv_buf, payload_len / 3);
        } else if (header.msg_type == MSG_MODE_SWITCH) {
            if (recv_buf[0] != 2) goto cleanup;
        }
    }

    if (!got_start) goto cleanup;

    /* Show audio/SB info on loading screen for debug */
    {
        char dbg[80];
        int dy = 0;

        sprintf(dbg, "Audio: %uHz %ubit %s",
                yt.audio_rate, yt.audio_bits,
                yt.has_audio ? "YES" : "NO");
        font_draw_string(yt.framebuf, YT_VIDEO_W, 4, 4 + dy,
                         dbg, 215, 0, FONT_SMALL);
        dy += 10;

        /* Detect Sound Blaster if server is sending audio */
        if (yt.has_audio) {
            sprintf(dbg, "SB detect: base=%Xh irq=%d dma=%d",
                    cfg->sb_base, cfg->sb_irq, cfg->sb_dma);
            font_draw_string(yt.framebuf, YT_VIDEO_W, 4, 4 + dy,
                             dbg, 215, 0, FONT_SMALL);
            dy += 10;

            yt.sb_available = (sb_detect(cfg->sb_base, cfg->sb_irq,
                                         cfg->sb_dma) == 0);

            sprintf(dbg, "SB result: %s",
                    yt.sb_available ? "FOUND" : "NOT FOUND");
            font_draw_string(yt.framebuf, YT_VIDEO_W, 4, 4 + dy,
                             dbg, yt.sb_available ? 243 : 242, 0,
                             FONT_SMALL);
            dy += 10;
        } else {
            font_draw_string(yt.framebuf, YT_VIDEO_W, 4, 4 + dy,
                             "Server sent no audio", 242, 0, FONT_SMALL);
            dy += 10;
        }

        yt_flush_frame(&yt);
    }

    yt.state = YT_STATE_PLAYING;
    yt.last_frame_num = 0xFFFF;  /* No frame received yet */
    yt.last_display = clock();   /* Init display pacing clock */

    /* Initialize mouse for Mode 13h.
     * INT 33h always reports x in virtual pixels (0-639) in Mode 13h
     * due to hardware pixel doubling. Set range to 640 and divide
     * x by 2 when reading. Standard Mode 13h technique. */
    input_init_mouse(YT_VIDEO_W * 2, YT_VIDEO_H);

    /* Mouse state must persist across loop iterations to track
     * button transitions (pressed → released). A local variable
     * would re-init to zero each iteration, causing every held
     * button poll to register as a new click. */
    {
    MouseState mouse;
    memset(&mouse, 0, sizeof(mouse));

    /* Clear framebuffer and force full VGA clear (remove debug text) */
    memset(yt.framebuf, 0, YT_FRAMEBUF_SIZE);
    memset(yt.dirty, 1, sizeof(yt.dirty));

    /* === Main YouTube playback loop ===
     *
     * ARCHITECTURE: Decode and display are DECOUPLED.
     * - Each iteration: receive ONE message, decode to framebuf only
     * - Display tick: on a fixed timer, copy dirty blocks to VGA
     *
     * The display tick is checked after EVERY message. This guarantees
     * VGA updates happen on time even when messages arrive continuously.
     * If the server sends faster than the display rate, frames silently
     * overwrite each other in the framebuf — only the latest is shown. */
    {
    clock_t display_interval = CLOCKS_PER_SEC / yt.fps;

    while (!quit) {
        /* 1. Network + audio pump */
        net_poll();
        if (yt.sb_started) sb_pump();

        /* 2. Try to receive ONE complete message */
        rc = net_recv_message(ctx, &header, recv_buf, &payload_len);
        if (rc < 0) { quit = 1; disconnected = 1; break; }

        if (rc == 1) {
            /* Got a complete message — decode to framebuf */
            switch (header.msg_type) {
            case MSG_YT_FRAME:
                yt_apply_frame_chunk(&yt, recv_buf, payload_len);
                break;

            case MSG_YT_AUDIO:
                yt_handle_audio(&yt, recv_buf, payload_len);
                break;

            case MSG_YT_EOF:
                yt.state = YT_STATE_ENDED;
                quit = 1;
                break;

            case MSG_MODE_SWITCH:
                if (payload_len >= 1 && recv_buf[0] != 2)
                    quit = 1;
                break;

            case MSG_PALETTE:
                video_set_palette(recv_buf, payload_len / 3);
                break;

            default:
                break;
            }
        }

        /* 3. Display tick — flush dirty blocks to VGA at fixed rate */
        {
            clock_t now = clock();
            if (now - yt.last_display >= display_interval) {
                /* Restore cursor save-under before drawing UI */
                yt_cursor_restore(&yt);

                /* Draw player UI overlay into framebuf */
                yt_draw_ui(&yt);

                /* Draw pause overlay if paused */
                if (yt.state == YT_STATE_PAUSED) {
                    yt_draw_pause(&yt);
                }

                /* Draw mouse cursor on top, mark its blocks dirty */
                yt_cursor_draw(&yt, mouse.x / 2, mouse.y);
                {
                    int cbx = (mouse.x / 2) / YT_BLOCK_SIZE;
                    int cby = mouse.y / YT_BLOCK_SIZE;
                    int dx, dy;
                    for (dy = cby; dy <= cby + 1 && dy < YT_BLOCKS_Y; dy++)
                        for (dx = cbx; dx <= cbx + 1 && dx < YT_BLOCKS_X; dx++)
                            yt.dirty[dy * YT_BLOCKS_X + dx] = 1;
                    /* Also mark old cursor position dirty */
                    if (yt_cursor_saved_x >= 0) {
                        int obx = yt_cursor_saved_x / YT_BLOCK_SIZE;
                        int oby = yt_cursor_saved_y / YT_BLOCK_SIZE;
                        for (dy = oby; dy <= oby + 1 && dy < YT_BLOCKS_Y; dy++)
                            for (dx = obx; dx <= obx + 1 && dx < YT_BLOCKS_X; dx++)
                                yt.dirty[dy * YT_BLOCKS_X + dx] = 1;
                    }
                }

                yt_flush_dirty(&yt);
                yt.last_display = now;
            }
        }

        /* 4. Handle keyboard */
        {
            KeyEvent key;
            while (input_poll_key(&key)) {
                if (key.ascii == 27) {
                    yt_send_control(ctx, YT_ACTION_STOP);
                    {
                        clock_t drain = clock() + CLOCKS_PER_SEC;
                        while (clock() < drain) {
                            net_poll();
                            if (yt.sb_started) sb_pump();
                            rc = net_recv_message(ctx, &header,
                                                   recv_buf, &payload_len);
                            if (rc < 0) break;
                            if (rc == 1 &&
                                header.msg_type == MSG_MODE_SWITCH)
                                break;
                        }
                    }
                    quit = 1;
                } else if (key.ascii == ' ') {
                    if (yt.state == YT_STATE_PAUSED) {
                        yt_send_control(ctx, YT_ACTION_RESUME);
                        yt.state = YT_STATE_PLAYING;
                    } else if (yt.state == YT_STATE_PLAYING) {
                        yt_send_control(ctx, YT_ACTION_PAUSE);
                        yt.state = YT_STATE_PAUSED;
                    }
                } else if (key.extended) {
                    /* Arrow key seek */
                    uint32_t seek_target = 0xFFFFFFFF;
                    if (key.scancode == 0x4B) {
                        /* Left arrow: seek back 10s */
                        seek_target = 0;
                        if (yt.current_ms > 10000)
                            seek_target = yt.current_ms - 10000;
                    } else if (key.scancode == 0x4D) {
                        /* Right arrow: seek forward 10s */
                        seek_target = yt.current_ms + 10000;
                        if (seek_target > yt.duration_ms)
                            seek_target = yt.duration_ms;
                    }
                    if (seek_target != 0xFFFFFFFF) {
                        /* Flush stale audio before seek */
                        if (yt.sb_started) {
                            sb_stop();
                            yt.sb_started = 0;
                        }
                        yt.audio_msgs = 0;
                        yt_send_seek(ctx, seek_target);
                    }
                }
            }
        }

        /* 5. Handle mouse — click on progress bar to seek */
        {
            input_poll_mouse(&mouse);
            mouse.x /= 2;  /* Mode 13h: virtual pixels → screen pixels */
            if (input_mouse_clicked(&mouse, 0)) {
                if (mouse.y >= 188 && mouse.y < 200) {
                    /* Click on progress bar area */
                    if (mouse.x >= yt.track_x1 && mouse.x < yt.track_x2
                        && yt.duration_ms > 0) {
                        /* Seek to clicked position */
                        uint32_t target_ms =
                            (uint32_t)(mouse.x - yt.track_x1)
                            * yt.duration_ms
                            / (uint32_t)(yt.track_x2 - yt.track_x1);
                        if (target_ms > yt.duration_ms)
                            target_ms = yt.duration_ms;
                        /* Flush stale audio before seek */
                        if (yt.sb_started) {
                            sb_stop();
                            yt.sb_started = 0;
                        }
                        yt.audio_msgs = 0;
                        yt_send_seek(ctx, target_ms);
                    }
                } else {
                    /* Click anywhere else — toggle pause */
                    if (yt.state == YT_STATE_PAUSED) {
                        yt_send_control(ctx, YT_ACTION_RESUME);
                        yt.state = YT_STATE_PLAYING;
                    } else if (yt.state == YT_STATE_PLAYING) {
                        yt_send_control(ctx, YT_ACTION_PAUSE);
                        yt.state = YT_STATE_PAUSED;
                    }
                }
            }
        }

        /* 6. Pump audio */
        if (yt.sb_started) sb_pump();
    }
    }  /* end while + display_interval scope */
    }  /* end mouse scope */

cleanup:
    /* Stop audio playback */
    if (yt.sb_started) {
        sb_stop();
        yt.sb_started = 0;
    }

    /* Disable near pointer access */
    if (yt.vga_base) {
        __djgpp_nearptr_disable();
        yt.vga_base = NULL;
    }

    /* Free framebuffer */
    if (yt.framebuf) {
        free(yt.framebuf);
        yt.framebuf = NULL;
    }

    /* Restore VESA mode */
    video_restore_vesa(vc);
    yt_set_palette();

    return disconnected ? 1 : 0;
}
