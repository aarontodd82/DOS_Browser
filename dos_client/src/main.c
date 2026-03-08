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

#define TILE_SIZE 16

static uint8_t payload_buf[MAX_PAYLOAD_SIZE];

/* Simple test palette: basic 16 colors + grayscale ramp */
static void set_test_palette(void)
{
    uint8_t pal[768];
    int i;

    memset(pal, 0, sizeof(pal));

    /* Standard 16 CGA/EGA colors */
    pal[0] = 0;   pal[1] = 0;   pal[2] = 0;       /* 0: Black */
    pal[3] = 0;   pal[4] = 0;   pal[5] = 168;     /* 1: Dark Blue */
    pal[6] = 0;   pal[7] = 168; pal[8] = 0;       /* 2: Dark Green */
    pal[9] = 0;   pal[10] = 168; pal[11] = 168;   /* 3: Dark Cyan */
    pal[12] = 168; pal[13] = 0;  pal[14] = 0;     /* 4: Dark Red */
    pal[15] = 168; pal[16] = 0;  pal[17] = 168;   /* 5: Dark Magenta */
    pal[18] = 168; pal[19] = 84; pal[20] = 0;     /* 6: Brown */
    pal[21] = 168; pal[22] = 168; pal[23] = 168;  /* 7: Light Gray */
    pal[24] = 84;  pal[25] = 84;  pal[26] = 84;   /* 8: Dark Gray */
    pal[27] = 84;  pal[28] = 84;  pal[29] = 252;  /* 9: Light Blue */
    pal[30] = 84;  pal[31] = 252; pal[32] = 84;   /* 10: Light Green */
    pal[33] = 84;  pal[34] = 252; pal[35] = 252;  /* 11: Light Cyan */
    pal[36] = 252; pal[37] = 84;  pal[38] = 84;   /* 12: Light Red */
    pal[39] = 252; pal[40] = 84;  pal[41] = 252;  /* 13: Light Magenta */
    pal[42] = 252; pal[43] = 252; pal[44] = 84;   /* 14: Yellow */
    pal[45] = 252; pal[46] = 252; pal[47] = 252;  /* 15: White */

    /* Grayscale ramp in entries 16-255 */
    for (i = 16; i < 256; i++) {
        uint8_t v = (uint8_t)((i - 16) * 255 / 239);
        pal[i * 3 + 0] = v;
        pal[i * 3 + 1] = v;
        pal[i * 3 + 2] = v;
    }

    video_set_palette(pal, 256);
}

/* Phase 1 splash screen */
static void show_splash(VideoConfig *vc)
{
    const char *title = "RetroSurf v0.1";
    const char *subtitle = "DOS Web Browser";
    const char *prompt;
    int tx, ty, sx, sy;

    set_test_palette();

    video_fill_rect(vc, 0, 0, vc->width, vc->height, 1);
    video_fill_rect(vc, 0, 0, vc->width, vc->chrome_height, 8);
    video_fill_rect(vc, 80, 4, vc->width - 88, vc->chrome_height - 8, 7);

    font_draw_string(vc->backbuffer, vc->width, 4, 4,
                     "[<][>][R]", 15, 8, FONT_LARGE);
    font_draw_string(vc->backbuffer, vc->width, 84, 4,
                     "https://retrosurf.local", 0, 7, FONT_LARGE);

    tx = (vc->width - font_string_width(title, FONT_LARGE)) / 2;
    ty = vc->height / 2 - 40;
    font_draw_string(vc->backbuffer, vc->width, tx, ty,
                     title, 14, 255, FONT_LARGE);

    sx = (vc->width - font_string_width(subtitle, FONT_MEDIUM)) / 2;
    sy = ty + 24;
    font_draw_string(vc->backbuffer, vc->width, sx, sy,
                     subtitle, 11, 255, FONT_MEDIUM);

    {
        char buf[80];
        sprintf(buf, "Mode: %ux%u %ubpp %s",
                vc->width, vc->height, vc->bpp,
                vc->has_lfb ? "LFB" : "Banked");
        tx = (vc->width - font_string_width(buf, FONT_SMALL)) / 2;
        font_draw_string(vc->backbuffer, vc->width, tx, sy + 24,
                         buf, 7, 255, FONT_SMALL);

        sprintf(buf, "Content: %ux%u  Tiles: %ux%u = %u",
                vc->content_width, vc->content_height,
                vc->tile_cols, vc->tile_rows, vc->tile_total);
        tx = (vc->width - font_string_width(buf, FONT_SMALL)) / 2;
        font_draw_string(vc->backbuffer, vc->width, tx, sy + 36,
                         buf, 7, 255, FONT_SMALL);
    }

    {
        int i, bx, by;
        bx = (vc->width - 16 * 20) / 2;
        by = sy + 56;
        for (i = 0; i < 16; i++)
            video_fill_rect(vc, bx + i * 20, by, 18, 18, i);
    }

    prompt = "Press any key to continue...";
    tx = (vc->width - font_string_width(prompt, FONT_MEDIUM)) / 2;
    font_draw_string(vc->backbuffer, vc->width, tx, vc->height - 30,
                     prompt, 15, 255, FONT_MEDIUM);

    video_flush_full(vc);
    while (!kbhit()) {}
    getch();
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
    MouseState mouse;
    int rc;
    msg_header_t header;
    uint16_t payload_len;
    server_hello_t server_hello;
    int quit = 0;
    int render_mode = 0;  /* 0 = screenshot, 1 = native */

    /* Initialize subsystems */
    cursor_init(&cursor);
    chrome_init(&chrome, vc);
    interact_init(&interact);
    memset(&mouse, 0, sizeof(mouse));

    /* Initialize native renderer */
    if (native_init(&native, vc) != 0) {
        printf("WARNING: Native renderer init failed\n");
    }

    chrome_set_status(&chrome, "Connected");

    rc = render_init(&render, vc);
    if (rc != 0) return 1;

    /* Initialize mouse (after video is set) */
    input_init_mouse(vc->width, vc->height);

    /* === Handshake === */
    {
        uint8_t hello_buf[64];
        int payload_size = proto_encode_client_hello(
            hello_buf, vc->width, vc->height,
            vc->bpp, TILE_SIZE, vc->chrome_height, 32, 0
        );
        rc = net_send_message(&ctx, MSG_CLIENT_HELLO, hello_buf, payload_size);
        if (rc != 0) goto disconnect;
    }

    /* Wait for SERVER_HELLO */
    while (1) {
        net_poll();
        rc = net_recv_message(&ctx, &header, payload_buf, &payload_len);
        if (rc < 0) goto disconnect;
        if (rc == 1) break;
    }
    if (header.msg_type != MSG_SERVER_HELLO) goto disconnect;
    proto_decode_server_hello(payload_buf, &server_hello);

    /* Wait for PALETTE */
    while (1) {
        net_poll();
        rc = net_recv_message(&ctx, &header, payload_buf, &payload_len);
        if (rc < 0) goto disconnect;
        if (rc == 1) break;
    }
    if (header.msg_type != MSG_PALETTE) goto disconnect;
    video_set_palette(payload_buf, payload_len / 3);

    /* Draw initial chrome */
    chrome_set_url(&chrome, cfg->home_url);
    chrome_set_status(&chrome, "Loading...");
    chrome_draw(&chrome, vc);
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
            if (rc < 0) { quit = 1; break; }
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
                break;

            case MSG_MODE_SWITCH:
                if (payload_len >= 1) {
                    int new_mode = payload_buf[0];
                    if (new_mode == 1) {
                        /* Switch to native mode (always reset) */
                        render_mode = 1;
                        native_reset(&native);
                        native.active = 1;
                        chrome_set_status(&chrome, "[N] Native mode");
                        chrome_draw_status(&chrome, vc);
                    } else if (new_mode == 0) {
                        /* Switch to screenshot mode */
                        render_mode = 0;
                        native.active = 0;
                        /* Clear content area for clean screenshot */
                        video_fill_rect(vc, 0, vc->chrome_height,
                                        vc->width, vc->content_height,
                                        0);
                        video_mark_dirty(vc, 0, vc->chrome_height,
                                         vc->width, vc->content_height);
                        chrome_set_status(&chrome, "[S] Screenshot mode");
                        chrome_draw_status(&chrome, vc);
                    }
                }
                break;

            case MSG_NATIVE_CONTENT:
                native_parse_content(&native, payload_buf, payload_len,
                                     header.flags);
                if (!(header.flags & FLAG_CONTINUED)) {
                    frame_received = 1;
                }
                break;

            case MSG_NATIVE_IMAGE:
                native_parse_image(&native, payload_buf, payload_len);
                native.needs_redraw = 1;  /* re-render to show image */
                break;

            default:
                break;
            }
        }
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
                            native_scroll(&native, -(int32_t)native.viewport_h);
                            handled = 1;
                            break;
                        case 0x51: /* Page Down */
                            native_scroll(&native, (int32_t)native.viewport_h);
                            handled = 1;
                            break;
                        case 0x47: /* Home */
                            native.scroll_y = 0;
                            native.needs_redraw = 1;
                            handled = 1;
                            break;
                        case 0x4F: /* End */
                            native.scroll_y = native.max_scroll_y;
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
                    mouse.y < chrome.status_y) {
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

            /* 6. Throttled mouse move (content area only) */
            if (input_should_send_mouse_move(&mouse) &&
                mouse.y > vc->chrome_height &&
                mouse.y < chrome.status_y) {
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
        if (mouse.y > vc->chrome_height && mouse.y < chrome.status_y) {
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

        /* 8. Save under + draw cursor at new position */
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

        /* 8b. Native mode: render content if dirty */
        if (render_mode == 1 && native.needs_redraw && native.hdr_parsed) {
            native_render(&native, vc);
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

disconnect:
    native_shutdown(&native);
    render_shutdown(&render);
    net_close(&ctx);
    net_shutdown();
    return 0;
}

int main(int argc, char *argv[])
{
    Config cfg;
    VideoConfig vc;
    int rc;

    printf("\n");
    printf("========================================\n");
    printf("  RetroSurf v0.1.0\n");
    printf("========================================\n\n");

    /* Load configuration */
    if (config_load(&cfg, "RETRO.CFG") == 0)
        printf("Config loaded from RETRO.CFG\n");
    else
        printf("Using default config\n");
    printf("  Server: %s:%u\n", cfg.server_ip, cfg.server_port);
    printf("  Home:   %s\n\n", cfg.home_url);

    if (argc > 1) strncpy(cfg.server_ip, argv[1], sizeof(cfg.server_ip) - 1);
    if (argc > 2) cfg.server_port = (uint16_t)atoi(argv[2]);

    /* Initialize fonts */
    printf("Loading fonts...\n");
    rc = font_init();
    if (rc != 0) {
        printf("ERROR: Font init failed\n");
        getch();
        return 1;
    }
    printf("Fonts loaded (8x8, 8x14, 8x16)\n");

    /* Initialize video */
    printf("Initializing video...\n");
    rc = video_init(&vc, cfg.video_mode);
    if (rc != 0) {
        printf("ERROR: Video init failed\n");
        getch();
        return 1;
    }

    /* Show splash screen */
    show_splash(&vc);

    /* Back to text mode for connection */
    video_shutdown(&vc);

    printf("\nPress any key to connect to server, ESC to quit.\n");
    {
        int key = getch();
        if (key == 27) {
            printf("Exiting.\n");
            return 0;
        }
    }

    /* Connect in text mode so user can see errors */
    {
        net_context_t ctx;
        rc = net_init();
        if (rc != 0) {
            printf("Network init failed. Press any key.\n");
            getch();
            return 1;
        }
        rc = net_connect(&ctx, cfg.server_ip, cfg.server_port);
        if (rc != 0) {
            printf("Connection failed: %s\n", ctx.error_msg);
            printf("Press any key to exit.\n");
            net_shutdown();
            getch();
            return 1;
        }
        printf("Connected! Starting browser...\n");

        /* Re-init video */
        rc = video_init(&vc, cfg.video_mode);
        if (rc != 0) {
            printf("ERROR: Video re-init failed\n");
            net_close(&ctx);
            net_shutdown();
            getch();
            return 1;
        }
        set_test_palette();

        /* Run browser (pass connected context) */
        run_browser(&cfg, &vc, &ctx);
    }

    /* Cleanup */
    video_shutdown(&vc);
    printf("RetroSurf exited.\n");
    printf("Press any key.\n");
    getch();
    return 0;
}
