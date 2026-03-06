/*
 * RetroSurf Software Cursor - Save-under/draw/restore cycle
 *
 * VESA modes don't support hardware cursors. We render a software
 * cursor into the backbuffer with a save/restore cycle to avoid
 * corrupting the underlying image.
 */

#ifndef RETROSURF_CURSOR_H
#define RETROSURF_CURSOR_H

#include <stdint.h>

#define CURSOR_MAX_SIZE 16

/* Software cursor state */
typedef struct {
    uint8_t  width, height;
    uint8_t  hotspot_x, hotspot_y;
    uint8_t  pixels[CURSOR_MAX_SIZE * CURSOR_MAX_SIZE];  /* 0 = transparent */
    uint8_t  save_under[CURSOR_MAX_SIZE * CURSOR_MAX_SIZE];
    int16_t  save_x, save_y;       /* Position where save_under was captured */
    int16_t  save_w, save_h;       /* Clipped size of saved region */
    uint8_t  visible;              /* Is cursor currently drawn? */
    uint8_t  shape;                /* Current shape ID */
} SoftCursor;

/* Initialize cursor with default arrow shape */
void cursor_init(SoftCursor *c);

/* Set cursor to a built-in shape.
 * shape: 0=arrow, 1=hand, 2=ibeam, 3=wait */
void cursor_set_shape(SoftCursor *c, uint8_t shape);

/* Set cursor to custom bitmap from server CURSOR_SHAPE message.
 * data: width*height bytes of palette-indexed pixels (0=transparent) */
void cursor_set_custom(SoftCursor *c, uint8_t w, uint8_t h,
                       uint8_t hx, uint8_t hy, const uint8_t *data);

/* Restore the backbuffer pixels that were under the cursor.
 * Call BEFORE updating the backbuffer (e.g., before rendering tiles). */
void cursor_restore(SoftCursor *c, uint8_t *backbuffer, int stride,
                    int screen_w, int screen_h);

/* Save the pixels under the new cursor position, then draw the cursor.
 * Call AFTER updating the backbuffer.
 * x, y: cursor position (hotspot location on screen) */
void cursor_save_and_draw(SoftCursor *c, uint8_t *backbuffer, int stride,
                          int screen_w, int screen_h, int x, int y);

#endif /* RETROSURF_CURSOR_H */
