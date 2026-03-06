/*
 * RetroSurf Input Subsystem - Keyboard and mouse handling
 *
 * Mouse: INT 33h via DPMI (CuteMouse or compatible driver)
 * Keyboard: kbhit/getch (DJGPP libc, uses BIOS INT 16h)
 * Mouse moves throttled to 30Hz, clicks sent immediately.
 */

#include <stdio.h>
#include <string.h>
#include <conio.h>
#include <time.h>
#include <dpmi.h>
#include "input.h"

/* Mouse move throttle: 30Hz (send at most every ~33ms) */
#define MOUSE_MOVE_INTERVAL (CLOCKS_PER_SEC / 30)

static clock_t last_mouse_send = 0;
static int16_t last_send_x = -1, last_send_y = -1;
static int mouse_available = 0;

int input_init_mouse(uint16_t max_x, uint16_t max_y)
{
    __dpmi_regs r;

    /* INT 33h AX=0000h: Reset mouse driver */
    memset(&r, 0, sizeof(r));
    r.x.ax = 0x0000;
    __dpmi_int(0x33, &r);

    if (r.x.ax == 0) {
        printf("Mouse driver not found.\n");
        mouse_available = 0;
        return 0;
    }
    mouse_available = 1;

    /* INT 33h AX=0007h: Set horizontal range */
    memset(&r, 0, sizeof(r));
    r.x.ax = 0x0007;
    r.x.cx = 0;
    r.x.dx = max_x - 1;
    __dpmi_int(0x33, &r);

    /* INT 33h AX=0008h: Set vertical range */
    memset(&r, 0, sizeof(r));
    r.x.ax = 0x0008;
    r.x.cx = 0;
    r.x.dx = max_y - 1;
    __dpmi_int(0x33, &r);

    /* Position mouse at center */
    memset(&r, 0, sizeof(r));
    r.x.ax = 0x0004;
    r.x.cx = max_x / 2;
    r.x.dx = max_y / 2;
    __dpmi_int(0x33, &r);

    last_mouse_send = clock();
    last_send_x = max_x / 2;
    last_send_y = max_y / 2;

    return 1;
}

void input_poll_mouse(MouseState *ms)
{
    __dpmi_regs r;

    ms->prev_buttons = ms->buttons;
    ms->prev_x = ms->x;
    ms->prev_y = ms->y;

    if (!mouse_available) return;

    /* INT 33h AX=0003h: Get mouse position and button status */
    memset(&r, 0, sizeof(r));
    r.x.ax = 0x0003;
    __dpmi_int(0x33, &r);

    ms->x = (int16_t)r.x.cx;
    ms->y = (int16_t)r.x.dx;
    ms->buttons = r.x.bx & 0x07;
}

int input_mouse_clicked(MouseState *ms, int button)
{
    uint8_t mask = 1 << button;
    return (ms->buttons & mask) && !(ms->prev_buttons & mask);
}

int input_mouse_released(MouseState *ms, int button)
{
    uint8_t mask = 1 << button;
    return !(ms->buttons & mask) && (ms->prev_buttons & mask);
}

int input_mouse_moved(MouseState *ms)
{
    return (ms->x != ms->prev_x) || (ms->y != ms->prev_y);
}

int input_poll_key(KeyEvent *evt)
{
    int key;

    if (!kbhit()) return 0;

    key = getch();
    memset(evt, 0, sizeof(KeyEvent));

    if (key == 0 || key == 0xE0) {
        /* Extended key - read scan code */
        evt->extended = 1;
        evt->scancode = getch();
        evt->ascii = 0;
    } else {
        evt->extended = 0;
        evt->ascii = (uint8_t)key;
        evt->scancode = 0;
    }

    return 1;
}

int input_should_send_mouse_move(MouseState *ms)
{
    clock_t now;

    if (ms->x == last_send_x && ms->y == last_send_y)
        return 0;  /* Position hasn't changed */

    now = clock();
    if (now - last_mouse_send >= MOUSE_MOVE_INTERVAL)
        return 1;

    return 0;
}

void input_mouse_event_sent(int16_t sent_x, int16_t sent_y)
{
    last_mouse_send = clock();
    last_send_x = sent_x;
    last_send_y = sent_y;
}
