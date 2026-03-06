/*
 * RetroSurf Browser Chrome - Address bar, nav buttons, status bar
 */

#include <stdio.h>
#include <string.h>
#include <time.h>
#include "chrome.h"
#include "font.h"
#include "video.h"

/* Colors - using server's reserved chrome palette entries (250-254)
 * and known 6x6x6 cube positions for basic colors.
 * Palette layout: 0-215 = 6x6x6 RGB cube, 216-239 = grayscale,
 *   249 = white, 250 = chrome bg (dark), 252 = light bg, 253 = black text
 * In 6x6x6 cube: index = r*36 + g*6 + b, where r,g,b are 0-5.
 *   Black = 0*36+0*6+0 = 0
 *   White = 5*36+5*6+5 = 215
 *   Light gray ~= 4*36+4*6+4 = 184 (204,204,204)
 */
#define COL_BLACK        0    /* (0,0,0) = 0*36+0*6+0 */
#define COL_WHITE       215   /* (255,255,255) = 5*36+5*6+5 */
#define COL_LIGHT_GRAY  172   /* (204,204,204) = 4*36+4*6+4 */

#define COL_CHROME_BG    COL_LIGHT_GRAY
#define COL_CHROME_BORDER COL_BLACK
#define COL_BTN_BG       COL_WHITE
#define COL_BTN_TEXT     COL_BLACK
#define COL_URL_BG       COL_WHITE
#define COL_URL_TEXT     COL_BLACK
#define COL_URL_CURSOR   COL_BLACK
#define COL_URL_FOCUS    COL_WHITE
#define COL_STATUS_BG    COL_LIGHT_GRAY
#define COL_STATUS_TEXT  COL_BLACK

/* Cursor blink interval (clock ticks) */
#define CURSOR_BLINK_MS 500

/* Button label widths at FONT_LARGE (8px per char) */
#define BTN_PADDING 4
#define BTN_LABEL_CHARS 3  /* e.g. " < " */

void chrome_init(ChromeState *cs, VideoConfig *vc)
{
    int btn_w, btn_x;

    memset(cs, 0, sizeof(ChromeState));

    cs->bar_y = 0;
    cs->bar_h = vc->chrome_height;
    /* Status bar overlays the bottom of the screen */
    cs->status_h = vc->status_height;
    cs->status_y = vc->height - cs->status_h;

    /* Button layout: [<] [>] [R] [X] then URL bar fills the rest */
    btn_w = font_char_width(FONT_LARGE) * BTN_LABEL_CHARS + BTN_PADDING * 2;
    btn_x = 2;

    cs->btn_back_x = btn_x;
    cs->btn_back_w = btn_w;
    btn_x += btn_w + 2;

    cs->btn_fwd_x = btn_x;
    cs->btn_fwd_w = btn_w;
    btn_x += btn_w + 2;

    cs->btn_reload_x = btn_x;
    cs->btn_reload_w = btn_w;
    btn_x += btn_w + 2;

    cs->btn_stop_x = btn_x;
    cs->btn_stop_w = btn_w;
    btn_x += btn_w + 4;

    cs->url_x = btn_x;
    cs->url_w = vc->width - btn_x - 4;

    cs->cursor_blink_tick = clock();
}

/* Draw a button with a centered label and 1px border */
static void draw_button(VideoConfig *vc, uint16_t x, uint16_t y,
                         uint16_t w, uint16_t h, const char *label)
{
    int tx, ty;

    /* 1px border */
    video_fill_rect(vc, x, y, w, 1, COL_CHROME_BORDER);           /* top */
    video_fill_rect(vc, x, y + h - 1, w, 1, COL_CHROME_BORDER);   /* bottom */
    video_fill_rect(vc, x, y, 1, h, COL_CHROME_BORDER);           /* left */
    video_fill_rect(vc, x + w - 1, y, 1, h, COL_CHROME_BORDER);   /* right */

    /* Button interior */
    video_fill_rect(vc, x + 1, y + 1, w - 2, h - 2, COL_BTN_BG);

    /* Centered label */
    tx = x + (w - font_string_width(label, FONT_LARGE)) / 2;
    ty = y + (h - font_char_height(FONT_LARGE)) / 2;
    font_draw_string(vc->backbuffer, vc->width, tx, ty,
                     label, COL_BTN_TEXT, COL_BTN_BG, FONT_LARGE);
}

void chrome_draw(ChromeState *cs, VideoConfig *vc)
{
    int btn_y = cs->bar_y + 2;
    int btn_h = cs->bar_h - 4;

    /* Chrome bar background */
    video_fill_rect(vc, 0, cs->bar_y, vc->width, cs->bar_h, COL_CHROME_BG);

    /* Nav buttons - white bg, black text, 1px border */
    draw_button(vc, cs->btn_back_x, btn_y, cs->btn_back_w, btn_h, " < ");
    draw_button(vc, cs->btn_fwd_x, btn_y, cs->btn_fwd_w, btn_h, " > ");
    draw_button(vc, cs->btn_reload_x, btn_y, cs->btn_reload_w, btn_h, " R ");
    draw_button(vc, cs->btn_stop_x, btn_y, cs->btn_stop_w, btn_h, " X ");

    /* URL bar */
    chrome_draw_urlbar(cs, vc);

    /* Status bar */
    chrome_draw_status(cs, vc);

    /* Mark entire chrome + status as dirty */
    video_mark_dirty(vc, 0, 0, vc->width, cs->bar_h);
    video_mark_dirty(vc, 0, cs->status_y, vc->width, cs->status_h);
}

void chrome_draw_urlbar(ChromeState *cs, VideoConfig *vc)
{
    int url_y = cs->bar_y + 2;
    int url_h = cs->bar_h - 4;
    uint8_t bg = cs->url_focused ? COL_URL_FOCUS : COL_URL_BG;
    int text_x, text_y;
    int max_chars;
    int draw_start;
    char display_buf[CHROME_MAX_URL + 1];
    int display_len;

    /* 1px border around URL bar */
    video_fill_rect(vc, cs->url_x, url_y, cs->url_w, 1, COL_CHROME_BORDER);
    video_fill_rect(vc, cs->url_x, url_y + url_h - 1, cs->url_w, 1, COL_CHROME_BORDER);
    video_fill_rect(vc, cs->url_x, url_y, 1, url_h, COL_CHROME_BORDER);
    video_fill_rect(vc, cs->url_x + cs->url_w - 1, url_y, 1, url_h, COL_CHROME_BORDER);

    /* URL bar interior */
    video_fill_rect(vc, cs->url_x + 1, url_y + 1, cs->url_w - 2, url_h - 2, bg);

    /* Calculate how many chars fit */
    max_chars = (cs->url_w - 8) / font_char_width(FONT_LARGE);
    if (max_chars < 1) max_chars = 1;

    /* Auto-scroll to keep cursor visible */
    if (cs->cursor_pos < cs->url_scroll) {
        cs->url_scroll = cs->cursor_pos;
    }
    if (cs->cursor_pos > cs->url_scroll + max_chars) {
        cs->url_scroll = cs->cursor_pos - max_chars;
    }
    if (cs->url_scroll < 0) cs->url_scroll = 0;

    /* Extract visible portion of URL */
    draw_start = cs->url_scroll;
    display_len = cs->url_len - draw_start;
    if (display_len > max_chars) display_len = max_chars;
    if (display_len < 0) display_len = 0;
    memcpy(display_buf, cs->url + draw_start, display_len);
    display_buf[display_len] = '\0';

    /* Draw URL text */
    text_x = cs->url_x + 4;
    text_y = url_y + (url_h - font_char_height(FONT_LARGE)) / 2;
    font_draw_string(vc->backbuffer, vc->width, text_x, text_y,
                     display_buf, COL_URL_TEXT, bg, FONT_LARGE);

    /* Draw text cursor if focused and visible */
    if (cs->url_focused && cs->cursor_visible) {
        int cursor_x = text_x + (cs->cursor_pos - cs->url_scroll)
                        * font_char_width(FONT_LARGE);
        if (cursor_x >= cs->url_x && cursor_x < cs->url_x + cs->url_w - 2) {
            video_fill_rect(vc, cursor_x, text_y, 2,
                           font_char_height(FONT_LARGE), COL_URL_CURSOR);
        }
    }

    video_mark_dirty(vc, cs->url_x, url_y, cs->url_w, url_h);
}

void chrome_draw_status(ChromeState *cs, VideoConfig *vc)
{
    /* Fill entire status area (no gap) */
    video_fill_rect(vc, 0, cs->status_y, vc->width, cs->status_h,
                    COL_STATUS_BG);
    /* 1px top border */
    video_fill_rect(vc, 0, cs->status_y, vc->width, 1, COL_CHROME_BORDER);

    if (cs->status[0]) {
        /* Center text vertically in status bar, below the border */
        int text_y = cs->status_y + (cs->status_h - font_char_height(FONT_SMALL)) / 2;
        if (text_y < cs->status_y + 1) text_y = cs->status_y + 1;
        font_draw_string(vc->backbuffer, vc->width, 4, text_y,
                         cs->status, COL_STATUS_TEXT, COL_STATUS_BG,
                         FONT_SMALL);
    }
    video_mark_dirty(vc, 0, cs->status_y, vc->width, cs->status_h);
}

int chrome_hit_test(ChromeState *cs, uint16_t x, uint16_t y)
{
    /* Check if in chrome bar area */
    if (y < cs->bar_h) {
        if (x >= cs->btn_back_x && x < cs->btn_back_x + cs->btn_back_w)
            return CHROME_HIT_BACK;
        if (x >= cs->btn_fwd_x && x < cs->btn_fwd_x + cs->btn_fwd_w)
            return CHROME_HIT_FORWARD;
        if (x >= cs->btn_reload_x && x < cs->btn_reload_x + cs->btn_reload_w)
            return CHROME_HIT_RELOAD;
        if (x >= cs->btn_stop_x && x < cs->btn_stop_x + cs->btn_stop_w)
            return CHROME_HIT_STOP;
        if (x >= cs->url_x && x < cs->url_x + cs->url_w)
            return CHROME_HIT_URLBAR;
        return CHROME_HIT_NONE;  /* Clicked on chrome bg but not a button */
    }

    /* Status bar clicks - ignore */
    if (y >= cs->status_y)
        return CHROME_HIT_NONE;

    /* Content area */
    return CHROME_HIT_NONE;
}

void chrome_set_url(ChromeState *cs, const char *url)
{
    int len = strlen(url);
    if (len >= CHROME_MAX_URL) len = CHROME_MAX_URL - 1;
    memcpy(cs->url, url, len);
    cs->url[len] = '\0';
    cs->url_len = len;
    /* Don't move cursor if user is editing */
    if (!cs->url_focused) {
        cs->cursor_pos = len;
        cs->url_scroll = 0;
    }
}

void chrome_set_status(ChromeState *cs, const char *text)
{
    int len = strlen(text);
    if (len >= (int)sizeof(cs->status)) len = sizeof(cs->status) - 1;
    memcpy(cs->status, text, len);
    cs->status[len] = '\0';
}

void chrome_focus_urlbar(ChromeState *cs)
{
    cs->url_focused = 1;
    cs->cursor_visible = 1;
    cs->cursor_blink_tick = clock();
    /* Select all text (move cursor to end) */
    cs->cursor_pos = cs->url_len;
}

void chrome_unfocus_urlbar(ChromeState *cs)
{
    cs->url_focused = 0;
    cs->cursor_visible = 0;
}

int chrome_handle_key(ChromeState *cs, uint8_t ascii, uint8_t scancode,
                      int extended)
{
    /* Reset cursor blink on any keypress */
    cs->cursor_visible = 1;
    cs->cursor_blink_tick = clock();

    if (extended) {
        switch (scancode) {
        case 0x4B:  /* Left arrow */
            if (cs->cursor_pos > 0)
                cs->cursor_pos--;
            return 0;
        case 0x4D:  /* Right arrow */
            if (cs->cursor_pos < cs->url_len)
                cs->cursor_pos++;
            return 0;
        case 0x47:  /* Home */
            cs->cursor_pos = 0;
            return 0;
        case 0x4F:  /* End */
            cs->cursor_pos = cs->url_len;
            return 0;
        case 0x53:  /* Delete */
            if (cs->cursor_pos < cs->url_len) {
                memmove(cs->url + cs->cursor_pos,
                        cs->url + cs->cursor_pos + 1,
                        cs->url_len - cs->cursor_pos);
                cs->url_len--;
            }
            return 0;
        }
        return 0;
    }

    /* ASCII keys */
    switch (ascii) {
    case 13:  /* Enter */
        chrome_unfocus_urlbar(cs);
        return 1;  /* Navigate */

    case 27:  /* Escape */
        chrome_unfocus_urlbar(cs);
        return 2;  /* Cancel */

    case 8:   /* Backspace */
        if (cs->cursor_pos > 0) {
            memmove(cs->url + cs->cursor_pos - 1,
                    cs->url + cs->cursor_pos,
                    cs->url_len - cs->cursor_pos + 1);
            cs->cursor_pos--;
            cs->url_len--;
        }
        return 0;

    default:
        /* Printable character - insert at cursor */
        if (ascii >= 32 && ascii <= 126 && cs->url_len < CHROME_MAX_URL - 1) {
            memmove(cs->url + cs->cursor_pos + 1,
                    cs->url + cs->cursor_pos,
                    cs->url_len - cs->cursor_pos + 1);
            cs->url[cs->cursor_pos] = (char)ascii;
            cs->cursor_pos++;
            cs->url_len++;
        }
        return 0;
    }
}

int chrome_tick_cursor(ChromeState *cs)
{
    clock_t now;
    clock_t elapsed;

    if (!cs->url_focused) return 0;

    now = clock();
    elapsed = (now - cs->cursor_blink_tick) * 1000 / CLOCKS_PER_SEC;

    if (elapsed >= CURSOR_BLINK_MS) {
        cs->cursor_visible = !cs->cursor_visible;
        cs->cursor_blink_tick = now;
        return 1;
    }
    return 0;
}
