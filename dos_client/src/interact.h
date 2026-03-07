/*
 * RetroSurf Interaction Map - Hit testing, forwarding mode, cursor shapes
 *
 * Parses INTERACTION_MAP messages from the server to know where
 * interactive elements are on the page. Provides:
 * - Hit testing: which element (if any) is at a given coordinate
 * - Forwarding mode: keystrokes sent to server for text inputs
 * - Local cursor shape: hand over links, I-beam over text fields
 */

#ifndef RETROSURF_INTERACT_H
#define RETROSURF_INTERACT_H

#include <stdint.h>
#include "protocol.h"
#include "network.h"
#include "cursor.h"

/* Maximum interactive elements we can track */
#define MAX_INTERACT_ELEMS 256

/* Maximum value text stored per element */
#define MAX_ELEM_VALUE 128

/* Stored interactive element (parsed from INTERACTION_MAP) */
typedef struct {
    uint16_t id;
    uint16_t x, y, w, h;        /* Content-area-relative coordinates */
    uint8_t  type;               /* ELEM_xxx from protocol.h */
    uint8_t  flags;              /* Bit 0=focused, 1=disabled, 2=checked */
    uint8_t  font_size;          /* 0=small, 1=medium, 2=large */
    uint8_t  text_color;         /* Palette index */
    uint8_t  bg_color;           /* Palette index */
} InteractElem;

/* Interaction map state */
typedef struct {
    InteractElem elems[MAX_INTERACT_ELEMS];
    uint16_t     elem_count;
    uint32_t     scroll_y;       /* Page scroll position (pixels) */
    uint32_t     scroll_height;  /* Total document height (pixels) */
} InteractMap;

/* Interaction mode */
#define INTERACT_MODE_NONE    0  /* Normal - keys go to server or global */
#define INTERACT_MODE_FORWARD 1  /* Forwarding keys to server (text field active) */

/* Interaction context */
typedef struct {
    InteractMap map;
    uint8_t     mode;            /* INTERACT_MODE_xxx */
    uint16_t    forward_elem_id; /* Element ID in forwarding mode */
    uint8_t     last_cursor;     /* Last cursor shape set (avoid redundant updates) */
} InteractCtx;

/* Initialize the interaction context. */
void interact_init(InteractCtx *ctx);

/* Parse an INTERACTION_MAP message payload. */
void interact_parse_map(InteractCtx *ctx, const uint8_t *payload,
                        uint16_t payload_len);

/* Hit-test a content-area click at (cx, cy).
 * Returns pointer to the hit element, or NULL. */
InteractElem *interact_hit_test(InteractCtx *ctx, uint16_t cx, uint16_t cy);

/* Handle a content-area click on an interactive element.
 * Enters forwarding mode for editable elements, sends click to server. */
void interact_handle_click(InteractCtx *ctx, InteractElem *elem,
                           net_context_t *net, uint16_t cx, uint16_t cy,
                           uint8_t buttons);

/* Handle a content-area click that missed all interactive elements.
 * Deactivates forwarding mode and sends the click to server. */
void interact_handle_miss(InteractCtx *ctx, net_context_t *net,
                          uint16_t cx, uint16_t cy, uint8_t buttons);

/* Deactivate forwarding mode. */
void interact_deactivate(InteractCtx *ctx);

/* Update cursor shape based on what element the mouse is over.
 * cx, cy are content-area-relative. Pass 0xFFFF to force arrow. */
void interact_update_cursor(InteractCtx *ctx, SoftCursor *cur,
                            uint16_t cx, uint16_t cy);

#endif /* RETROSURF_INTERACT_H */
