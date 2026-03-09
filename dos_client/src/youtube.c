/*
 * RetroSurf YouTube Mode - Video playback in Mode 13h
 *
 * Receives block-delta RLE video frames from the server and blits
 * them directly to VGA Mode 13h (320x200x256). Phase 1: silent video.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <conio.h>
#include <dpmi.h>
#include <go32.h>
#include <sys/farptr.h>
#include <tcp.h>

#include "youtube.h"
#include "protocol.h"
#include "network.h"
#include "video.h"
#include "font.h"
#include "input.h"
#include "render.h"  /* for rle_decode */

/* YouTube context (local to this module) */
typedef struct {
    uint8_t  state;
    uint16_t video_w, video_h;
    uint8_t  fps;
    uint32_t duration_ms;
    uint32_t current_ms;
    char     title[80];

    /* Software framebuffer (320x200 in extended memory) */
    uint8_t  *framebuf;
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

    /* Web UI accent colors (indices 240-249) — the server LUT maps
     * near-white pixels to index 247 (240,240,240). Without these
     * entries, those pixels render as BLACK. */
    pal[240*3+0] =   0; pal[240*3+1] = 102; pal[240*3+2] = 204; /* sel blue */
    pal[241*3+0] =   0; pal[241*3+1] =   0; pal[241*3+2] = 238; /* link blue */
    pal[242*3+0] = 204; pal[242*3+1] =   0; pal[242*3+2] =   0; /* error red */
    pal[243*3+0] =   0; pal[243*3+1] = 153; pal[243*3+2] =   0; /* success grn */
    pal[244*3+0] = 255; pal[244*3+1] = 204; pal[244*3+2] =   0; /* warn yellow */
    pal[245*3+0] = 192; pal[245*3+1] = 192; pal[245*3+2] = 192; /* border gray */
    pal[246*3+0] = 128; pal[246*3+1] = 128; pal[246*3+2] = 128; /* shadow gray */
    pal[247*3+0] = 240; pal[247*3+1] = 240; pal[247*3+2] = 240; /* light bg */
    pal[248*3+0] =  51; pal[248*3+1] =  51; pal[248*3+2] =  51; /* dark text */
    pal[249*3+0] = 255; pal[249*3+1] = 255; pal[249*3+2] = 255; /* white */

    /* DOS chrome colors (indices 250-254) */
    pal[250*3+0] =  64; pal[250*3+1] =  64; pal[250*3+2] =  64; /* chrome bg */
    pal[251*3+0] = 255; pal[251*3+1] = 255; pal[251*3+2] = 255; /* chrome text */
    pal[252*3+0] = 240; pal[252*3+1] = 240; pal[252*3+2] = 240; /* addr bar bg */
    pal[253*3+0] =   0; pal[253*3+1] =   0; pal[253*3+2] =   0; /* addr bar txt*/
    pal[254*3+0] =   0; pal[254*3+1] = 120; pal[254*3+2] = 215; /* highlight */

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

static void yt_send_ack(net_context_t *ctx, uint16_t audio_buffer_ms)
{
    uint8_t buf[2];
    buf[0] = (uint8_t)(audio_buffer_ms & 0xFF);
    buf[1] = (uint8_t)(audio_buffer_ms >> 8);
    net_send_message(ctx, MSG_YT_ACK, buf, 2);
}

/* ---- Frame parsing ---- */

static void yt_parse_start(YTContext *yt, const uint8_t *payload, uint16_t len)
{
    uint16_t title_len;

    if (len < 14) return;

    memcpy(&yt->video_w, payload + 0, 2);
    memcpy(&yt->video_h, payload + 2, 2);
    yt->fps = payload[4];
    /* skip audio_rate (2 bytes at offset 5) and audio_bits (1 byte at 7) */
    memcpy(&yt->duration_ms, payload + 8, 4);
    yt->duration_ms *= 1000;  /* convert seconds to ms */
    memcpy(&title_len, payload + 12, 2);

    if (title_len > 79) title_len = 79;
    if (14 + title_len <= len) {
        memcpy(yt->title, payload + 14, title_len);
    }
    yt->title[title_len] = '\0';
}

static void yt_apply_frame_chunk(YTContext *yt, const uint8_t *payload,
                                  uint16_t len)
{
    const uint8_t *ptr = payload;
    const uint8_t *end = payload + len;
    uint16_t block_count;
    uint8_t block_temp[YT_BLOCK_PIXELS];
    int i;

    /* Skip frame_num(4) + timestamp_ms(4) */
    if (len < 10) return;

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

        /* Blit 8x8 block to framebuffer */
        dst = yt->framebuf + (uint16_t)by * 8 * YT_VIDEO_W
              + (uint16_t)bx * 8;
        for (row = 0; row < YT_BLOCK_SIZE; row++) {
            memcpy(dst, block_temp + row * YT_BLOCK_SIZE, YT_BLOCK_SIZE);
            dst += YT_VIDEO_W;
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

    (void)cfg;

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

    yt.state = YT_STATE_PLAYING;

    /* Clear framebuffer for first real frame */
    memset(yt.framebuf, 0, YT_FRAMEBUF_SIZE);

    /* === Main YouTube playback loop === */
    while (!quit) {
        int frame_done = 0;
        int recv_retries = 0;
        int prev_recv_pos = 0;

        /* Poll network */
        net_poll();

        /* Receive and process messages */
        while (1) {
            prev_recv_pos = ctx->recv_pos;
            rc = net_recv_message(ctx, &header, recv_buf, &payload_len);
            if (rc < 0) { quit = 1; disconnected = 1; break; }
            if (rc == 0) {
                /* No data yet - retry if mid-message */
                if (ctx->recv_pos > 0) {
                    if (ctx->recv_pos > prev_recv_pos)
                        recv_retries = 0;
                    if (recv_retries < 50) {
                        net_poll();
                        recv_retries++;
                        continue;
                    }
                }
                break;
            }
            recv_retries = 0;

            switch (header.msg_type) {
            case MSG_YT_FRAME:
                yt_apply_frame_chunk(&yt, recv_buf, payload_len);
                if (!(header.flags & FLAG_CONTINUED)) {
                    frame_done = 1;
                }
                break;

            case MSG_YT_EOF:
                yt.state = YT_STATE_ENDED;
                quit = 1;
                break;

            case MSG_MODE_SWITCH:
                if (payload_len >= 1 && recv_buf[0] != 2) {
                    quit = 1;
                }
                break;

            case MSG_PALETTE:
                video_set_palette(recv_buf, payload_len / 3);
                break;

            default:
                break;
            }
        }

        /* Flush frame to VGA and send ACK */
        if (frame_done) {
            yt_flush_frame(&yt);
            yt_send_ack(ctx, 0);  /* audio_buffer_ms = 0 (Phase 1) */
            net_poll();  /* push ACK out immediately */
        }

        /* Handle keyboard */
        {
            KeyEvent key;
            while (input_poll_key(&key)) {
                if (key.ascii == 27) {
                    /* ESC - stop YouTube */
                    yt_send_control(ctx, YT_ACTION_STOP);
                    /* Drain briefly for clean shutdown */
                    {
                        clock_t drain = clock() + CLOCKS_PER_SEC;
                        while (clock() < drain) {
                            net_poll();
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
                    /* Space - toggle pause */
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

        /* Keep TCP alive */
        net_poll();

        /* Small idle delay when no data flowing */
        if (!frame_done) {
            int i;
            for (i = 0; i < 3; i++) {
                net_poll();
                if (net_data_ready()) break;
            }
        }
    }

cleanup:
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
