/*
 * RetroSurf Browser Chrome - Address bar, nav buttons, status bar
 *
 * Layout (640-wide, 24px chrome):
 *   [<][>][R][X] | http://example.com__________________ |
 *   +--- Content area -----------------------------------+
 *   | Status bar text                                    | 12px
 *
 * The chrome owns the address bar editing state. When the address
 * bar is focused, keys go to local editing. Enter sends NAVIGATE.
 */

#ifndef RETROSURF_CHROME_H
#define RETROSURF_CHROME_H

#include <stdint.h>
#include "video.h"

/* Maximum URL length for the address bar */
#define CHROME_MAX_URL 256

/* Hit-test results */
#define CHROME_HIT_NONE      0   /* Click is in content area */
#define CHROME_HIT_BACK      1
#define CHROME_HIT_FORWARD   2
#define CHROME_HIT_RELOAD    3
#define CHROME_HIT_STOP      4
#define CHROME_HIT_URLBAR    5

/* Chrome state */
typedef struct {
    /* URL bar */
    char    url[CHROME_MAX_URL];
    int     url_len;
    int     cursor_pos;       /* Text cursor position in url[] */
    int     url_focused;      /* 1 if address bar has focus */
    int     url_scroll;       /* Horizontal scroll offset (chars) */

    /* Status bar */
    char    status[128];

    /* Layout (computed from VideoConfig) */
    uint16_t bar_y;           /* Top of chrome bar (always 0) */
    uint16_t bar_h;           /* Chrome bar height */
    uint16_t url_x;           /* Left edge of URL text area */
    uint16_t url_w;           /* Width of URL text area */
    uint16_t status_y;        /* Top of status bar */
    uint16_t status_h;        /* Status bar height */

    /* Button positions (x, width) - all at y=0, h=bar_h */
    uint16_t btn_back_x, btn_back_w;
    uint16_t btn_fwd_x, btn_fwd_w;
    uint16_t btn_reload_x, btn_reload_w;
    uint16_t btn_stop_x, btn_stop_w;

    /* Blink state for text cursor */
    int     cursor_visible;
    uint32_t cursor_blink_tick;
} ChromeState;

/* Initialize chrome layout from video config.
 * Sets up button positions, URL bar rect, status bar rect. */
void chrome_init(ChromeState *cs, VideoConfig *vc);

/* Draw the entire chrome (bar + status) to the backbuffer.
 * Call after chrome state changes or on full redraw. */
void chrome_draw(ChromeState *cs, VideoConfig *vc);

/* Draw just the URL bar area (for text editing updates). */
void chrome_draw_urlbar(ChromeState *cs, VideoConfig *vc);

/* Draw just the status bar. */
void chrome_draw_status(ChromeState *cs, VideoConfig *vc);

/* Hit-test a mouse click at (x, y).
 * Returns CHROME_HIT_xxx. CHROME_HIT_NONE means content area. */
int chrome_hit_test(ChromeState *cs, uint16_t x, uint16_t y);

/* Set the URL bar text (e.g. after navigation completes). */
void chrome_set_url(ChromeState *cs, const char *url);

/* Set the status bar text. */
void chrome_set_status(ChromeState *cs, const char *text);

/* Focus/unfocus the address bar. */
void chrome_focus_urlbar(ChromeState *cs);
void chrome_unfocus_urlbar(ChromeState *cs);

/* Handle a key press while the URL bar is focused.
 * Returns:
 *   0 = key consumed (normal editing)
 *   1 = Enter pressed - URL is ready in cs->url, caller should navigate
 *   2 = Escape pressed - unfocused, caller should resume normal input
 */
int chrome_handle_key(ChromeState *cs, uint8_t ascii, uint8_t scancode,
                      int extended);

/* Tick the cursor blink (call from main loop).
 * Returns 1 if the cursor visibility changed (needs redraw). */
int chrome_tick_cursor(ChromeState *cs);

#endif /* RETROSURF_CHROME_H */
