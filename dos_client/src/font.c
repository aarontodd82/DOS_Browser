/*
 * RetroSurf Font Subsystem - BIOS ROM font loading and rendering
 */

#include <stdio.h>
#include <string.h>
#include <dpmi.h>
#include <go32.h>
#include <sys/movedata.h>
#include "font.h"

/* Font data loaded from BIOS ROM */
static uint8_t font_8x8[256 * 8];     /* 2048 bytes */
static uint8_t font_8x14[256 * 14];   /* 3584 bytes */
static uint8_t font_8x16[256 * 16];   /* 4096 bytes */

/* Pointers and metrics for each font size */
static struct {
    uint8_t *data;
    int      width;       /* pixel width of glyph bitmap */
    int      advance;     /* pixel advance (may differ from width) */
    int      height;
    int      bytes_per_char;
} fonts[3];

int font_init(void)
{
    __dpmi_regs r;
    uint32_t addr;

    /* Get 8x16 font (BH=6) */
    memset(&r, 0, sizeof(r));
    r.x.ax = 0x1130;
    r.h.bh = 6;
    __dpmi_int(0x10, &r);
    addr = ((uint32_t)r.x.es << 4) + r.x.bp;
    dosmemget(addr, 256 * 16, font_8x16);

    /* Get 8x14 font (BH=2) */
    memset(&r, 0, sizeof(r));
    r.x.ax = 0x1130;
    r.h.bh = 2;
    __dpmi_int(0x10, &r);
    addr = ((uint32_t)r.x.es << 4) + r.x.bp;
    dosmemget(addr, 256 * 14, font_8x14);

    /* Get 8x8 font (BH=3, chars 0-127; BH=4 not needed for UI) */
    memset(&r, 0, sizeof(r));
    r.x.ax = 0x1130;
    r.h.bh = 3;
    __dpmi_int(0x10, &r);
    addr = ((uint32_t)r.x.es << 4) + r.x.bp;
    dosmemget(addr, 128 * 8, font_8x8);

    /* Get 8x8 font upper half (BH=4, chars 128-255) */
    memset(&r, 0, sizeof(r));
    r.x.ax = 0x1130;
    r.h.bh = 4;
    __dpmi_int(0x10, &r);
    addr = ((uint32_t)r.x.es << 4) + r.x.bp;
    dosmemget(addr, 128 * 8, font_8x8 + 128 * 8);

    /* Set up font table */
    fonts[FONT_SMALL].data = font_8x8;
    fonts[FONT_SMALL].width = 8;
    fonts[FONT_SMALL].advance = 6;    /* compact advance */
    fonts[FONT_SMALL].height = 8;
    fonts[FONT_SMALL].bytes_per_char = 8;

    fonts[FONT_MEDIUM].data = font_8x14;
    fonts[FONT_MEDIUM].width = 8;
    fonts[FONT_MEDIUM].advance = 8;
    fonts[FONT_MEDIUM].height = 14;
    fonts[FONT_MEDIUM].bytes_per_char = 14;

    fonts[FONT_LARGE].data = font_8x16;
    fonts[FONT_LARGE].width = 8;
    fonts[FONT_LARGE].advance = 8;
    fonts[FONT_LARGE].height = 16;
    fonts[FONT_LARGE].bytes_per_char = 16;

    return 0;
}

void font_draw_char(uint8_t *buf, int stride, int x, int y,
                    unsigned char ch, uint8_t color, uint8_t bg,
                    int font_size)
{
    const uint8_t *glyph;
    int w, h, row, col;

    if (font_size < 0 || font_size > 2) return;

    glyph = fonts[font_size].data + ch * fonts[font_size].bytes_per_char;
    w = fonts[font_size].advance;
    h = fonts[font_size].height;

    for (row = 0; row < h; row++) {
        uint8_t bits = glyph[row];
        uint8_t *dst = buf + (y + row) * stride + x;
        for (col = 0; col < w; col++) {
            if (bits & (0x80 >> col)) {
                dst[col] = color;
            } else if (bg != 255) {
                dst[col] = bg;
            }
        }
    }
}

void font_draw_string(uint8_t *buf, int stride, int x, int y,
                      const char *str, uint8_t color, uint8_t bg,
                      int font_size)
{
    int advance;

    if (font_size < 0 || font_size > 2) return;
    advance = fonts[font_size].advance;

    while (*str) {
        font_draw_char(buf, stride, x, y,
                       (unsigned char)*str, color, bg, font_size);
        x += advance;
        str++;
    }
}

int font_char_width(int font_size)
{
    if (font_size < 0 || font_size > 2) return 8;
    return fonts[font_size].advance;
}

int font_char_height(int font_size)
{
    if (font_size < 0 || font_size > 2) return 16;
    return fonts[font_size].height;
}

int font_string_width(const char *str, int font_size)
{
    return (int)strlen(str) * font_char_width(font_size);
}
