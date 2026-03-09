/*
 * RetroSurf DOS Client - Full Browser
 *
 * Phase 1: Static display (video, fonts, palette)       [DONE]
 * Phase 2: Network rendering (RLE/XOR tiles)             [DONE]
 * Phase 3: Mouse cursor and input (click, scroll, keys)  [DONE]
 * Phase 4: Browser chrome (address bar, nav buttons)  [DONE]
 * Phase 5: Interaction map + forwarding mode + cursors [DONE]
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <conio.h>

#include <tcp.h>

#include "protocol.h"
#include "network.h"
#include "config.h"
#include "font.h"
#include "video.h"
#include "render.h"
#include "input.h"
#include "cursor.h"
#include "chrome.h"
#include "interact.h"
#include "native.h"
#include "scrollbar.h"
#include "youtube.h"

#define TILE_SIZE 16

static uint8_t payload_buf[MAX_PAYLOAD_SIZE];

/* 6x6x6 RGB cube palette — matches the server palette so chrome
 * colors (black=0, white=215, light gray=172) work from startup. */
static void set_startup_palette(void)
{
    uint8_t pal[768];
    int r, g, b, i;

    memset(pal, 0, sizeof(pal));

    /* 6x6x6 RGB cube (indices 0-215) */
    for (r = 0; r < 6; r++) {
        for (g = 0; g < 6; g++) {
            for (b = 0; b < 6; b++) {
                i = r * 36 + g * 6 + b;
                pal[i * 3 + 0] = (uint8_t)(r * 51);
                pal[i * 3 + 1] = (uint8_t)(g * 51);
                pal[i * 3 + 2] = (uint8_t)(b * 51);
            }
        }
    }

    /* Grayscale ramp (indices 216-239) */
    for (i = 216; i < 240; i++) {
        uint8_t v = (uint8_t)((i - 216) * 255 / 23);
        pal[i * 3 + 0] = v;
        pal[i * 3 + 1] = v;
        pal[i * 3 + 2] = v;
    }

    /* White at 249 (used by some chrome code paths) */
    pal[249 * 3 + 0] = 255;
    pal[249 * 3 + 1] = 255;
    pal[249 * 3 + 2] = 255;

    video_set_palette(pal, 256);
}

/* === Dial-up Connection Dialog === */

/* 6x6x6 palette colors for the dialog */
#define COL_DLG_BG       0     /* Black background */
#define COL_DLG_BORDER   18    /* Dark green (0,153,0) */
#define COL_DLG_TITLE_BG 3     /* Dark blue (0,0,153) */
#define COL_DLG_TITLE_FG 215   /* White */
#define COL_DLG_TEXT     30    /* Bright green (0,255,0) */
#define COL_DLG_DIM      18    /* Dark green for secondary text */
#define COL_DLG_ERR      180   /* Red (255,0,0) */

#define DLG_W         340
#define DLG_H         170
#define DLG_TITLE_H    18
#define DLG_PAD         8
#define DLG_LINE_H     12
#define DLG_MAX_LINES  10

typedef struct {
    uint16_t x, y, w, h;
    int      line_count;
    char     lines[DLG_MAX_LINES][52];
    uint8_t  line_colors[DLG_MAX_LINES];
} ConnectDialog;

static void dialog_init(ConnectDialog *dlg, VideoConfig *vc)
{
    uint16_t content_top = vc->chrome_height;
    uint16_t content_h = vc->content_height;

    dlg->w = DLG_W;
    dlg->h = DLG_H;
    dlg->x = (vc->width - dlg->w) / 2;
    dlg->y = content_top + (content_h - dlg->h) / 2;
    dlg->line_count = 0;
}

static void dialog_draw(ConnectDialog *dlg, VideoConfig *vc)
{
    int i, tx, ty;
    const char *title = "RetroSurf - Connecting";

    /* Border (dark green frame) */
    video_fill_rect(vc, dlg->x - 2, dlg->y - 2,
                    dlg->w + 4, dlg->h + 4, COL_DLG_BORDER);

    /* Black interior */
    video_fill_rect(vc, dlg->x, dlg->y, dlg->w, dlg->h, COL_DLG_BG);

    /* Title bar (dark blue) */
    video_fill_rect(vc, dlg->x, dlg->y, dlg->w, DLG_TITLE_H,
                    COL_DLG_TITLE_BG);

    /* Title text (centered) */
    tx = dlg->x + (dlg->w - font_string_width(title, FONT_MEDIUM)) / 2;
    ty = dlg->y + (DLG_TITLE_H - font_char_height(FONT_MEDIUM)) / 2;
    font_draw_string(vc->backbuffer, vc->width, tx, ty,
                     title, COL_DLG_TITLE_FG, COL_DLG_TITLE_BG,
                     FONT_MEDIUM);

    /* Separator below title */
    video_fill_rect(vc, dlg->x, dlg->y + DLG_TITLE_H,
                    dlg->w, 1, COL_DLG_BORDER);

    /* Modem text lines */
    for (i = 0; i < dlg->line_count && i < DLG_MAX_LINES; i++) {
        tx = dlg->x + DLG_PAD;
        ty = dlg->y + DLG_TITLE_H + 6 + i * DLG_LINE_H;
        font_draw_string(vc->backbuffer, vc->width, tx, ty,
                         dlg->lines[i], dlg->line_colors[i],
                         COL_DLG_BG, FONT_SMALL);
    }

    video_mark_dirty(vc, dlg->x - 2, dlg->y - 2,
                     dlg->w + 4, dlg->h + 4);
}

static void dialog_add_line(ConnectDialog *dlg, VideoConfig *vc,
                             const char *text, uint8_t color)
{
    if (dlg->line_count >= DLG_MAX_LINES) return;
    strncpy(dlg->lines[dlg->line_count], text, 51);
    dlg->lines[dlg->line_count][51] = '\0';
    dlg->line_colors[dlg->line_count] = color;
    dlg->line_count++;
    dialog_draw(dlg, vc);
    video_flush_dirty(vc);
}

static void dialog_update_last(ConnectDialog *dlg, VideoConfig *vc,
                                const char *text, uint8_t color)
{
    int i;
    if (dlg->line_count <= 0) return;
    i = dlg->line_count - 1;
    strncpy(dlg->lines[i], text, 51);
    dlg->lines[i][51] = '\0';
    dlg->line_colors[i] = color;
    dialog_draw(dlg, vc);
    video_flush_dirty(vc);
}

/* Small delay that keeps the TCP stack alive */
static void delay_ms(int ms)
{
    clock_t end = clock() + (clock_t)ms * CLOCKS_PER_SEC / 1000;
    while (clock() < end) {
        net_poll();
    }
}

/* Process a CURSOR_SHAPE message */
static void handle_cursor_shape(SoftCursor *cur,
                                const uint8_t *payload, uint16_t len)
{
    if (len < 5) return;
    {
        uint8_t cursor_type = payload[0];
        uint8_t hx = payload[1];
        uint8_t hy = payload[2];
        uint8_t w = payload[3];
        uint8_t h = payload[4];

        if (cursor_type < 4) {
            cursor_set_shape(cur, cursor_type);
        } else if (len >= 5 + w * h) {
            cursor_set_custom(cur, w, h, hx, hy, payload + 5);
        }
    }
}

/* Send a NAV_ACTION to the server */
static void send_nav_action(net_context_t *ctx, uint8_t action)
{
    uint8_t buf[4];
    int len = proto_encode_nav_action(buf, action);
    net_send_message(ctx, MSG_NAV_ACTION, buf, len);
}

/* Full browser main loop */
static int run_browser(Config *cfg, VideoConfig *vc, net_context_t *passed_ctx)
{
    net_context_t ctx = *passed_ctx;
    RenderContext render;
    SoftCursor cursor;
    ChromeState chrome;
    InteractCtx interact;
    NativeCtx native;
    ScrollbarState scrollbar;
    MouseState mouse;
    int rc;
    msg_header_t header;
    uint16_t payload_len;
    int quit = 0;
    int disconnected = 0;
    int render_mode = 0;  /* 0 = screenshot, 1 = native */

    /* Initialize subsystems */
    cursor_init(&cursor);
    chrome_init(&chrome, vc);
    interact_init(&interact);
    scrollbar_init(&scrollbar, vc);
    memset(&mouse, 0, sizeof(mouse));

    /* Initialize native renderer */
    if (native_init(&native, vc) != 0) {
        printf("WARNING: Native renderer init failed\n");
    }

    rc = render_init(&render, vc);
    if (rc != 0) return 1;

    /* Initialize mouse (after video is set) */
    input_init_mouse(vc->width, vc->height);

    /* Draw initial chrome (handshake already done by caller) */
    chrome_set_url(&chrome, cfg->home_url);
    chrome_set_status(&chrome, "Loading...");
    chrome_draw(&chrome, vc);
    /* Clear content area and draw scrollbar */
    video_fill_rect(vc, 0, vc->chrome_height, vc->content_width,
                    vc->content_height, 0);
    scrollbar_draw(&scrollbar, vc);
    video_flush_full(vc);

    /* === Main Loop === */
    {
    int frame_received = 0;
    int had_activity = 0;
    while (!quit) {

        /* 1. Poll mouse position */
        input_poll_mouse(&mouse);

        /* 2. Restore cursor (remove from backbuffer before rendering) */
        cursor_restore(&cursor, vc->backbuffer, vc->width,
                       vc->width, vc->height);

        /* 3. Process network messages — stay in tight loop until drained.
         * For partial messages, keep retrying with a progress check:
         * allow up to 50 retries with no new data (network stall),
         * but reset the counter whenever data actually arrives.
         * This handles 10Mbps links where a 50KB frame takes ~40ms. */
        net_poll();
        {
        int recv_retries = 0;
        int prev_recv_pos = 0;
        while (1) {
            prev_recv_pos = ctx.recv_pos;
            rc = net_recv_message(&ctx, &header, payload_buf, &payload_len);
            if (rc < 0) { quit = 1; disconnected = 1; break; }
            if (rc == 0) {
                /* No data yet - if mid-message, pump TCP and retry */
                if (ctx.recv_pos > 0) {
                    if (ctx.recv_pos > prev_recv_pos) {
                        recv_retries = 0; /* progress - reset */
                    }
                    if (recv_retries < 50) {
                        net_poll();
                        recv_retries++;
                        continue;
                    }
                }
                break;
            }
            recv_retries = 0;

            had_activity = 1;
            switch (header.msg_type) {
            case MSG_FRAME_FULL:
            case MSG_FRAME_DELTA:
                /* Shift backbuffer to match server's scroll optimization */
                if (header.reserved != 0) {
                    video_shift_content(vc, header.reserved, 0);
                    scrollbar_draw(&scrollbar, vc);
                    vc->full_flush = 1;
                }
                render_apply_frame(&render, vc, payload_buf, payload_len);
                /* Mark frame complete when last chunk (no CONTINUED flag) */
                if (!(header.flags & FLAG_CONTINUED)) {
                    frame_received = 1;
                }
                break;

            case MSG_PALETTE:
                video_set_palette(payload_buf, payload_len / 3);
                break;

            case MSG_STATUS: {
                /* Don't overwrite DIRECT mode indicator */
                if (interact.mode != INTERACT_MODE_FORWARD) {
                    int slen = payload_len < 127 ? payload_len : 127;
                    char status_buf[128];
                    memcpy(status_buf, payload_buf, slen);
                    status_buf[slen] = '\0';
                    chrome_set_status(&chrome, status_buf);
                    chrome_draw_status(&chrome, vc);
                }
                break;
            }

            case MSG_CURSOR_SHAPE:
                handle_cursor_shape(&cursor, payload_buf, payload_len);
                break;

            case MSG_INTERACTION_MAP:
                interact_parse_map(&interact, payload_buf, payload_len);
                /* Update scrollbar from interaction map scroll info */
                if (scrollbar_update(&scrollbar,
                                     interact.map.scroll_height,
                                     vc->content_height,
                                     interact.map.scroll_y)) {
                    scrollbar_draw(&scrollbar, vc);
                }
                break;

            case MSG_MODE_SWITCH:
                if (payload_len >= 1) {
                    int new_mode = payload_buf[0];
                    if (new_mode == 2) {
                        /* YouTube mode - enter video player */
                        render_mode = 2;
                    } else if (new_mode == 1) {
                        /* Switch to native mode (always reset) */
                        render_mode = 1;
                        native_reset(&native);
                        native.active = 1;
                        chrome_set_status(&chrome, "[N] Native mode");
                        chrome_draw_status(&chrome, vc);
                        /* Reset scrollbar for new content */
                        scrollbar_update(&scrollbar, 0, vc->content_height, 0);
                        scrollbar_draw(&scrollbar, vc);
                    } else if (new_mode == 0) {
                        /* Switch to screenshot mode */
                        render_mode = 0;
                        native.active = 0;
                        /* Clear content area for clean screenshot */
                        video_fill_rect(vc, 0, vc->chrome_height,
                                        vc->content_width,
                                        vc->content_height, 0);
                        video_mark_dirty(vc, 0, vc->chrome_height,
                                         vc->content_width,
                                         vc->content_height);
                        chrome_set_status(&chrome, "[S] Screenshot mode");
                        chrome_draw_status(&chrome, vc);
                        scrollbar_update(&scrollbar, 0, vc->content_height, 0);
                        scrollbar_draw(&scrollbar, vc);
                    }
                }
                break;

            case MSG_GLYPH_CACHE:
                native_parse_glyph_cache(&native, payload_buf,
                                          payload_len, header.flags);
                break;

            case MSG_NATIVE_CONTENT:
                native_parse_content(&native, payload_buf, payload_len,
                                     header.flags);
                if (!(header.flags & FLAG_CONTINUED)) {
                    frame_received = 1;
                }
                break;

            case MSG_NATIVE_IMAGE:
                native_parse_image(&native, payload_buf, payload_len,
                                    header.flags);
                if (!(header.flags & FLAG_CONTINUED)) {
                    native.scroll_pending_dy = 0;  /* force full redraw */
                    native.needs_redraw = 1;  /* re-render to show image */
                }
                break;

            default:
                break;
            }
        }
        }

        /* YouTube mode: enter video player, blocking until it returns */
        if (render_mode == 2) {
            int yt_rc;
            cursor_restore(&cursor, vc->backbuffer, vc->width,
                           vc->width, vc->height);
            yt_rc = run_youtube(cfg, vc, &ctx, payload_buf);
            render_mode = 0;
            if (yt_rc == 1) { quit = 1; disconnected = 1; break; }
            /* Re-initialize mouse for VESA resolution (Mode 13h was 320x200) */
            input_init_mouse(vc->width, vc->height);
            /* Restore browser chrome.
             * NOTE: do NOT call set_startup_palette() here — yt_set_palette()
             * already set the complete 256-entry palette. set_startup_palette()
             * is missing indices 240-255 which causes black dots on whites. */
            chrome_draw(&chrome, vc);
            video_fill_rect(vc, 0, vc->chrome_height,
                            vc->content_width, vc->content_height, 0);
            scrollbar_draw(&scrollbar, vc);
            chrome_set_status(&chrome, "Loading...");
            chrome_draw_status(&chrome, vc);
            video_flush_full(vc);
            /* Force server to send a fresh frame by reloading the page.
             * The push_loop may be stuck in idle after YouTube mode. */
            send_nav_action(&ctx, NAV_RELOAD);
            continue;  /* restart main loop */
        }

        /* Send FRAME_ACK immediately after receiving — BEFORE VGA flush.
         * This lets the server start capturing the next frame while we
         * render the current one to VGA (pipelining). */
        if (frame_received) {
            net_send_message(&ctx, MSG_ACK, NULL, 0);
            frame_received = 0;
        }

        /* Extra TCP processing to keep ACKs flowing */
        net_poll();

        /* 4. Handle keyboard */
        {
            KeyEvent key;
            while (input_poll_key(&key)) {
                /* Priority 1: URL bar focused → chrome editing */
                if (chrome.url_focused) {
                    int result = chrome_handle_key(&chrome, key.ascii,
                                                   key.scancode, key.extended);
                    if (result == 1) {
                        /* Enter - navigate to URL */
                        uint8_t nbuf[CHROME_MAX_URL + 8];
                        int nlen = proto_encode_navigate(nbuf, chrome.url);
                        net_send_message(&ctx, MSG_NAVIGATE, nbuf, nlen);
                        chrome_set_status(&chrome, "Loading...");
                        chrome_draw(&chrome, vc);
                    } else if (result == 2) {
                        /* Escape - unfocused */
                        chrome_draw_urlbar(&chrome, vc);
                    } else {
                        /* Normal editing */
                        chrome_draw_urlbar(&chrome, vc);
                    }
                    continue;
                }

                /* Priority 2: Native mode → handle scroll keys locally */
                if (render_mode == 1) {
                    int handled = 0;
                    if (key.extended) {
                        switch (key.scancode) {
                        case 0x48: /* Up arrow */
                            native_scroll(&native, -font_char_height(1));
                            handled = 1;
                            break;
                        case 0x50: /* Down arrow */
                            native_scroll(&native, font_char_height(1));
                            handled = 1;
                            break;
                        case 0x49: /* Page Up */
                            /* 5 lines × 48px = 240px, matches screenshot mode */
                            native_scroll(&native, -(5 * 48));
                            handled = 1;
                            break;
                        case 0x51: /* Page Down */
                            native_scroll(&native, (5 * 48));
                            handled = 1;
                            break;
                        case 0x47: /* Home */
                            native.scroll_y = 0;
                            native.scroll_pending_dy = 0;
                            native.needs_redraw = 1;
                            handled = 1;
                            break;
                        case 0x4F: /* End */
                            native.scroll_y = native.max_scroll_y;
                            native.scroll_pending_dy = 0;
                            native.needs_redraw = 1;
                            handled = 1;
                            break;
                        }
                    }
                    /* ESC to quit (even in native mode) */
                    if (!handled && key.ascii == 27) {
                        quit = 1;
                        break;
                    }
                    if (handled) continue;
                    /* Fall through for unhandled keys */
                }

                /* Priority 3: Forwarding mode → send keys to server */
                if (interact.mode == INTERACT_MODE_FORWARD) {
                    /* Escape exits forwarding mode */
                    if (key.ascii == 27) {
                        interact.mode = INTERACT_MODE_NONE;
                        interact.forward_elem_id = 0;
                        chrome_set_status(&chrome, "");
                        chrome_draw_status(&chrome, vc);
                        continue;
                    }
                    /* Forward key to server */
                    {
                        uint8_t kbuf[4];
                        int klen;
                        if (key.extended) {
                            klen = proto_encode_key_event(kbuf,
                                        key.scancode, 0, 0, 0);
                        } else {
                            klen = proto_encode_key_event(kbuf,
                                        0, key.ascii, 0, 0);
                        }
                        net_send_message(&ctx, MSG_KEY_EVENT, kbuf, klen);
                    }
                    continue;
                }

                /* F5 = toggle rendering mode */
                if (key.extended && key.scancode == 0x3F) {
                    send_nav_action(&ctx, NAV_TOGGLE_MODE);
                    chrome_set_status(&chrome, "Switching mode...");
                    chrome_draw_status(&chrome, vc);
                    continue;
                }

                /* Priority 3: Normal mode - global keys */
                /* ESC to quit */
                if (key.ascii == 27) {
                    quit = 1;
                    break;
                }

                if (key.extended) {
                    uint8_t sbuf[4];
                    int slen;
                    switch (key.scancode) {
                    case 0x49: /* Page Up - scroll UP (direction=1) */
                        slen = proto_encode_scroll_event(sbuf, 1, 5);
                        net_send_message(&ctx, MSG_SCROLL_EVENT, sbuf, slen);
                        break;
                    case 0x51: /* Page Down - scroll DOWN (direction=0) */
                        slen = proto_encode_scroll_event(sbuf, 0, 5);
                        net_send_message(&ctx, MSG_SCROLL_EVENT, sbuf, slen);
                        break;
                    case 0x48: /* Up arrow - scroll UP (direction=1) */
                        slen = proto_encode_scroll_event(sbuf, 1, 1);
                        net_send_message(&ctx, MSG_SCROLL_EVENT, sbuf, slen);
                        break;
                    case 0x50: /* Down arrow - scroll DOWN (direction=0) */
                        slen = proto_encode_scroll_event(sbuf, 0, 1);
                        net_send_message(&ctx, MSG_SCROLL_EVENT, sbuf, slen);
                        break;
                    default:
                        /* Send as KEY_EVENT to server */
                        slen = proto_encode_key_event(sbuf, key.scancode,
                                                     0, 0, 0);
                        net_send_message(&ctx, MSG_KEY_EVENT, sbuf, slen);
                        break;
                    }
                } else {
                    /* Regular ASCII key → send to server */
                    uint8_t kbuf[4];
                    int klen = proto_encode_key_event(kbuf, 0, key.ascii,
                                                     0, 0);
                    net_send_message(&ctx, MSG_KEY_EVENT, kbuf, klen);
                }
            }
        }

        /* 5. Handle mouse clicks */
        if (input_mouse_clicked(&mouse, 0)) {
            int hit = chrome_hit_test(&chrome, mouse.x, mouse.y);

            switch (hit) {
            case CHROME_HIT_BACK:
                send_nav_action(&ctx, NAV_BACK);
                break;
            case CHROME_HIT_FORWARD:
                send_nav_action(&ctx, NAV_FORWARD);
                break;
            case CHROME_HIT_RELOAD:
                send_nav_action(&ctx, NAV_RELOAD);
                chrome_set_status(&chrome, "Reloading...");
                chrome_draw_status(&chrome, vc);
                break;
            case CHROME_HIT_STOP:
                send_nav_action(&ctx, NAV_STOP);
                break;
            case CHROME_HIT_URLBAR:
                /* Deactivate any content forwarding */
                interact_deactivate(&interact);
                chrome_focus_urlbar(&chrome);
                chrome_draw_urlbar(&chrome, vc);
                break;
            default:
                /* Content area click - unfocus URL bar */
                if (chrome.url_focused) {
                    chrome_unfocus_urlbar(&chrome);
                    chrome_draw_urlbar(&chrome, vc);
                }
                /* Scrollbar click — handle before content */
                if (scrollbar_contains(&scrollbar, mouse.x, mouse.y)) {
                    int sb_hit = scrollbar_hit_test(&scrollbar,
                                                     mouse.x, mouse.y);
                    if (render_mode == 1) {
                        /* Native mode: scroll locally */
                        switch (sb_hit) {
                        case SB_HIT_UP_ARROW:
                            native_scroll(&native, -font_char_height(1));
                            break;
                        case SB_HIT_DOWN_ARROW:
                            native_scroll(&native, font_char_height(1));
                            break;
                        case SB_HIT_TRACK_UP:
                            native_scroll(&native, -(5 * 48));
                            break;
                        case SB_HIT_TRACK_DOWN:
                            native_scroll(&native, (5 * 48));
                            break;
                        }
                    } else {
                        /* Screenshot mode: send scroll to server */
                        uint8_t sbuf[4];
                        int slen;
                        switch (sb_hit) {
                        case SB_HIT_UP_ARROW:
                            slen = proto_encode_scroll_event(sbuf, 1, 1);
                            net_send_message(&ctx, MSG_SCROLL_EVENT,
                                             sbuf, slen);
                            break;
                        case SB_HIT_DOWN_ARROW:
                            slen = proto_encode_scroll_event(sbuf, 0, 1);
                            net_send_message(&ctx, MSG_SCROLL_EVENT,
                                             sbuf, slen);
                            break;
                        case SB_HIT_TRACK_UP:
                            slen = proto_encode_scroll_event(sbuf, 1, 5);
                            net_send_message(&ctx, MSG_SCROLL_EVENT,
                                             sbuf, slen);
                            break;
                        case SB_HIT_TRACK_DOWN:
                            slen = proto_encode_scroll_event(sbuf, 0, 5);
                            net_send_message(&ctx, MSG_SCROLL_EVENT,
                                             sbuf, slen);
                            break;
                        }
                    }
                    input_mouse_event_sent(mouse.x, mouse.y);
                    break;
                }
                /* Only process if actually in content area */
                if (mouse.y > vc->chrome_height &&
                    mouse.y < chrome.status_y) {

                    if (render_mode == 1) {
                        /* Native mode: hit test against link rects */
                        int link_id = native_hit_test(&native,
                                                       mouse.x, mouse.y);
                        if (link_id >= 0) {
                            uint8_t cbuf[4];
                            int clen = proto_encode_native_click(
                                cbuf, (uint16_t)link_id);
                            net_send_message(&ctx, MSG_NATIVE_CLICK,
                                             cbuf, clen);
                            chrome_set_status(&chrome, "Loading...");
                            chrome_draw_status(&chrome, vc);
                        } else {
                            /* No link hit — send click to server.
                             * Server checks if a form element is at
                             * these coords and only switches to
                             * screenshot mode if so. */
                            uint16_t mx = mouse.x;
                            uint16_t my = mouse.y - vc->chrome_height;
                            uint8_t mbuf[6];
                            int mlen = proto_encode_mouse_event(
                                mbuf, mx, my, mouse.buttons,
                                MOUSE_CLICK);
                            net_send_message(&ctx, MSG_MOUSE_EVENT,
                                             mbuf, mlen);
                        }
                    } else {
                        /* Screenshot mode: use interaction map */
                        uint16_t mx = mouse.x;
                        uint16_t my = mouse.y - vc->chrome_height;
                        InteractElem *elem;

                        elem = interact_hit_test(&interact, mx, my);
                        if (elem) {
                            interact_handle_click(&interact, elem, &ctx,
                                                  mx, my, mouse.buttons);
                            if (interact.mode == INTERACT_MODE_FORWARD) {
                                chrome_set_status(&chrome,
                                    "DIRECT mode (ESC to exit)");
                                chrome_draw_status(&chrome, vc);
                            }
                        } else {
                            interact_handle_miss(&interact, &ctx,
                                                 mx, my, mouse.buttons);
                        }
                    }
                }
                break;
            }
            input_mouse_event_sent(mouse.x, mouse.y);
        }
        if (render_mode == 0) {
            /* Screenshot mode: send mouse release and move to server */
            if (input_mouse_released(&mouse, 0)) {
                if (mouse.y > vc->chrome_height &&
                    mouse.y < chrome.status_y &&
                    !scrollbar_contains(&scrollbar, mouse.x, mouse.y)) {
                    uint16_t mx = mouse.x;
                    uint16_t my = mouse.y - vc->chrome_height;
                    uint8_t mbuf[6];
                    int mlen = proto_encode_mouse_event(mbuf, mx, my,
                                                        mouse.buttons,
                                                        MOUSE_RELEASE);
                    net_send_message(&ctx, MSG_MOUSE_EVENT, mbuf, mlen);
                }
                input_mouse_event_sent(mouse.x, mouse.y);
            }

            /* 6. Throttled mouse move (content area only, not scrollbar) */
            if (input_should_send_mouse_move(&mouse) &&
                mouse.y > vc->chrome_height &&
                mouse.y < chrome.status_y &&
                !scrollbar_contains(&scrollbar, mouse.x, mouse.y)) {
                uint16_t mx = mouse.x;
                uint16_t my = mouse.y - vc->chrome_height;
                uint8_t mbuf[6];
                int mlen = proto_encode_mouse_event(mbuf, mx, my,
                                                    mouse.buttons, MOUSE_MOVE);
                net_send_message(&ctx, MSG_MOUSE_EVENT, mbuf, mlen);
                input_mouse_event_sent(mouse.x, mouse.y);
            }
        }

        /* 7. Tick cursor blink */
        if (chrome_tick_cursor(&chrome)) {
            chrome_draw_urlbar(&chrome, vc);
        }

        /* 7b. Update cursor shape based on mode */
        if (scrollbar_contains(&scrollbar, mouse.x, mouse.y)) {
            cursor_set_shape(&cursor, CURSOR_ARROW);
        } else if (mouse.y > vc->chrome_height &&
                   mouse.y < chrome.status_y) {
            if (render_mode == 1) {
                native_update_cursor(&native, &cursor,
                                     mouse.x, mouse.y);
            } else {
                interact_update_cursor(&interact, &cursor,
                                       mouse.x,
                                       mouse.y - vc->chrome_height);
            }
        } else {
            cursor_set_shape(&cursor, CURSOR_ARROW);
        }

        /* 8. Native mode: render content if dirty (BEFORE cursor draw,
         *    so memmove doesn't bake cursor into shifted content) */
        if (render_mode == 1 && native.needs_redraw && native.hdr_parsed) {
            native_render(&native, vc);
            /* Update scrollbar after native scroll/render */
            if (scrollbar_update(&scrollbar, native.hdr_content_height,
                                 vc->content_height, native.scroll_y)) {
                scrollbar_draw(&scrollbar, vc);
            }
        }

        /* 8b. Save under + draw cursor at new position */
        cursor_save_and_draw(&cursor, vc->backbuffer, vc->width,
                             vc->width, vc->height,
                             mouse.x, mouse.y);

        /* Mark cursor area dirty */
        {
            int cx = mouse.x - cursor.hotspot_x;
            int cy = mouse.y - cursor.hotspot_y;
            if (cx < 0) cx = 0;
            if (cy < 0) cy = 0;
            video_mark_dirty(vc, cx, cy, cursor.width, cursor.height);
            /* Also mark old position if moved */
            if (input_mouse_moved(&mouse) && cursor.save_x >= 0) {
                video_mark_dirty(vc, mouse.prev_x - cursor.hotspot_x,
                                 mouse.prev_y - cursor.hotspot_y,
                                 cursor.width, cursor.height);
            }
        }

        /* 9. Pump TCP before flush to keep ACKs flowing */
        net_poll();

        /* 10. Flush dirty regions to VGA */
        video_flush_dirty(vc);

        /* 11. Idle delay: reduce CPU waste when nothing is happening */
        if (!had_activity) {
            int i;
            for (i = 0; i < 5; i++) {
                net_poll();
                if (net_data_ready()) break;
            }
        }
        had_activity = 0;
    }
    }

    native_shutdown(&native);
    render_shutdown(&render);
    net_close(&ctx);
    /* 0 = user quit (ESC), 1 = disconnected (server went away) */
    return disconnected ? 1 : 0;
}

int main(int argc, char *argv[])
{
    Config cfg;
    VideoConfig vc;
    net_context_t ctx;
    ConnectDialog dlg;
    ChromeState boot_chrome;
    msg_header_t header;
    uint16_t payload_len;
    int rc;
    int reconnecting = 0;

    printf("\n  RetroSurf v0.1.0\n\n");

    /* Load configuration */
    if (config_load(&cfg, "RETRO.CFG") == 0)
        printf("Config: RETRO.CFG  Server: %s:%u\n",
               cfg.server_ip, cfg.server_port);
    else
        printf("Using defaults  Server: %s:%u\n",
               cfg.server_ip, cfg.server_port);

    if (argc > 1) strncpy(cfg.server_ip, argv[1], sizeof(cfg.server_ip) - 1);
    if (argc > 2) cfg.server_port = (uint16_t)atoi(argv[2]);

    /* Initialize fonts (text mode) */
    rc = font_init();
    if (rc != 0) {
        printf("ERROR: Font init failed\n");
        getch();
        return 1;
    }

    /* Initialize TCP/IP stack (text mode — prints debug info) */
    rc = net_init();
    if (rc != 0) {
        printf("Network init failed. Press any key.\n");
        getch();
        return 1;
    }

    /* === Switch to graphics mode === */
    rc = video_init(&vc, cfg.video_mode);
    if (rc != 0) {
        printf("ERROR: Video init failed\n");
        net_shutdown();
        getch();
        return 1;
    }

    /* Set up 6x6x6 palette so chrome colors work immediately */
    set_startup_palette();

    /* ============================================================
     * Connection loop — handles initial connect AND reconnection.
     * Retries indefinitely until server is reachable or ESC pressed.
     * ============================================================ */
connect:
    /* Draw browser chrome */
    chrome_init(&boot_chrome, &vc);
    chrome_set_url(&boot_chrome, cfg.home_url);
    chrome_set_status(&boot_chrome,
                      reconnecting ? "Reconnecting..." : "Connecting...");
    chrome_draw(&boot_chrome, &vc);
    video_fill_rect(&vc, 0, vc.chrome_height, vc.content_width,
                    vc.content_height, 0);
    video_flush_full(&vc);

    /* Init connection dialog */
    dialog_init(&dlg, &vc);

    if (reconnecting) {
        /* Reconnect: brief message then straight to dialing */
        dialog_add_line(&dlg, &vc, "Connection lost.", COL_DLG_ERR);
        dialog_add_line(&dlg, &vc, "", COL_DLG_TEXT);
        /* Reset palette in case server palette was weird */
        set_startup_palette();
    } else {
        /* First connect: modem init sequence (cosmetic) */
        dialog_add_line(&dlg, &vc, "ATZ", COL_DLG_DIM);
        delay_ms(300);
        dialog_update_last(&dlg, &vc,
            "ATZ                                  OK", COL_DLG_TEXT);
        delay_ms(200);
        dialog_add_line(&dlg, &vc, "AT&F1", COL_DLG_DIM);
        delay_ms(250);
        dialog_update_last(&dlg, &vc,
            "AT&F1                                OK", COL_DLG_TEXT);
        delay_ms(200);
    }

    /* Show dial command */
    {
        char dial_str[80];
        sprintf(dial_str, "ATDT %s:%u", cfg.server_ip, cfg.server_port);
        dialog_add_line(&dlg, &vc, dial_str, COL_DLG_TEXT);
    }

    /* === Connection retry loop === */
    {
        int attempt = 0;
        int connected = 0;

        dialog_add_line(&dlg, &vc, "RINGING...", COL_DLG_DIM);

        while (!connected) {
            attempt++;

            /* Resolve host */
            rc = net_resolve_host(&ctx, cfg.server_ip);
            if (rc != 0) {
                char s[52];
                sprintf(s, "NO CARRIER - Retry %d...", attempt);
                dialog_update_last(&dlg, &vc, s, COL_DLG_ERR);
                delay_ms(3000);
                if (kbhit() && getch() == 27) goto cleanup;
                continue;
            }

            /* Start TCP connection */
            rc = net_start_connect(&ctx, cfg.server_port);
            if (rc != 0) {
                char s[52];
                sprintf(s, "NO CARRIER - Retry %d...", attempt);
                dialog_update_last(&dlg, &vc, s, COL_DLG_ERR);
                delay_ms(3000);
                if (kbhit() && getch() == 27) goto cleanup;
                continue;
            }

            /* Poll for TCP connection (5s per attempt) */
            {
                clock_t poll_deadline = clock() + 5 * CLOCKS_PER_SEC;
                clock_t last_anim = clock();
                int dots = 0;

                /* Show ringing with attempt count */
                if (attempt > 1) {
                    char s[52];
                    sprintf(s, "RINGING...              (attempt %d)",
                            attempt);
                    dialog_update_last(&dlg, &vc, s, COL_DLG_DIM);
                }

                while (clock() < poll_deadline) {
                    rc = net_poll_connect(&ctx);
                    if (rc == 1) { connected = 1; break; }
                    if (rc == -1) break; /* refused — will retry */

                    if (kbhit() && getch() == 27) goto cleanup;

                    /* Animate dots every ~500ms */
                    if (clock() - last_anim >= CLOCKS_PER_SEC / 2) {
                        char ring[52];
                        dots = (dots + 1) % 4;
                        if (attempt > 1)
                            sprintf(ring,
                                "RINGING%.*s             (attempt %d)",
                                dots, "...", attempt);
                        else
                            sprintf(ring, "RINGING%.*s   ",
                                    dots, "...");
                        dialog_update_last(&dlg, &vc, ring, COL_DLG_DIM);
                        last_anim = clock();
                    }
                }

                if (!connected) {
                    /* Timeout or refused — wait then retry */
                    char s[52];
                    sprintf(s, "NO CARRIER - Retry %d...", attempt + 1);
                    dialog_update_last(&dlg, &vc, s, COL_DLG_ERR);
                    delay_ms(3000);
                    if (kbhit() && getch() == 27) goto cleanup;
                }
            }
        }
    }

    /* Connected! */
    net_finish_connect(&ctx);
    dialog_update_last(&dlg, &vc, "CONNECT 9600", COL_DLG_TEXT);
    delay_ms(300);

    /* === Protocol handshake === */
    dialog_add_line(&dlg, &vc, "", COL_DLG_TEXT);
    dialog_add_line(&dlg, &vc, "Negotiating protocol...", COL_DLG_DIM);

    /* Send CLIENT_HELLO */
    {
        uint8_t hello_buf[64];
        int payload_size = proto_encode_client_hello(
            hello_buf, vc.content_width,
            vc.content_height + vc.chrome_height,
            vc.bpp, TILE_SIZE, vc.chrome_height, 32, 0
        );
        rc = net_send_message(&ctx, MSG_CLIENT_HELLO, hello_buf, payload_size);
        if (rc != 0) {
            /* Handshake send failed — reconnect */
            net_close(&ctx);
            reconnecting = 1;
            goto connect;
        }
    }

    /* Wait for SERVER_HELLO */
    {
        clock_t deadline = clock() + 10 * CLOCKS_PER_SEC;
        rc = 0;
        while (clock() < deadline) {
            net_poll();
            rc = net_recv_message(&ctx, &header, payload_buf, &payload_len);
            if (rc < 0) break;
            if (rc == 1) break;
        }
        if (rc != 1 || header.msg_type != MSG_SERVER_HELLO) {
            net_close(&ctx);
            reconnecting = 1;
            goto connect;
        }
    }

    /* Wait for PALETTE */
    {
        clock_t deadline = clock() + 10 * CLOCKS_PER_SEC;
        while (clock() < deadline) {
            net_poll();
            rc = net_recv_message(&ctx, &header, payload_buf, &payload_len);
            if (rc < 0) break;
            if (rc == 1) break;
        }
        if (rc == 1 && header.msg_type == MSG_PALETTE) {
            video_set_palette(payload_buf, payload_len / 3);
        }
    }

    dialog_update_last(&dlg, &vc,
        "Negotiating protocol...              OK", COL_DLG_TEXT);
    delay_ms(200);
    dialog_add_line(&dlg, &vc, "Loading homepage...", COL_DLG_TEXT);
    delay_ms(400);

    /* === Launch the browser === */
    rc = run_browser(&cfg, &vc, &ctx);

    if (rc == 1) {
        /* Disconnected — reconnect automatically */
        reconnecting = 1;
        goto connect;
    }

    /* rc == 0: user pressed ESC to quit */

cleanup:
    video_shutdown(&vc);
    net_shutdown();
    printf("RetroSurf exited.\n");
    return 0;
}
