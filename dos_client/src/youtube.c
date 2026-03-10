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

/* ---- Protocol helpers ---- */

static void yt_send_control(net_context_t *ctx, uint8_t action)
{
    uint8_t buf[1];
    buf[0] = action;
    net_send_message(ctx, MSG_YT_CONTROL, buf, 1);
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

    /* Clear framebuffer for first real frame */
    memset(yt.framebuf, 0, YT_FRAMEBUF_SIZE);

    /* === Main YouTube playback loop ===
     *
     * ARCHITECTURE: Decode and display are DECOUPLED.
     * - Decode: drain all available messages, write to framebuf only
     * - Display: on a fixed timer, copy dirty blocks to VGA
     *
     * This makes display timing independent of network arrival timing.
     * If frames queue up in TCP, they all decode to framebuf but only
     * the latest pixels hit VGA at the next display tick. */
    {
    clock_t display_interval = CLOCKS_PER_SEC / yt.fps;

    while (!quit) {
        /* 1. Drain all available network messages into framebuf */
        {
        int draining = 1;
        while (draining && !quit) {
            int prev_recv_pos;

            net_poll();
            if (yt.sb_started) sb_pump();

            rc = net_recv_message(ctx, &header, recv_buf, &payload_len);
            if (rc < 0) { quit = 1; disconnected = 1; break; }

            if (rc == 0) {
                /* Partial message — pump and retry briefly */
                if (ctx->recv_pos > 0) {
                    int retries = 0;
                    while (retries < 50) {
                        net_poll();
                        if (yt.sb_started) sb_pump();
                        prev_recv_pos = ctx->recv_pos;
                        rc = net_recv_message(ctx, &header, recv_buf,
                                              &payload_len);
                        if (rc != 0) break;
                        if (ctx->recv_pos == prev_recv_pos)
                            retries++;
                        else
                            retries = 0;
                    }
                    if (rc < 0) { quit = 1; disconnected = 1; break; }
                    if (rc == 0) { draining = 0; break; }
                } else {
                    draining = 0;
                    break;
                }
            }

            /* Process message — decode only, no VGA writes */
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

            if (yt.sb_started) sb_pump();
        }
        }

        if (quit) break;

        /* 2. Display: flush dirty blocks to VGA at fixed frame rate */
        {
            clock_t now = clock();
            if (now - yt.last_display >= display_interval) {
                yt_flush_dirty(&yt);
                yt.last_display = now;
            }
        }

        /* 3. Handle keyboard */
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
                }
            }
        }

        /* 4. Pump audio + poll between cycles */
        if (yt.sb_started) sb_pump();
        net_poll();
    }
    }

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
