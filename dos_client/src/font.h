/*
 * RetroSurf Font Subsystem - Bitmap font rendering
 *
 * Loads VGA BIOS ROM fonts via INT 10h AH=11h AL=30h.
 * Provides char/string drawing to pixel buffers.
 */

#ifndef RETROSURF_FONT_H
#define RETROSURF_FONT_H

#include <stdint.h>

/* Font size identifiers */
#define FONT_SMALL   0   /* 8x8  (6-pixel advance for compact UI) */
#define FONT_MEDIUM  1   /* 8x14 */
#define FONT_LARGE   2   /* 8x16 (address bar, chrome) */

/* Initialize fonts by reading BIOS ROM font tables.
 * Must be called before any drawing functions.
 * Returns 0 on success, -1 on failure. */
int font_init(void);

/* Draw a single character at (x,y) in a pixel buffer.
 * buf: pixel buffer, stride: bytes per row
 * color: palette index, bg: background color (255 = transparent) */
void font_draw_char(uint8_t *buf, int stride, int x, int y,
                    unsigned char ch, uint8_t color, uint8_t bg,
                    int font_size);

/* Draw a null-terminated string at (x,y). */
void font_draw_string(uint8_t *buf, int stride, int x, int y,
                      const char *str, uint8_t color, uint8_t bg,
                      int font_size);

/* Get font character advance width in pixels */
int font_char_width(int font_size);

/* Get font character height in pixels */
int font_char_height(int font_size);

/* Get string width in pixels */
int font_string_width(const char *str, int font_size);

#endif /* RETROSURF_FONT_H */
