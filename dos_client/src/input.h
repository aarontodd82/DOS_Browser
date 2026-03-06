/*
 * RetroSurf Input Subsystem - Keyboard and mouse handling
 *
 * INT 33h mouse polling, keyboard polling, event throttling.
 */

#ifndef RETROSURF_INPUT_H
#define RETROSURF_INPUT_H

#include <stdint.h>

/* Mouse state */
typedef struct {
    int16_t  x, y;           /* Current position */
    uint8_t  buttons;        /* Current button state (bit 0=left, 1=right, 2=middle) */
    uint8_t  prev_buttons;   /* Previous button state */
    int16_t  prev_x, prev_y; /* Previous position */
} MouseState;

/* Key event from polling */
typedef struct {
    uint8_t  ascii;      /* ASCII value (0 if extended key) */
    uint8_t  scancode;   /* Scan code (for extended keys) */
    uint8_t  extended;   /* 1 if extended key (prefixed with 0x00 or 0xE0) */
} KeyEvent;

/* Initialize mouse driver (INT 33h reset + set coordinate range).
 * Returns 1 if mouse is available, 0 if not. */
int input_init_mouse(uint16_t max_x, uint16_t max_y);

/* Poll mouse state. Updates ms in place. */
void input_poll_mouse(MouseState *ms);

/* Check if a mouse button was just pressed (transition from up to down).
 * button: 0=left, 1=right, 2=middle */
int input_mouse_clicked(MouseState *ms, int button);

/* Check if a mouse button was just released. */
int input_mouse_released(MouseState *ms, int button);

/* Check if mouse position changed since last poll. */
int input_mouse_moved(MouseState *ms);

/* Poll keyboard. Returns 1 if a key is available, fills *evt.
 * Returns 0 if no key waiting. */
int input_poll_key(KeyEvent *evt);

/* Throttled mouse move sending.
 * Returns 1 if a move event should be sent now, 0 if throttled.
 * Call this each frame; it manages its own timing. */
int input_should_send_mouse_move(MouseState *ms);

/* Mark that a mouse event was just sent (resets throttle timer).
 * Pass the coordinates that were sent so we don't re-send same position. */
void input_mouse_event_sent(int16_t sent_x, int16_t sent_y);

#endif /* RETROSURF_INPUT_H */
