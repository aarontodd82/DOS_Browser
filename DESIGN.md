# RetroSurf: A Modern Web Browser for DOS 6.22

## Design & Implementation Document

**Version:** 0.4.0 (End-to-End Networking Verified)
**Date:** 2026-03-06
**Status:** Pi Server Complete, DOS Networking Verified, Full Client Pending

---

## Table of Contents

1. [Project Overview](#1-project-overview)
2. [System Architecture](#2-system-architecture)
3. [Hardware Requirements](#3-hardware-requirements)
4. [Protocol Specification](#4-protocol-specification)
5. [DOS Client Design](#5-dos-client-design)
6. [Pi Server Design](#6-pi-server-design)
7. [Image Processing Pipeline](#7-image-processing-pipeline)
8. [Interactive Element System](#8-interactive-element-system)
9. [Performance Budget](#9-performance-budget)
10. [Build & Deployment](#10-build--deployment)
11. [V1 Scope](#11-v1-scope)
12. [Future Enhancements](#12-future-enhancements)
13. [Changelog](#13-changelog)

---

## 1. Project Overview

### 1.1 Goal

Build a fully functional web browser that runs on a DOS 6.22 machine (25MHz CPU, 4MB RAM) capable of rendering and interacting with modern web applications including Claude.ai, ChatGPT, Google Drive, and other complex JavaScript-heavy web apps.

### 1.2 Approach

A split-architecture system where a Raspberry Pi 4 acts as a rendering proxy. The Pi runs a full headless Chromium browser, captures the rendered output, compresses it into a format the DOS machine can display, and streams it over Ethernet. The DOS machine acts as a thin client: it displays the rendered frames, handles local input, and communicates user interactions back to the Pi.

### 1.3 Design Principles

- **Responsiveness over fidelity** -- typing and clicking must feel instant, even if visual updates lag slightly behind.
- **Correctness over beauty** -- every website must be usable. Dithered 256-color rendering is acceptable; broken layouts are not.
- **Simplicity on the DOS side** -- the DOS client should be as "dumb" as possible. All complexity lives on the Pi.
- **Multi-resolution flexibility** -- support multiple VGA/VESA modes for hardware compatibility and community sharing.

### 1.4 Project Name

**RetroSurf**

---

## 2. System Architecture

### 2.1 High-Level Overview

```
+------------------+          Ethernet (10Mbps)          +------------------+
|   DOS Machine    |<====================================>|  Raspberry Pi 4  |
|   (Thin Client)  |    Custom binary protocol / TCP     |  (Render Server) |
+------------------+                                      +------------------+
        |                                                         |
   +----+----+                                            +-------+-------+
   | Display |  VGA/VESA                                  | Headless      |
   | (8bpp)  |  framebuffer                               | Chromium via  |
   +---------+                                            | Playwright    |
   | Input   |  Keyboard (INT 16h)                        +---------------+
   | (mouse/ |  Mouse (INT 33h/CuteMouse)                 | Image Pipeline|
   |  kbd)   |                                            | (NumPy/PIL)   |
   +---------+                                            +---------------+
   | Network |  NE2000 packet driver                      | Protocol      |
   | (Watt32)|  + Watt-32 TCP/IP                          | Server (TCP)  |
   +---------+                                            +---------------+
```

### 2.2 Data Flow

**Rendering flow (Pi -> DOS):**
1. Chromium renders a web page at the negotiated viewport size
2. CDP Screencast pushes a JPEG frame when content visually changes
3. Python decodes JPEG to NumPy array
4. Pipeline: palette quantize (via LUT) -> ordered dither -> tile -> delta detect (XOR vs previous frame) -> RLE compress
5. Changed tiles sent as `FRAME_DELTA` message over TCP
6. DOS client receives, RLE-decompresses each tile, XORs with previous, writes to backbuffer
7. Backbuffer copied to VGA framebuffer (LFB or banked)

**Interaction flow (DOS -> Pi):**
1. DOS client detects mouse click at screen coordinates (x, y)
2. Checks interaction map: is this inside a known interactive element?
   - **Yes, standard input**: DOS enters local edit mode, renders text locally at zero latency
   - **Yes, contenteditable/custom**: DOS enters forwarding mode, sends keystrokes to Pi
   - **Yes, button/link/checkbox**: DOS sends click event to Pi
   - **No**: DOS sends raw click coordinates to Pi (Pi will process via Playwright)
3. Pi receives event, injects it into Chromium via Playwright
4. Chromium processes the interaction, page updates
5. CDP Screencast fires with new frame, cycle repeats

### 2.3 Connection Lifecycle

```
1. DOS client starts, reads RETROSURF.CFG for Pi IP address and port
2. DOS client initializes: video mode detection, packet driver, Watt-32
3. DOS client connects to Pi server via TCP
4. HANDSHAKE:
   Client -> Server: CLIENT_HELLO (resolution, color depth, protocol version)
   Server -> Client: SERVER_HELLO (session ID, palette data, confirmation)
5. Server sends initial FRAME_FULL + INTERACTION_MAP
6. Main loop: bidirectional async message exchange
7. On disconnect: server keeps browser session alive for 5 minutes
8. On reconnect: client sends CLIENT_HELLO with previous session ID
   Server resumes session, sends current FRAME_FULL + INTERACTION_MAP
```

---

## 3. Hardware Requirements

### 3.1 DOS Machine (Minimum)

| Component | Requirement | Notes |
|-----------|------------|-------|
| CPU | 386 or 486 @ 25MHz+ | 486 strongly recommended for L1 cache benefits |
| RAM | 4MB minimum | ~3.0-3.5MB usable by application after DOS/DPMI overhead |
| Video | VGA (640x480x16 color minimum) | VESA VBE 1.2+ for 256-color modes; VBE 2.0+ for LFB |
| Network | NE2000-compatible Ethernet ISA | D-Link DE-220PCT or similar |
| Mouse | Serial or PS/2 with INT 33h driver | CuteMouse recommended |
| DOS | MS-DOS 6.22 or compatible | FreeDOS also supported |

### 3.2 DOS Machine (Recommended)

| Component | Recommendation | Benefit |
|-----------|---------------|---------|
| CPU | 486DX @ 33MHz+ | Faster decompression, smoother scrolling |
| RAM | 8MB | More headroom for larger tile caches |
| Video | VESA VBE 2.0+ with LFB | ~2-3x faster display writes vs bank switching |
| Network | NE2000-compatible | Same card, performance is CPU-bound anyway |

### 3.3 Raspberry Pi 4

| Component | Requirement | Notes |
|-----------|------------|-------|
| Model | Pi 4 Model B | ARM Cortex-A72 quad-core @ 1.5GHz |
| RAM | 4GB minimum | Chromium uses 200-600MB; 2GB Pi will be very tight |
| Storage | 16GB+ SD card | OS + Chromium + dependencies |
| OS | Raspberry Pi OS 64-bit (Bookworm+) | Required for Playwright ARM64 Chromium builds |
| Network | Ethernet (built-in gigabit) | Connected to same LAN as DOS machine |

### 3.4 Network

- Both machines on the same local network
- Static IP recommended for the Pi (configured in DOS client's `RETROSURF.CFG`)
- No internet access needed on the DOS machine -- the Pi handles all web traffic
- Latency between machines should be <1ms (direct Ethernet or simple switch)

---

## 4. Protocol Specification

### 4.1 Transport

TCP over IPv4. Single persistent connection. Port **8086** (default, configurable).

### 4.2 Byte Order

**Little-endian throughout.** The x86 DOS client reads multi-byte fields natively without byte-swapping. The Pi server converts to little-endian before sending.

### 4.3 Message Framing

Every message has a fixed 8-byte header:

```c
struct msg_header {
    uint8_t  msg_type;       // Message type identifier
    uint8_t  flags;          // Bit flags (see below)
    uint16_t payload_len;    // Bytes following this header (max 65535)
    uint16_t sequence;       // Monotonically increasing per-sender
    uint16_t reserved;       // Must be 0 (future use)
};
```

**Flags byte:**
```
Bit 0: COMPRESSED     - Payload is RLE compressed
Bit 1: CONTINUED      - More fragments follow (for payloads > 64KB)
Bit 2: URGENT         - Process before queued messages
Bits 3-7: Reserved (0)
```

### 4.4 Message Types

#### 4.4.1 Client -> Server Messages

| Type | ID | Description |
|------|-----|-------------|
| `CLIENT_HELLO` | `0x01` | Handshake initiation |
| `MOUSE_EVENT` | `0x10` | Mouse position + button state |
| `KEY_EVENT` | `0x11` | Keyboard scancode + modifiers |
| `SCROLL_EVENT` | `0x12` | Scroll direction + amount |
| `TEXT_INPUT` | `0x13` | Full text content of a locally-edited input field |
| `NAVIGATE` | `0x14` | URL string for address bar navigation |
| `NAV_ACTION` | `0x15` | Back, Forward, Stop, Reload |
| `ACK` | `0xF0` | Acknowledge received message |
| `KEEPALIVE` | `0xF1` | Connection keepalive ping |

#### 4.4.2 Server -> Client Messages

| Type | ID | Description |
|------|-----|-------------|
| `SERVER_HELLO` | `0x81` | Handshake response + session info |
| `PALETTE` | `0x82` | 256-entry RGB palette (768 bytes) |
| `FRAME_FULL` | `0x83` | Complete screen of tile data (used on initial load / full refresh) |
| `FRAME_DELTA` | `0x84` | Changed tiles only (primary update mechanism) |
| `INTERACTION_MAP` | `0x85` | List of interactive elements with bounding boxes |
| `CURSOR_SHAPE` | `0x86` | Mouse cursor bitmap + hotspot |
| `STATUS` | `0x87` | Status bar text (loading state, page title, etc.) |
| `INPUT_STATE` | `0x88` | Update to a locally-managed input field (e.g., autocomplete) |
| `ACK` | `0xF0` | Acknowledge received message |
| `KEEPALIVE_ACK` | `0xF1` | Keepalive response |

### 4.5 Message Payloads

#### CLIENT_HELLO (0x01)

```c
struct client_hello {
    uint16_t protocol_version;    // 0x0001 for v1
    uint16_t screen_width;        // e.g., 640 or 800
    uint16_t screen_height;       // e.g., 480 or 600
    uint8_t  color_depth;         // 4 (16 colors) or 8 (256 colors)
    uint8_t  tile_size;           // 16 (16x16 tiles)
    uint16_t chrome_height;       // Height in pixels reserved for UI chrome
    uint16_t max_recv_buffer;     // Max message size client can handle (KB)
    uint32_t session_id;          // 0 for new session, nonzero to resume
};
// Total: 16 bytes
```

#### SERVER_HELLO (0x81)

```c
struct server_hello {
    uint16_t protocol_version;
    uint32_t session_id;          // Assigned session ID
    uint16_t content_width;       // Actual content area width
    uint16_t content_height;      // Actual content area height
    uint8_t  tile_size;           // Confirmed tile size
    uint8_t  server_flags;        // Capability flags
};
// Total: 12 bytes
// Immediately followed by a PALETTE message
```

#### MOUSE_EVENT (0x10)

```c
struct mouse_event {
    uint16_t x;                   // Screen X coordinate
    uint16_t y;                   // Screen Y coordinate (relative to content area)
    uint8_t  buttons;             // Bit 0=left, 1=right, 2=middle
    uint8_t  event_type;          // 0=move, 1=click, 2=release, 3=double-click
};
// Total: 6 bytes
```

#### KEY_EVENT (0x11)

```c
struct key_event {
    uint8_t  scancode;            // DOS scancode
    uint8_t  ascii;               // ASCII value (0 if special key)
    uint8_t  modifiers;           // Bit 0=shift, 1=ctrl, 2=alt
    uint8_t  event_type;          // 0=press, 1=release
};
// Total: 4 bytes
```

#### TEXT_INPUT (0x13)

```c
struct text_input {
    uint16_t element_id;          // Interactive element ID from interaction map
    uint16_t cursor_pos;          // Cursor position within text
    uint16_t text_len;            // Length of text following
    // text_len bytes of UTF-8 text follow (converted from DOS codepage by Pi)
};
// Total: 6 bytes + text
```

#### FRAME_DELTA (0x84)

```c
struct frame_delta {
    uint16_t tile_count;          // Number of changed tiles in this message
    // Followed by tile_count tile entries:
};

struct tile_entry {
    uint16_t tile_index;          // Linear index in tile grid (row-major)
    uint16_t compressed_size;     // Bytes of compressed data following
    // compressed_size bytes of XOR-delta + RLE compressed tile data
};
```

#### INTERACTION_MAP (0x85)

```c
struct interaction_map {
    uint16_t element_count;       // Number of interactive elements
    uint16_t page_scroll_y;       // Current page scroll position
    // Followed by element_count element entries:
};

struct element_entry {
    uint16_t element_id;          // Unique ID for this element
    uint16_t x, y, w, h;         // Bounding rectangle (content-area-relative)
    uint8_t  element_type;        // See element types below
    uint8_t  flags;               // Bit 0=focused, 1=disabled, 2=checked, 3=password
    uint8_t  font_size;           // 0=small(6x8), 1=medium(8x14), 2=large(8x16)
    uint8_t  text_color;          // Palette index for text
    uint8_t  bg_color;            // Palette index for background
    uint16_t value_len;           // Length of current value string
    // value_len bytes of value/label text follow
};

// Element types:
// 0x00 = LINK           (clickable, navigates)
// 0x01 = BUTTON         (clickable, triggers action)
// 0x02 = TEXT_INPUT     (single-line text, locally editable)
// 0x03 = TEXT_AREA      (multi-line text, locally editable)
// 0x04 = PASSWORD       (locally editable, display as asterisks)
// 0x05 = CHECKBOX       (toggle on click)
// 0x06 = RADIO          (select on click)
// 0x07 = SELECT         (dropdown -- click to show options)
// 0x08 = CONTENTEDITABLE (rich text -- forwarding mode)
// 0x09 = CUSTOM_WIDGET  (unknown interactive -- forwarding mode)
```

#### CURSOR_SHAPE (0x86)

```c
struct cursor_shape {
    uint8_t  cursor_type;         // 0=arrow, 1=hand, 2=text, 3=wait, 4=custom
    uint8_t  hotspot_x;           // Hotspot offset within cursor bitmap
    uint8_t  hotspot_y;
    uint8_t  width;               // Cursor bitmap width (max 16)
    uint8_t  height;              // Cursor bitmap height (max 16)
    // width * height bytes of cursor bitmap data (palette-indexed)
    // 0x00 = transparent
};
```

### 4.6 Tile Compression Format

Each tile in a `FRAME_DELTA` or `FRAME_FULL` message is compressed using **XOR delta + byte-level RLE**:

**Encoding (Pi side):**
1. XOR the new tile pixels with the previous tile pixels. Unchanged pixels become `0x00`.
2. RLE-encode the resulting delta buffer.

**RLE Format:**
```
Control byte:
  Bits 7:    0 = literal run, 1 = repeat run
  Bits 6-0:  length (1-127)

If literal (bit 7 = 0):
  Next <length> bytes are literal pixel values

If repeat (bit 7 = 1):
  Next 1 byte is the value to repeat <length> times
```

**Special case:** A `compressed_size` of 0 for a tile means "no change" (tile is identical to previous). This tile is skipped entirely.

**Decoding (DOS side):**
```c
void decode_tile(uint8_t *compressed, uint16_t comp_size,
                 uint8_t *prev_tile, uint8_t *output,
                 uint16_t tile_pixels) {
    uint8_t *src = compressed;
    uint8_t *end = compressed + comp_size;
    uint8_t *dst = output;

    // Step 1: RLE decode into output buffer
    while (src < end) {
        uint8_t ctrl = *src++;
        uint8_t len = ctrl & 0x7F;
        if (ctrl & 0x80) {
            // Repeat run
            uint8_t val = *src++;
            memset(dst, val, len);
            dst += len;
        } else {
            // Literal run
            memcpy(dst, src, len);
            src += len;
            dst += len;
        }
    }

    // Step 2: XOR with previous tile to reconstruct
    for (int i = 0; i < tile_pixels; i++) {
        output[i] ^= prev_tile[i];
    }

    // Step 3: Update previous tile cache
    memcpy(prev_tile, output, tile_pixels);
}
```

### 4.7 Session Management

- The Pi server maintains browser sessions keyed by `session_id`
- Sessions persist across DOS client disconnects for **5 minutes** (configurable)
- On reconnect with a valid `session_id`, the Pi resumes the existing Chromium page
- On reconnect with an expired/invalid `session_id`, the Pi creates a new session
- Maximum concurrent sessions: configurable (default: 1, limited by Pi RAM)

---

## 5. DOS Client Design

### 5.1 Source File Structure

```
dos_client/
  src/
    main.c           -- Entry point, main loop, initialization
    video.c/.h       -- VGA/VESA mode detection, framebuffer management
    video_banked.c   -- Bank-switching write path (fallback)
    video_lfb.c      -- Linear framebuffer write path (preferred)
    network.c/.h     -- Watt-32 socket management, message send/recv
    protocol.c/.h    -- Message parsing, serialization
    input.c/.h       -- Keyboard and mouse handling
    ui_chrome.c/.h   -- Address bar, nav buttons, status bar rendering
    render.c/.h      -- Tile decompression, framebuffer compositing
    interact.c/.h    -- Interactive element overlay, local text editing
    cursor.c/.h      -- Software mouse cursor rendering
    config.c/.h      -- RETROSURF.CFG parsing
    font.c/.h        -- Bitmap font data and text rendering
    fonts/
      font_6x8.h     -- Small font (for dense UI)
      font_8x14.h    -- Medium font (default)
      font_8x16.h    -- Large font (headers, address bar)
  Makefile            -- Cross-compilation makefile
  RETROSURF.CFG       -- Sample configuration file
```

### 5.2 Display Subsystem (`video.c`)

#### 5.2.1 Mode Detection and Selection

At startup, the display subsystem probes available video modes in priority order:

```
Priority 1: 800x600 @ 256 colors (VESA mode 0x103) with LFB
Priority 2: 800x600 @ 256 colors (VESA mode 0x103) banked
Priority 3: 640x480 @ 256 colors (VESA mode 0x101) with LFB
Priority 4: 640x480 @ 256 colors (VESA mode 0x101) banked
Priority 5: 640x480 @ 16 colors  (VGA mode 0x12)  -- guaranteed fallback
```

The probe checks:
1. Is VBE present? (`INT 10h, AX=4F00h` returns "VESA" signature)
2. What VBE version? (need 1.2+ for mode info; 2.0+ for LFB)
3. For each candidate mode: is it listed in the mode table? Query mode info. Check `ModeAttributes` bit 0 (supported) and bit 7 (LFB available, VBE 2.0+ only).
4. Select the highest-priority supported mode.
5. Store mode parameters in a global `video_config` struct:

```c
typedef struct {
    uint16_t width;              // e.g., 640 or 800
    uint16_t height;             // e.g., 480 or 600
    uint16_t content_width;      // width (same as screen width)
    uint16_t content_height;     // height - chrome_height
    uint16_t chrome_height;      // pixels reserved for UI (24 for 640, 28 for 800)
    uint8_t  bpp;                // 8 (256 color) or 4 (16 color)
    uint16_t bytes_per_line;     // from VBE mode info (may include padding)
    uint8_t  has_lfb;            // 1 if linear framebuffer available
    uint32_t lfb_phys_addr;      // physical address of LFB (if has_lfb)
    uint8_t  *lfb_ptr;           // mapped LFB pointer (if has_lfb, via nearptr)
    uint8_t  *backbuffer;        // system RAM backbuffer (malloc'd)
    uint16_t vesa_mode;          // VESA mode number (0 if standard VGA)
    uint16_t bank_granularity;   // bank granularity in KB (for banked modes)
} VideoConfig;
```

#### 5.2.2 Framebuffer Architecture

The DOS client uses **double buffering in system RAM**:

1. **Backbuffer** (`backbuffer`): All rendering (tile decompression, interactive element overlays, cursor, UI chrome) happens here. This is a `malloc`'d buffer in extended memory.
2. **VGA framebuffer** (LFB or banked at `0xA0000`): The backbuffer is copied here once per frame using the fastest available method.

**Why double buffer**: Writing directly to VGA memory causes visual tearing and is slow through bank switching. By compositing everything in system RAM first, we get clean frames and only one VGA write per display cycle.

**Copy to VGA:**
- **LFB path** (`video_lfb.c`): `memcpy(lfb_ptr, backbuffer, width * height)`. With `__djgpp_nearptr_enable()`, this is a single fast memory copy. At 640x480: ~307KB copy, takes ~5-10ms on a 25MHz 486.
- **Banked path** (`video_banked.c`): Copy in 64KB chunks, switching banks between chunks. Each bank switch requires an `INT 10h` call (~50-100us). For 640x480: 5 bank switches, total ~8-15ms.

**Dirty rectangle optimization**: Rather than copying the entire backbuffer to VGA every frame, track which screen regions changed and only copy those. This is especially effective for small delta updates (e.g., cursor movement, text input).

```c
#define MAX_DIRTY_RECTS 64

typedef struct {
    uint16_t x, y, w, h;
} DirtyRect;

static DirtyRect dirty_rects[MAX_DIRTY_RECTS];
static int dirty_count = 0;

void mark_dirty(uint16_t x, uint16_t y, uint16_t w, uint16_t h);
void flush_dirty_to_vga(void);  // copies only dirty regions to VGA
void flush_full_to_vga(void);   // copies entire backbuffer (used for full frames)
```

#### 5.2.3 16-Color Mode (VGA Mode 12h) Special Handling

VGA mode 12h (640x480x16 colors) uses a **planar** memory layout, not a linear byte-per-pixel layout. Each pixel is 4 bits spread across 4 bit planes. Writing pixels requires:

1. Set the write plane mask via the VGA Sequencer (port `0x3C4/0x3C5`)
2. Write the pixel data for that plane
3. Repeat for all 4 planes

This is significantly more complex and slower than 8bpp linear modes. The backbuffer will still be stored as 8bpp (using palette indices 0-15) for simplicity, and converted to planar format during the VGA copy step.

```c
// In video_banked.c, for mode 12h:
void flush_to_vga_planar(uint8_t *backbuffer, int width, int height) {
    for (int plane = 0; plane < 4; plane++) {
        outportb(0x3C4, 0x02);          // Sequencer: Map Mask register
        outportb(0x3C5, 1 << plane);    // Select plane

        for (int y = 0; y < height; y++) {
            for (int x = 0; x < width; x += 8) {
                uint8_t byte = 0;
                for (int bit = 0; bit < 8; bit++) {
                    uint8_t pixel = backbuffer[y * width + x + bit];
                    if (pixel & (1 << plane))
                        byte |= (0x80 >> bit);
                }
                _farpokeb(_dos_ds, 0xA0000 + y * (width / 8) + x / 8, byte);
            }
        }
    }
}
```

This is slow (~50-80ms per full frame at 640x480) but functional. 16-color mode is a compatibility fallback, not the target experience.

### 5.3 Network Subsystem (`network.c`)

#### 5.3.1 Initialization

```c
int network_init(const char *server_ip, uint16_t server_port) {
    sock_init();  // Initialize Watt-32, find packet driver

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_port = htons(server_port);
    addr.sin_addr.s_addr = inet_addr(server_ip);

    // Set non-blocking
    int on = 1;
    ioctlsocket(fd, FIONBIO, (char *)&on);

    connect(fd, (struct sockaddr *)&addr, sizeof(addr));
    // Non-blocking connect returns -1 with EINPROGRESS
    // Poll with select() in main loop until connected

    return fd;
}
```

#### 5.3.2 Receive Buffer

A 32KB ring buffer handles incoming data. Messages are parsed incrementally:

```c
#define RECV_BUFFER_SIZE  (32 * 1024)

typedef struct {
    uint8_t  data[RECV_BUFFER_SIZE];
    uint16_t read_pos;
    uint16_t write_pos;
    uint16_t used;
} RingBuffer;

// In main loop:
void network_poll(int fd, RingBuffer *rb) {
    tcp_tick(NULL);  // Pump Watt-32 TCP/IP state machine -- CRITICAL

    // Read available data into ring buffer
    int space = RECV_BUFFER_SIZE - rb->used;
    if (space > 0) {
        int n = recv(fd, rb->data + rb->write_pos, space, 0);
        if (n > 0) {
            rb->write_pos = (rb->write_pos + n) % RECV_BUFFER_SIZE;
            rb->used += n;
        }
    }

    // Parse complete messages
    while (rb->used >= sizeof(msg_header)) {
        msg_header *hdr = (msg_header *)(rb->data + rb->read_pos);
        uint16_t total = sizeof(msg_header) + hdr->payload_len;
        if (rb->used < total) break;  // Incomplete message

        process_message(hdr, rb->data + rb->read_pos + sizeof(msg_header));
        rb->read_pos = (rb->read_pos + total) % RECV_BUFFER_SIZE;
        rb->used -= total;
    }
}
```

**Important**: `tcp_tick(NULL)` MUST be called regularly (at least once per main loop iteration) to pump the Watt-32 TCP/IP state machine. Without it, incoming packets are not processed and TCP timers do not fire.

#### 5.3.3 Send Buffer

A 4KB send buffer with immediate flush. Client->server messages are small (4-6 bytes for input events), so buffering is minimal:

```c
#define SEND_BUFFER_SIZE  (4 * 1024)

void network_send_message(int fd, uint8_t type, void *payload, uint16_t len) {
    msg_header hdr;
    hdr.msg_type = type;
    hdr.flags = 0;
    hdr.payload_len = len;
    hdr.sequence = next_sequence++;
    hdr.reserved = 0;

    send(fd, &hdr, sizeof(hdr), 0);
    if (len > 0) {
        send(fd, payload, len, 0);
    }
}
```

### 5.4 Input Subsystem (`input.c`)

#### 5.4.1 Keyboard

Keyboard input is read via `int kbhit()` and `int getch()` (DJGPP libc), which use the BIOS keyboard buffer (INT 16h). For scan codes and key-up detection, direct port reading (port `0x60`) or INT 09h hooking is used.

```c
void input_poll_keyboard(void) {
    while (kbhit()) {
        int key = getch();
        if (key == 0 || key == 0xE0) {
            // Extended key -- read second byte
            int ext = getch();
            handle_special_key(ext);
        } else {
            handle_ascii_key(key);
        }
    }
}
```

**Key routing:**
1. If the address bar is focused: keystrokes go to the local address bar editor
2. If a local text input is active: keystrokes go to the local text editor (TEXT_INPUT)
3. If in forwarding mode (contenteditable): keystrokes sent as KEY_EVENT to Pi
4. Otherwise: keystrokes sent as KEY_EVENT to Pi (Pi handles shortcuts, etc.)

#### 5.4.2 Mouse

Mouse state is polled via INT 33h function 03h every main loop iteration:

```c
typedef struct {
    int16_t x, y;
    uint8_t buttons;
    uint8_t prev_buttons;
} MouseState;

void input_poll_mouse(MouseState *ms) {
    __dpmi_regs r;
    memset(&r, 0, sizeof(r));
    r.x.ax = 0x0003;
    __dpmi_int(0x33, &r);

    ms->prev_buttons = ms->buttons;
    ms->x = r.x.cx;
    ms->y = r.x.dx;
    ms->buttons = r.x.bx & 0x07;
}
```

**Mouse coordinate range** is set after video mode initialization to match screen resolution:

```c
void input_init_mouse(uint16_t max_x, uint16_t max_y) {
    __dpmi_regs r;

    // Reset mouse
    memset(&r, 0, sizeof(r));
    r.x.ax = 0x0000;
    __dpmi_int(0x33, &r);

    // Set X range
    memset(&r, 0, sizeof(r));
    r.x.ax = 0x0007;
    r.x.cx = 0;
    r.x.dx = max_x - 1;
    __dpmi_int(0x33, &r);

    // Set Y range
    memset(&r, 0, sizeof(r));
    r.x.ax = 0x0008;
    r.x.cx = 0;
    r.x.dx = max_y - 1;
    __dpmi_int(0x33, &r);
}
```

**Mouse event throttling** (using `uclock()` for microsecond-resolution timing):

```c
#define MOUSE_MOVE_INTERVAL (UCLOCKS_PER_SEC / 30)  // 30Hz position reports

static uclock_t last_mouse_report = 0;
static int16_t last_report_x = -1, last_report_y = -1;

void maybe_send_mouse_move(MouseState *ms) {
    // Button changes: send immediately (latency-critical)
    if (ms->buttons != ms->prev_buttons) {
        uint8_t event_type = (ms->buttons & ~ms->prev_buttons) ? 1 : 2; // click or release
        send_mouse_event(ms->x, ms->y, ms->buttons, event_type);
        last_mouse_report = uclock();
        last_report_x = ms->x;
        last_report_y = ms->y;
        return;
    }

    // Position-only changes: throttle to 30Hz
    if ((ms->x != last_report_x || ms->y != last_report_y) &&
        (uclock() - last_mouse_report >= MOUSE_MOVE_INTERVAL)) {
        send_mouse_event(ms->x, ms->y, ms->buttons, 0);  // event_type 0 = move
        last_mouse_report = uclock();
        last_report_x = ms->x;
        last_report_y = ms->y;
    }
}
```

### 5.5 UI Chrome (`ui_chrome.c`)

The top portion of the screen is reserved for navigation UI, rendered entirely by the DOS client. The chrome is drawn into the backbuffer above the content area.

#### 5.5.1 Layout

**For 640x480 (24px chrome, content area = 640x456):**
```
+-- 640px -----------------------------------------------+
| [<][>][R][S] | http://example.com__________________ |24px
+--- Content area: 640x456 -----------------------------+
|                                                        |
|                                                        |
|                                                        |
|                        ...                             |
|                                                        |
+-- Status bar (last 12px of content area) --------------+
| Loading... | claude.ai                           | 75% |12px
+--------------------------------------------------------+
```

**For 800x600 (28px chrome, content area = 800x572):**
```
+-- 800px -----------------------------------------------+
| [Back][Fwd][Rld][Stop] | http://example.com________ |28px
+--- Content area: 800x572 -----------------------------+
|                                                        |
|                        ...                             |
+-- Status bar (last 14px of content area) --------------+
| Loading... | claude.ai                           | 75% |14px
+--------------------------------------------------------+
```

#### 5.5.2 Chrome Elements

- **Back button `[<]`**: Sends `NAV_ACTION` with action=BACK
- **Forward button `[>]`**: Sends `NAV_ACTION` with action=FORWARD
- **Reload button `[R]`**: Sends `NAV_ACTION` with action=RELOAD
- **Stop button `[S]`**: Sends `NAV_ACTION` with action=STOP (replaces Reload while loading)
- **Address bar**: A locally-editable text field. Click to focus, type URL, press Enter to send `NAVIGATE` message. Escape to cancel editing and restore current URL.
- **Status bar**: Shows loading state, hovered link URL, and connection quality indicator. Updated via `STATUS` messages from the Pi.

All chrome is rendered using the bitmap fonts at the `LARGE` size (8x16). Background color is a neutral gray from the palette. Text is white on dark gray for the chrome bar, dark on light gray for the address field.

### 5.6 Rendering Pipeline (`render.c`)

#### 5.6.1 Tile Grid

The content area is divided into a grid of 16x16 pixel tiles:

```c
#define TILE_SIZE 16
#define TILE_PIXELS (TILE_SIZE * TILE_SIZE)  // 256 bytes per tile at 8bpp

typedef struct {
    uint16_t cols;                     // tiles per row
    uint16_t rows;                     // tiles per column
    uint16_t total;                    // total tiles
    uint8_t  *prev_tiles;             // previous frame tile data (for XOR delta)
    uint8_t  *current_tiles;          // current frame tile data
} TileGrid;

// For 640x456 content area: 40 cols x 28.5 rows = 40 x 29 tiles = 1160 tiles
// (bottom row of tiles extends 8px below content area -- clipped during display)
// Total tile cache memory: 1160 * 256 * 2 = ~580KB (prev + current)
//
// For 800x572: 50 x 36 = 1800 tiles
// Total tile cache memory: 1800 * 256 * 2 = ~900KB
```

#### 5.6.2 Receiving and Applying Tile Updates

When a `FRAME_DELTA` message arrives:

```c
void render_apply_delta(TileGrid *grid, uint8_t *payload, uint16_t len) {
    uint16_t tile_count = *(uint16_t *)payload;
    uint8_t *ptr = payload + 2;

    for (int i = 0; i < tile_count; i++) {
        uint16_t tile_index = *(uint16_t *)ptr;
        ptr += 2;
        uint16_t comp_size = *(uint16_t *)ptr;
        ptr += 2;

        if (comp_size == 0) continue;  // No change

        uint16_t tile_col = tile_index % grid->cols;
        uint16_t tile_row = tile_index / grid->cols;

        // Decode into current tile buffer
        uint8_t *prev = grid->prev_tiles + tile_index * TILE_PIXELS;
        uint8_t *curr = grid->current_tiles + tile_index * TILE_PIXELS;
        decode_tile(ptr, comp_size, prev, curr, TILE_PIXELS);

        // Copy decoded tile to backbuffer
        uint16_t dst_x = tile_col * TILE_SIZE;
        uint16_t dst_y = tile_row * TILE_SIZE + chrome_height;
        blit_tile_to_backbuffer(curr, dst_x, dst_y, TILE_SIZE, TILE_SIZE);

        // Mark region as dirty for VGA flush
        mark_dirty(dst_x, dst_y, TILE_SIZE, TILE_SIZE);

        // Update prev_tiles for next delta
        memcpy(prev, curr, TILE_PIXELS);

        ptr += comp_size;
    }
}
```

#### 5.6.3 Full Frame Handling

A `FRAME_FULL` message contains ALL tiles (used on initial page load, navigation, or full refresh). Same format as `FRAME_DELTA` but with `tile_count` equal to the total tile grid size. The Pi may split this across multiple messages using the `CONTINUED` flag.

### 5.7 Software Mouse Cursor (`cursor.c`)

VESA modes do not support hardware mouse cursors. The DOS client renders a software cursor:

```c
#define CURSOR_MAX_SIZE 16

typedef struct {
    uint8_t  width, height;
    uint8_t  hotspot_x, hotspot_y;
    uint8_t  pixels[CURSOR_MAX_SIZE * CURSOR_MAX_SIZE];  // 0 = transparent
    uint8_t  save_under[CURSOR_MAX_SIZE * CURSOR_MAX_SIZE];
    int16_t  save_x, save_y;
    uint8_t  visible;
} SoftCursor;

void cursor_save_under(SoftCursor *c, uint8_t *backbuffer, int stride);
void cursor_draw(SoftCursor *c, uint8_t *backbuffer, int stride, int x, int y);
void cursor_restore(SoftCursor *c, uint8_t *backbuffer, int stride);
```

The cursor rendering cycle in the main loop:
1. **Restore** the backbuffer pixels under the previous cursor position
2. Apply any incoming tile updates to the backbuffer
3. **Save** the backbuffer pixels under the new cursor position
4. **Draw** the cursor at the new position
5. Flush dirty regions (including old and new cursor areas) to VGA

This ensures the cursor never "burns in" to the backbuffer or corrupts tile data.

### 5.8 Main Loop (`main.c`)

```c
int main(int argc, char *argv[]) {
    // 1. Parse RETROSURF.CFG
    Config cfg;
    config_load("RETROSURF.CFG", &cfg);

    // 2. Initialize video (auto-detect best mode)
    VideoConfig video;
    video_init(&video);

    // 3. Initialize mouse
    input_init_mouse(video.width, video.height);

    // 4. Initialize network and connect to Pi
    int sock = network_init(cfg.server_ip, cfg.server_port);

    // 5. Send CLIENT_HELLO
    send_client_hello(sock, &video);

    // 6. Wait for SERVER_HELLO + PALETTE + initial FRAME_FULL
    wait_for_handshake(sock, &video);

    // 7. Set the VGA palette from received palette data
    video_set_palette(video.palette);

    // --- Main Loop ---
    while (!quit_requested) {
        uclock_t frame_start = uclock();

        // A. Poll network -- receive and process all pending messages
        //    This calls tcp_tick(NULL) internally
        network_poll(sock, &recv_buffer);

        // B. Poll keyboard
        input_poll_keyboard();

        // C. Poll mouse
        MouseState ms;
        input_poll_mouse(&ms);

        // D. Handle mouse interaction
        //    - Check if click hits UI chrome (address bar, nav buttons)
        //    - Check if click hits an interactive element
        //    - Send appropriate events to Pi
        handle_mouse_interaction(&ms, &interaction_map);

        // E. Send throttled mouse position to Pi (for hover)
        maybe_send_mouse_move(&ms);

        // F. Update local text editing (if active)
        if (active_text_input) {
            update_local_text_input();
        }

        // G. Render cursor (restore -> draw at new position)
        cursor_restore(&cursor, video.backbuffer, video.content_width);
        cursor_save_under(&cursor, video.backbuffer, video.content_width);
        cursor_draw(&cursor, video.backbuffer, video.content_width,
                    ms.x, ms.y);

        // H. Flush dirty regions to VGA
        flush_dirty_to_vga(&video);

        // I. Keepalive (every 30 seconds)
        maybe_send_keepalive(sock);
    }

    // Cleanup
    video_shutdown(&video);
    network_close(sock);
    return 0;
}
```

**Target loop frequency**: As fast as possible (no artificial frame cap). The loop is naturally limited by network I/O and VGA write speed. Expected: 30-60 iterations/second with minimal updates, 10-20 during heavy tile streaming.

---

## 6. Pi Server Design

### 6.1 Source File Structure

```
pi_server/
  server.py               -- Main server entry point, TCP listener
  session.py              -- Browser session management (Playwright lifecycle)
  browser_bridge.py       -- Chromium interaction (screenshots, element detection)
  image_pipeline.py       -- Screenshot -> palette -> dither -> tile -> compress
  protocol.py             -- Message encoding/decoding
  interaction_detector.py -- Interactive element detection and classification
  palette.py              -- Fixed palette generation and LUT building
  config.py               -- Server configuration
  requirements.txt        -- Python dependencies
  install.sh              -- Pi setup script
```

### 6.2 Dependencies

```
# requirements.txt
playwright>=1.40
Pillow>=10.0
numpy>=1.24
```

### 6.3 Server Architecture

The Pi server uses **Python asyncio** for concurrent handling of:
- TCP connections from DOS clients
- Playwright browser automation
- CDP Screencast frame events
- Periodic interaction map updates

```python
import asyncio
from playwright.async_api import async_playwright

class RetroSurfServer:
    def __init__(self, config):
        self.config = config
        self.sessions = {}  # session_id -> BrowserSession

    async def start(self):
        # Start Playwright browser (one instance, shared)
        self.playwright = await async_playwright().start()
        self.browser = await self.playwright.chromium.launch(
            headless=True,
            args=[
                '--disable-gpu',
                '--disable-software-rasterizer',
                '--no-sandbox',
                '--disable-dev-shm-usage',
                '--disable-extensions',
                '--disable-background-networking',
                '--disable-sync',
                '--disable-translate',
            ]
        )

        # Start TCP server
        server = await asyncio.start_server(
            self.handle_client,
            '0.0.0.0',
            self.config.port
        )
        await server.serve_forever()

    async def handle_client(self, reader, writer):
        # Read CLIENT_HELLO
        hello = await read_message(reader)
        session = self.get_or_create_session(hello)

        # Configure viewport to match DOS client
        await session.configure_viewport(
            hello.screen_width,
            hello.screen_height - hello.chrome_height,
            hello.color_depth,
            hello.tile_size
        )

        # Send SERVER_HELLO + PALETTE + initial FRAME_FULL
        await send_server_hello(writer, session)
        await send_palette(writer, session.palette)
        await session.send_full_frame(writer)
        await session.send_interaction_map(writer)

        # Main client loop -- bidirectional async
        await asyncio.gather(
            self.receive_loop(reader, session),
            self.push_loop(writer, session),
        )
```

### 6.4 Browser Session (`session.py`)

Each DOS client gets a `BrowserSession` with its own Chromium page:

```python
class BrowserSession:
    def __init__(self, browser, session_id):
        self.session_id = session_id
        self.context = None
        self.page = None
        self.viewport_width = 640
        self.viewport_height = 456
        self.pipeline = None  # ImagePipeline instance
        self.dirty = True     # full frame needed
        self.frame_pending = asyncio.Event()

    async def configure_viewport(self, width, height, color_depth, tile_size):
        self.viewport_width = width
        self.viewport_height = height

        self.context = await self.browser.new_context(
            viewport={'width': width, 'height': height},
            device_scale_factor=1,
        )
        self.page = await self.context.new_page()

        # Inject CSS to disable animations/transitions (reduces unnecessary updates)
        await self.page.add_init_script('''
            const style = document.createElement('style');
            style.textContent = `
                *, *::before, *::after {
                    animation-duration: 0s !important;
                    animation-delay: 0s !important;
                    transition-duration: 0s !important;
                    transition-delay: 0s !important;
                    scroll-behavior: auto !important;
                }
            `;
            document.head.appendChild(style);
        ''')

        # Inject MutationObserver for dirty region tracking
        await self.page.add_init_script('''
            window.__retrosurf_dirty = true;  // start dirty (full frame needed)

            const observer = new MutationObserver(() => {
                window.__retrosurf_dirty = true;
            });

            if (document.body) {
                observer.observe(document.body, {
                    childList: true, attributes: true,
                    characterData: true, subtree: true
                });
            }

            // Also track scroll and resize
            window.addEventListener('scroll', () => {
                window.__retrosurf_dirty = true;
            }, {passive: true, capture: true});

            window.addEventListener('resize', () => {
                window.__retrosurf_dirty = true;
            });
        ''')

        # Initialize image pipeline
        self.pipeline = ImagePipeline(width, height, color_depth, tile_size)

    async def navigate(self, url):
        await self.page.goto(url, wait_until='domcontentloaded')
        self.dirty = True
        self.frame_pending.set()
```

### 6.5 Push Loop (Server -> Client Streaming)

The push loop continuously checks for visual changes and sends delta updates:

```python
async def push_loop(self, writer, session):
    """Continuously push frame updates and interaction maps to the client."""

    FRAME_INTERVAL = 0.05  # 20 FPS max check rate
    INTERACTION_INTERVAL = 0.5  # Re-scan interactive elements every 500ms

    last_interaction_time = 0

    while True:
        await asyncio.sleep(FRAME_INTERVAL)

        # Check if page content has changed
        try:
            is_dirty = await session.page.evaluate(
                '() => { const d = window.__retrosurf_dirty; '
                'window.__retrosurf_dirty = false; return d; }'
            )
        except Exception:
            continue

        if not is_dirty and not session.dirty:
            continue

        # Capture screenshot as JPEG bytes
        screenshot_bytes = await session.page.screenshot(
            type='jpeg',
            quality=70,
        )

        # Process through image pipeline
        delta_tiles = session.pipeline.process_frame(screenshot_bytes)

        if delta_tiles:
            # Send FRAME_DELTA message(s)
            # Split into chunks if total payload > 60KB
            await self.send_frame_delta(writer, delta_tiles)

        session.dirty = False

        # Periodically update interaction map
        now = asyncio.get_event_loop().time()
        if now - last_interaction_time > INTERACTION_INTERVAL:
            elements = await detect_interactive_elements(session.page)
            await self.send_interaction_map(writer, elements)
            last_interaction_time = now
```

### 6.6 Receive Loop (Client -> Server Events)

```python
async def receive_loop(self, reader, session):
    """Process incoming messages from the DOS client."""

    while True:
        msg_type, payload = await read_message(reader)

        if msg_type == MSG_MOUSE_EVENT:
            x, y, buttons, event_type = parse_mouse_event(payload)
            await self.handle_mouse(session, x, y, buttons, event_type)

        elif msg_type == MSG_KEY_EVENT:
            scancode, ascii_val, modifiers, event_type = parse_key_event(payload)
            await self.handle_key(session, scancode, ascii_val, modifiers, event_type)

        elif msg_type == MSG_SCROLL_EVENT:
            direction, amount = parse_scroll_event(payload)
            await self.handle_scroll(session, direction, amount)

        elif msg_type == MSG_TEXT_INPUT:
            element_id, cursor_pos, text = parse_text_input(payload)
            await self.handle_text_input(session, element_id, cursor_pos, text)

        elif msg_type == MSG_NAVIGATE:
            url = parse_navigate(payload)
            await session.navigate(url)

        elif msg_type == MSG_NAV_ACTION:
            action = parse_nav_action(payload)
            await self.handle_nav_action(session, action)

        elif msg_type == MSG_KEEPALIVE:
            await send_keepalive_ack(writer)
```

### 6.7 Event Injection

Mouse and keyboard events from the DOS client are injected into Chromium via Playwright:

```python
async def handle_mouse(self, session, x, y, buttons, event_type):
    """Inject mouse event into the browser page."""
    if event_type == 0:  # move
        await session.page.mouse.move(x, y)
    elif event_type == 1:  # click
        button = 'left'
        if buttons & 0x02: button = 'right'
        elif buttons & 0x04: button = 'middle'
        await session.page.mouse.click(x, y, button=button)
    elif event_type == 2:  # release
        await session.page.mouse.up()
    elif event_type == 3:  # double-click
        await session.page.mouse.dblclick(x, y)

    session.dirty = True
    session.frame_pending.set()

async def handle_key(self, session, scancode, ascii_val, modifiers, event_type):
    """Inject keyboard event. Maps DOS scancodes to web key names."""
    key = dos_scancode_to_web_key(scancode, ascii_val)
    if event_type == 0:  # press
        await session.page.keyboard.press(key)
    elif event_type == 1:  # release
        await session.page.keyboard.up(key)

    session.dirty = True

async def handle_text_input(self, session, element_id, cursor_pos, text):
    """Handle text synced from local DOS editing."""
    # Clear the element and type the new text
    # Uses JavaScript to set the value directly for reliability
    await session.page.evaluate('''
        ([id, text]) => {
            const el = document.querySelector(`[data-retrosurf-id="${id}"]`);
            if (el) {
                if (el.tagName === 'INPUT' || el.tagName === 'TEXTAREA') {
                    el.value = text;
                    el.dispatchEvent(new Event('input', {bubbles: true}));
                }
            }
        }
    ''', [element_id, text])

    session.dirty = True

async def handle_scroll(self, session, direction, amount):
    """Inject scroll event."""
    delta_y = amount * 100 * (1 if direction == 0 else -1)  # 0=down, 1=up
    await session.page.mouse.wheel(0, delta_y)
    session.dirty = True
```

---

## 7. Image Processing Pipeline

### 7.1 Overview

The image pipeline converts a Chromium screenshot (JPEG, full color) into a compressed tile-delta format that the DOS client can decompress and display. This runs on the Pi for every visual update.

```
JPEG screenshot (from Playwright)
    |
    v
Decode to NumPy array (H x W x 3, uint8 RGB)        ~5-10ms
    |
    v
Palette quantization via precomputed 3D LUT           ~3-5ms
    -> produces (H x W) uint8 palette indices
    |
    v
Ordered dithering (Bayer 4x4 matrix)                  ~3-5ms
    -> applied BEFORE LUT for better visual quality
    |
    v
Tile segmentation (16x16 grid)                        ~1ms
    |
    v
Delta detection (XOR with previous frame tiles)        ~3-5ms
    -> identifies which tiles changed
    |
    v
RLE compression of XOR-delta tiles                    ~2-5ms
    -> only for changed tiles
    |
    v
Binary protocol message assembly                      ~1ms
    |
    v
Total: ~18-32ms per frame (31-55 FPS processing capacity)
```

### 7.2 Fixed Palette Design (`palette.py`)

A fixed 256-color palette is used across all sessions. This is precomputed once and never changes. Using a fixed palette (vs. per-frame adaptive) enables the precomputed LUT optimization and ensures consistent color mapping across frames (critical for XOR delta to work well).

**Palette allocation strategy:**

```
Indices 0-215:    6x6x6 RGB color cube (216 colors)
                  R,G,B each take values: 0, 51, 102, 153, 204, 255
Indices 216-239:  24 grayscale ramp (excluding black and white)
                  Values: 8, 18, 28, 38, ..., 238
Indices 240-249:  10 "web UI" colors for common interface elements
                  (selection blue, link blue, error red, success green,
                   warning yellow, border gray, shadow, etc.)
Indices 250-254:  5 reserved for DOS client UI chrome
                  (chrome bg, chrome text, address bg, address text, highlight)
Index 255:        Transparent / cursor key color
```

```python
import numpy as np

def build_256_palette():
    palette = np.zeros((256, 3), dtype=np.uint8)

    # 6x6x6 color cube (indices 0-215)
    idx = 0
    for r in range(6):
        for g in range(6):
            for b in range(6):
                palette[idx] = [r * 51, g * 51, b * 51]
                idx += 1

    # 24 grayscale ramp (indices 216-239)
    for i in range(24):
        v = 8 + i * 10
        palette[216 + i] = [v, v, v]

    # 10 web UI colors (indices 240-249)
    web_colors = [
        [0, 102, 204],    # Selection blue
        [0, 0, 238],      # Link blue
        [204, 0, 0],      # Error red
        [0, 153, 0],      # Success green
        [255, 204, 0],    # Warning yellow
        [192, 192, 192],  # Border gray
        [128, 128, 128],  # Shadow gray
        [240, 240, 240],  # Light background
        [51, 51, 51],     # Dark text
        [255, 255, 255],  # White
    ]
    for i, color in enumerate(web_colors):
        palette[240 + i] = color

    # 5 chrome colors (indices 250-254)
    palette[250] = [64, 64, 64]     # Chrome background
    palette[251] = [255, 255, 255]  # Chrome text
    palette[252] = [240, 240, 240]  # Address bar background
    palette[253] = [0, 0, 0]        # Address bar text
    palette[254] = [0, 120, 215]    # Highlight/focus

    palette[255] = [255, 0, 255]    # Transparent/key (magenta)

    return palette
```

**16-color palette** (for VGA mode 12h fallback):
Uses the standard VGA 16-color palette (black, dark blue, dark green, dark cyan, dark red, dark magenta, brown, light gray, dark gray, bright blue, bright green, bright cyan, bright red, bright magenta, yellow, white). Dithering is much heavier in this mode.

### 7.3 Precomputed LUT (`palette.py`)

The 3D LUT maps any RGB value to the nearest palette index in O(1) time:

```python
def build_palette_lut(palette, bits=5):
    """Build a 3D lookup table for fast RGB -> palette index mapping.

    Args:
        palette: (256, 3) uint8 array of RGB palette colors
        bits: bits per channel for LUT resolution (5 = 32x32x32 = 32KB LUT)

    Returns:
        (size, size, size) uint8 array where LUT[r][g][b] = palette_index
    """
    size = 1 << bits
    step = 256 // size

    # Generate all possible quantized RGB values
    vals = np.arange(size) * step + step // 2  # center of each bucket
    rr, gg, bb = np.meshgrid(vals, vals, vals, indexing='ij')
    grid = np.stack([rr, gg, bb], axis=-1).reshape(-1, 3).astype(np.float32)

    # Find nearest palette entry for each grid point
    pal = palette.astype(np.float32)
    # Compute distances in chunks to avoid memory explosion
    lut_flat = np.empty(size ** 3, dtype=np.uint8)
    chunk = 1024
    for i in range(0, len(grid), chunk):
        dists = np.sum((grid[i:i+chunk, None, :] - pal[None, :, :]) ** 2, axis=2)
        lut_flat[i:i+chunk] = np.argmin(dists, axis=1).astype(np.uint8)

    return lut_flat.reshape(size, size, size)

def apply_lut(img_rgb, lut, bits=5):
    """Map an RGB image to palette indices using the precomputed LUT.

    Args:
        img_rgb: (H, W, 3) uint8 RGB image
        lut: precomputed LUT from build_palette_lut()

    Returns:
        (H, W) uint8 palette index image
    """
    shift = 8 - bits
    r = img_rgb[:, :, 0] >> shift
    g = img_rgb[:, :, 1] >> shift
    b = img_rgb[:, :, 2] >> shift
    return lut[r, g, b]
```

**Performance**: LUT build takes ~50ms (one-time at startup). LUT apply takes **~3ms** for a 640x480 image on Pi 4. This is the critical optimization that makes the whole pipeline viable.

### 7.4 Ordered Dithering (`image_pipeline.py`)

Ordered (Bayer) dithering is applied **before** palette mapping. It adds a spatial threshold pattern that creates the illusion of more colors through pixel-level mixing.

```python
# Bayer 4x4 ordered dithering matrix
BAYER_4x4 = np.array([
    [ 0,  8,  2, 10],
    [12,  4, 14,  6],
    [ 3, 11,  1,  9],
    [15,  7, 13,  5],
], dtype=np.float32) / 16.0 - 0.5  # normalize to [-0.5, +0.5]

def apply_ordered_dither(img_rgb, bayer=BAYER_4x4, strength=32.0):
    """Apply ordered dithering to an RGB image.

    Args:
        img_rgb: (H, W, 3) uint8 RGB image
        bayer: dither matrix (normalized to [-0.5, +0.5])
        strength: dither amplitude in color levels (higher = more visible dither)

    Returns:
        (H, W, 3) uint8 dithered RGB image
    """
    h, w, _ = img_rgb.shape
    bh, bw = bayer.shape

    # Tile the Bayer matrix across the image
    threshold = np.tile(bayer, (h // bh + 1, w // bw + 1))[:h, :w]

    # Apply threshold to all channels
    img_f = img_rgb.astype(np.float32)
    for c in range(3):
        img_f[:, :, c] += threshold * strength

    return np.clip(img_f, 0, 255).astype(np.uint8)
```

**Dither strength tuning:**
- For 256 colors (6x6x6 cube): `strength=32.0` works well. The color cube has steps of ~51 levels, so +-16 levels of dither creates smooth gradients.
- For 16 colors: `strength=64.0` or higher. More aggressive dithering needed to compensate for the tiny palette.

### 7.5 Tile Delta Detection and Compression (`image_pipeline.py`)

```python
class ImagePipeline:
    def __init__(self, width, height, color_depth, tile_size=16):
        self.width = width
        self.height = height
        self.tile_size = tile_size
        self.tile_cols = width // tile_size
        self.tile_rows = (height + tile_size - 1) // tile_size  # ceil division
        self.prev_indexed = None  # Previous frame as palette indices
        self.palette = build_256_palette() if color_depth == 8 else build_16_palette()
        self.lut = build_palette_lut(self.palette)

    def process_frame(self, jpeg_bytes):
        """Process a JPEG screenshot into compressed delta tiles.

        Returns: list of (tile_index, compressed_bytes) tuples
        """
        # 1. Decode JPEG
        from PIL import Image
        import io
        img = Image.open(io.BytesIO(jpeg_bytes)).convert('RGB')
        img_rgb = np.asarray(img)

        # 2. Apply ordered dithering
        img_dithered = apply_ordered_dither(img_rgb)

        # 3. Map to palette indices via LUT
        indexed = apply_lut(img_dithered, self.lut)

        # 4. Find changed tiles
        if self.prev_indexed is None:
            # First frame -- all tiles are "changed"
            changed_tiles = list(range(self.tile_cols * self.tile_rows))
        else:
            changed_tiles = self._find_changed_tiles(indexed)

        # 5. Compress changed tiles as XOR delta + RLE
        result = []
        for tile_idx in changed_tiles:
            row = tile_idx // self.tile_cols
            col = tile_idx % self.tile_cols
            y0 = row * self.tile_size
            x0 = col * self.tile_size
            y1 = min(y0 + self.tile_size, self.height)
            x1 = min(x0 + self.tile_size, self.width)

            new_tile = indexed[y0:y1, x0:x1].copy()

            if self.prev_indexed is not None:
                old_tile = self.prev_indexed[y0:y1, x0:x1]
                delta = np.bitwise_xor(new_tile, old_tile)
            else:
                delta = new_tile

            # Flatten and RLE compress
            compressed = rle_compress(delta.flatten().tobytes())
            result.append((tile_idx, compressed))

        # 6. Update previous frame
        self.prev_indexed = indexed

        return result

    def _find_changed_tiles(self, indexed):
        """Find tile indices where pixels differ from previous frame."""
        diff = indexed != self.prev_indexed  # boolean (H, W)

        changed = []
        for row in range(self.tile_rows):
            for col in range(self.tile_cols):
                y0 = row * self.tile_size
                x0 = col * self.tile_size
                y1 = min(y0 + self.tile_size, self.height)
                x1 = min(x0 + self.tile_size, self.width)
                if np.any(diff[y0:y1, x0:x1]):
                    changed.append(row * self.tile_cols + col)

        return changed


def rle_compress(data):
    """RLE compress a bytes object.

    Format: control byte + data
      Control bit 7 = 1: repeat run (next byte repeated <len> times)
      Control bit 7 = 0: literal run (next <len> bytes are literal)
    """
    result = bytearray()
    i = 0
    n = len(data)

    while i < n:
        # Check for a run of identical bytes
        run_start = i
        while i < n - 1 and data[i] == data[i + 1] and i - run_start < 126:
            i += 1
        run_len = i - run_start + 1

        if run_len >= 3:
            # Repeat run (worth encoding as run if >= 3 bytes)
            result.append(0x80 | run_len)
            result.append(data[run_start])
            i = run_start + run_len
        else:
            # Literal run -- collect non-repeating bytes
            lit_start = run_start
            i = run_start
            while i < n and i - lit_start < 127:
                # Look ahead: if next 3+ bytes are identical, stop literal here
                if (i < n - 2 and data[i] == data[i + 1] == data[i + 2]):
                    break
                i += 1

            lit_len = i - lit_start
            if lit_len > 0:
                result.append(lit_len)  # bit 7 = 0
                result.extend(data[lit_start:lit_start + lit_len])

    return bytes(result)
```

### 7.6 Pipeline Performance Summary (Pi 4, 640x480)

| Stage | Time | Notes |
|-------|------|-------|
| JPEG decode | 5-10ms | via PIL/libjpeg-turbo |
| Ordered dither | 3-5ms | NumPy vectorized |
| Palette LUT apply | 3-5ms | 3D array indexing |
| Tile change detection | 3-5ms | NumPy boolean diff |
| XOR delta + RLE (30 tiles) | 2-5ms | Python + NumPy |
| XOR delta + RLE (1160 tiles, full) | 15-30ms | Full frame, worst case |
| Message assembly | <1ms | struct packing |
| **Total (partial update, ~30 tiles)** | **~18-32ms** | **~31-55 FPS capacity** |
| **Total (full frame, ~1160 tiles)** | **~35-60ms** | **~17-29 FPS capacity** |

The Pi can process frames faster than the DOS client can receive and display them. The bottleneck is the NE2000 network throughput on the DOS side.

---

## 8. Interactive Element System

### 8.1 Overview

Interactive elements (form fields, buttons, links, etc.) are detected by the Pi and reported to the DOS client via the `INTERACTION_MAP` message. The DOS client uses this map to provide instant-response local interactions where possible.

### 8.2 Element Detection (`interaction_detector.py`)

```python
async def detect_interactive_elements(page):
    """Detect all interactive elements and their bounding boxes.

    Returns a list of dicts with element info.
    """
    elements = await page.evaluate('''() => {
        const results = [];
        let nextId = 1;

        const selectors = [
            'a[href]',
            'button',
            'input:not([type="hidden"])',
            'select',
            'textarea',
            '[contenteditable="true"]',
            '[role="button"]',
            '[role="link"]',
            '[role="checkbox"]',
            '[role="radio"]',
            '[role="textbox"]',
            '[role="combobox"]',
            '[role="tab"]',
            '[tabindex]:not([tabindex="-1"])',
        ];

        const allElements = document.querySelectorAll(selectors.join(','));
        const seen = new Set();

        for (const el of allElements) {
            if (seen.has(el)) continue;
            seen.add(el);

            const rect = el.getBoundingClientRect();
            if (rect.width < 2 || rect.height < 2) continue;

            const style = window.getComputedStyle(el);
            if (style.display === 'none' || style.visibility === 'hidden') continue;
            if (parseFloat(style.opacity) < 0.1) continue;

            // Determine element type
            let elemType = 0x09; // default: CUSTOM_WIDGET
            const tag = el.tagName.toLowerCase();
            const type = (el.type || '').toLowerCase();

            if (tag === 'a') elemType = 0x00; // LINK
            else if (tag === 'button' || type === 'button' || type === 'submit')
                elemType = 0x01; // BUTTON
            else if (tag === 'input' && (type === 'text' || type === 'search' ||
                     type === 'email' || type === 'url' || type === 'tel' ||
                     type === 'number' || type === ''))
                elemType = 0x02; // TEXT_INPUT
            else if (tag === 'textarea')
                elemType = 0x03; // TEXT_AREA
            else if (tag === 'input' && type === 'password')
                elemType = 0x04; // PASSWORD
            else if (tag === 'input' && type === 'checkbox')
                elemType = 0x05; // CHECKBOX
            else if (tag === 'input' && type === 'radio')
                elemType = 0x06; // RADIO
            else if (tag === 'select')
                elemType = 0x07; // SELECT
            else if (el.isContentEditable)
                elemType = 0x08; // CONTENTEDITABLE

            // Determine font size bucket
            const fontSize = parseFloat(style.fontSize);
            let fontBucket = 1; // medium (8x14)
            if (fontSize <= 10) fontBucket = 0; // small (6x8)
            else if (fontSize >= 18) fontBucket = 2; // large (8x16)

            // Get computed colors (approximate to nearest palette entry)
            const color = style.color;
            const bgColor = style.backgroundColor;

            // Assign a data attribute for later targeting
            const id = nextId++;
            el.setAttribute('data-retrosurf-id', id);

            results.push({
                id: id,
                x: Math.round(rect.x),
                y: Math.round(rect.y),
                w: Math.round(rect.width),
                h: Math.round(rect.height),
                type: elemType,
                focused: document.activeElement === el,
                disabled: el.disabled || false,
                checked: el.checked || false,
                isPassword: type === 'password',
                fontBucket: fontBucket,
                value: (el.value || el.innerText || '').substring(0, 500),
                textColor: color,
                bgColor: bgColor,
            });
        }
        return results;
    }''')

    return elements
```

### 8.3 Local Text Editing (DOS Client, `interact.c`)

When the user clicks into a `TEXT_INPUT`, `TEXT_AREA`, or `PASSWORD` element, the DOS client activates local editing mode:

```c
typedef struct {
    uint16_t element_id;
    uint16_t x, y, w, h;         // Screen position (content-area-relative)
    uint8_t  type;                // Element type
    uint8_t  font_size;           // 0=small, 1=medium, 2=large
    uint8_t  text_color;          // Palette index
    uint8_t  bg_color;            // Palette index
    char     text[512];           // Current text content
    uint16_t text_len;
    uint16_t cursor_pos;          // Character position of cursor
    uint16_t scroll_offset;       // Horizontal scroll for long text
    uint8_t  is_password;
    uint8_t  active;              // Currently being edited
} LocalTextInput;

static LocalTextInput active_input;
```

**Behavior when local editing is active:**

1. Keyboard input is captured locally (not sent to Pi)
2. Printable characters are inserted at `cursor_pos`
3. Backspace/Delete remove characters
4. Left/Right arrows move cursor
5. Home/End jump to start/end
6. The text field rectangle in the backbuffer is redrawn by the DOS client:
   - Fill with `bg_color`
   - Draw text (or asterisks for passwords) using the appropriate bitmap font
   - Draw blinking cursor at `cursor_pos`
7. Every 200ms (or on Enter/Tab), the full text is synced to the Pi via `TEXT_INPUT` message
8. Enter or Tab deactivates local editing and sends the final text
9. Escape cancels editing and restores the original text

**Why local editing**: This gives **zero-latency keystroke response**. The user sees each character appear instantly. The Pi is updated periodically (every 200ms) so it can process auto-suggestions, validation, etc. and push back frame updates if the page changes.

### 8.4 Forwarding Mode (DOS Client)

For `CONTENTEDITABLE` and `CUSTOM_WIDGET` elements, the DOS client cannot reliably render text locally because these elements may have complex formatting, custom rendering, or JavaScript behavior that only Chromium can handle.

In forwarding mode:
1. All keystrokes are sent as `KEY_EVENT` messages to the Pi immediately
2. The Pi injects them into Chromium
3. Chromium processes them (text appears, autocomplete fires, etc.)
4. The visual result comes back as a `FRAME_DELTA` in the next update cycle

**Latency in forwarding mode**: ~50-200ms per keystroke (round-trip). This is noticeable but acceptable for rich text editors. The status bar shows "DIRECT" to indicate the user is in forwarding mode.

### 8.5 Click Handling

When the user clicks in the content area:

```c
void handle_content_click(int x, int y, int button) {
    // Convert to content-area coordinates
    int content_y = y - chrome_height;

    // Search interaction map for hit
    ElementEntry *hit = find_element_at(x, content_y);

    if (hit) {
        switch (hit->type) {
            case ELEM_LINK:
            case ELEM_BUTTON:
                // Send click to Pi, it handles navigation/action
                send_mouse_event(x, content_y, button, EVENT_CLICK);
                break;

            case ELEM_TEXT_INPUT:
            case ELEM_TEXT_AREA:
            case ELEM_PASSWORD:
                // Activate local editing
                activate_local_input(hit);
                // Also send click to Pi so Chromium focuses the element
                send_mouse_event(x, content_y, button, EVENT_CLICK);
                break;

            case ELEM_CHECKBOX:
            case ELEM_RADIO:
                // Toggle locally for instant visual feedback
                hit->checked = !hit->checked;
                redraw_element(hit);
                // Send click to Pi for actual state change
                send_mouse_event(x, content_y, button, EVENT_CLICK);
                break;

            case ELEM_SELECT:
                // Send click to Pi -- it will render the dropdown
                // as part of the next frame update
                send_mouse_event(x, content_y, button, EVENT_CLICK);
                break;

            case ELEM_CONTENTEDITABLE:
            case ELEM_CUSTOM_WIDGET:
                // Activate forwarding mode
                activate_forwarding_mode(hit);
                send_mouse_event(x, content_y, button, EVENT_CLICK);
                break;
        }
    } else {
        // No interactive element -- send raw click to Pi
        send_mouse_event(x, content_y, button, EVENT_CLICK);
    }
}
```

### 8.6 Scrolling

Scrolling is handled as **Pi-controlled with delta optimization**:

1. Mouse wheel or Page Up/Page Down detected by DOS client
2. `SCROLL_EVENT` sent to Pi
3. Pi injects scroll into Chromium (`page.mouse.wheel()`)
4. Chromium scrolls, page content changes
5. MutationObserver / scroll listener fires, marks page as dirty
6. Next frame cycle captures updated screenshot
7. Delta tiles sent to DOS client (only the changed content)

**Scroll wheel mapping:**
```c
void handle_scroll_wheel(int direction) {
    struct scroll_event evt;
    evt.direction = direction;  // 0=down, 1=up
    evt.amount = 3;             // lines per scroll step
    network_send_message(sock, MSG_SCROLL_EVENT, &evt, sizeof(evt));
}
```

For machines without a scroll wheel, keyboard alternatives:
- Page Up / Page Down: `amount = 10` (jump a full page)
- Arrow Up / Arrow Down (when not in a text field): `amount = 1`

---

## 9. Performance Budget

### 9.1 DOS Client CPU Budget (25MHz 486)

Total budget: 25,000,000 cycles/second

| Task | Cycles/Frame | % CPU @ 15 FPS | Notes |
|------|-------------|----------------|-------|
| NE2000 packet reception | ~20,000/pkt x ~25 pkts | 30% | ~500,000 cycles for typical frame delta |
| RLE decompression (30 tiles) | ~5,000/tile x 30 | 9% | ~150,000 cycles |
| XOR delta apply (30 tiles) | ~500/tile x 30 | 1% | ~15,000 cycles |
| Backbuffer tile blit (30 tiles) | ~500/tile x 30 | 1% | memcpy 256 bytes x 30 |
| VGA flush (dirty rects) | ~100,000 | 6% | Partial backbuffer copy to LFB |
| Mouse polling + cursor render | ~5,000 | <1% | INT 33h + save/draw cursor |
| Keyboard polling | ~1,000 | <1% | kbhit() + getch() |
| Protocol parsing | ~10,000 | <1% | Fixed-header parsing is trivial |
| Local text rendering | ~20,000 | 1% | Only when editing |
| **Total** | | **~50%** | **~50% CPU headroom** |

This leaves comfortable headroom for spikes (full frame updates, complex interactions).

### 9.2 Network Bandwidth Budget

NE2000 on 25MHz 486: ~300-400 KB/s practical throughput

| Scenario | Data Size | @ 350 KB/s | Achievable FPS |
|----------|-----------|------------|----------------|
| Full frame (640x456, all tiles) | ~150-250 KB compressed | 0.4-0.7s | 1.4-2.5 |
| Scroll (40% tiles changed) | ~60-100 KB compressed | 170-285ms | 3.5-6 |
| Small interaction (10% tiles) | ~15-25 KB compressed | 43-71ms | 14-23 |
| Typing feedback (5% tiles) | ~8-13 KB compressed | 23-37ms | 27-43 |
| Hover effect (2-3 tiles) | ~1-2 KB compressed | 3-6ms | 160+ |
| Static page (no change) | 0 KB | 0ms | infinite |

**Key insight**: The system is most responsive during typical usage (clicking, typing, small UI changes). Full page loads and scrolling are slower but still well under 1 second.

### 9.3 Pi Server Performance Budget

Pi 4 is not the bottleneck. Key timings:

| Task | Time | Notes |
|------|------|-------|
| Screenshot capture (JPEG) | 80-180ms | The slowest step; limits max FPS to ~6-12 |
| Image pipeline (dither+LUT+tile+compress) | 18-32ms | Well within budget |
| Element detection | 5-15ms | JavaScript evaluate() |
| Event injection | 1-5ms | Playwright API calls |
| TCP send | <1ms | Gigabit Ethernet, tiny data |

**Effective end-to-end latency** (user action -> visual update on DOS screen):
- Best case (small change): ~150ms (screenshot + pipeline + network + decompress)
- Typical case: ~200-300ms
- Worst case (full page): ~700-1000ms

### 9.4 Memory Budget (DOS Client, 640x480 mode)

Starting from ~3200 KB usable:

| Allocation | Size (KB) | Notes |
|------------|-----------|-------|
| Program code + Watt-32 | ~200 | Statically linked |
| Stack | 64 | Reduced via stubedit |
| Watt-32 internal buffers | ~40 | Socket buffers, ARP, DNS |
| Backbuffer (640x480x8) | 307 | Double-buffer target |
| Prev tile cache | 290 | 1160 tiles x 256 bytes |
| Tile decode buffer | 1 | Single tile temp buffer |
| Receive ring buffer | 32 | Network receive |
| Send buffer | 4 | Network send |
| Interaction map | 20 | ~500 elements max |
| Cursor save-under | 1 | 16x16 pixels |
| Font data | 12 | 3 bitmap fonts |
| Config/strings/misc | 30 | Various |
| **Total used** | **~1001** | |
| **Remaining** | **~2199** | **Ample headroom** |

For 800x600 mode, add ~162KB for the larger backbuffer and ~112KB for more tiles. Still very comfortable.

---

## 10. Build & Deployment

### 10.1 Pi Server Setup

```bash
#!/bin/bash
# install.sh -- Run on Raspberry Pi 4 (Pi OS 64-bit)

# Update system
sudo apt update && sudo apt upgrade -y

# Install Python 3 and pip
sudo apt install -y python3 python3-pip python3-venv

# Create virtual environment
python3 -m venv /opt/retrosurf
source /opt/retrosurf/bin/activate

# Install dependencies
pip install playwright numpy Pillow

# Install Chromium for Playwright
playwright install chromium

# Create systemd service for auto-start
sudo cat > /etc/systemd/system/retrosurf.service << 'SERVICEEOF'
[Unit]
Description=RetroSurf Rendering Server
After=network.target

[Service]
Type=simple
User=pi
WorkingDirectory=/opt/retrosurf
ExecStart=/opt/retrosurf/bin/python /opt/retrosurf/server.py
Restart=always
RestartSec=5

[Install]
WantedBy=multi-user.target
SERVICEEOF

sudo systemctl enable retrosurf
sudo systemctl start retrosurf
```

**Server configuration** (`/opt/retrosurf/config.json`):
```json
{
    "port": 8086,
    "default_url": "https://www.google.com",
    "session_timeout_seconds": 300,
    "max_sessions": 2,
    "screenshot_quality": 70,
    "frame_check_interval_ms": 50,
    "interaction_scan_interval_ms": 500,
    "chromium_args": [
        "--disable-gpu",
        "--no-sandbox",
        "--disable-dev-shm-usage"
    ]
}
```

### 10.2 DOS Client Cross-Compilation (Windows)

All tools run natively on Windows. No WSL, no Linux, no admin rights needed.

**Tools (pre-installed in `tools/` directory):**
- `tools/djgpp/` -- DJGPP cross-compiler v3.4 (GCC 12.2.0, Windows native MinGW build)
- `tools/watt32/` -- Watt-32 TCP/IP library (compiled from source with DJGPP)
- `tools/dosbox-x/` -- DOSBox-X portable (for testing)

**Building:**
```
cd dos_client
build.bat
```

The Makefile auto-detects tool paths relative to the project directory.
Build uses `tools/watt32/util/win32/gnumake.exe` (bundled with Watt-32).
Output: `dos_client/build/RETRO.EXE`

**Testing in DOSBox-X:**
```
cd dos_client
run.bat
```

DOSBox-X launches with NE2000+SLIRP networking. The host machine is reachable
at 10.0.2.2, so the DOS client connects to 10.0.2.2:8086 to reach the
Pi server running on localhost.

### 10.3 DOS Machine Setup

**Files to copy to the DOS machine** (e.g., `C:\RSURF\`):
```
C:\RSURF\RSURF.EXE         -- The browser client
C:\RSURF\CWSDPMI.EXE       -- DPMI host (needed to run DJGPP programs)
C:\RSURF\RETROSURF.CFG     -- Configuration file
```

**RETROSURF.CFG:**
```ini
# RetroSurf Configuration
SERVER_IP=192.168.1.100
SERVER_PORT=8086
# VIDEO_MODE=auto
# auto | 800x600 | 640x480 | 640x480x16
```

**AUTOEXEC.BAT additions:**
```batch
@echo off
REM Load NE2000 packet driver (adjust IRQ and I/O for your card)
C:\DRIVERS\NE2000.COM 0x60 10 0x300

REM Load mouse driver
C:\DRIVERS\CTMOUSE.EXE
```

**Running RetroSurf:**
```
C:\RSURF\RSURF.EXE
```

Or with a command-line URL:
```
C:\RSURF\RSURF.EXE https://claude.ai
```

---

## 11. V1 Scope

### 11.1 V1 Included Features

| Feature | Status | Notes |
|---------|--------|-------|
| Multi-resolution support | Planned | Auto-detect: 800x600, 640x480 (256 and 16 color) |
| Full page rendering | **Server done** | 256-color ordered dither with fixed palette |
| Tile-based delta streaming | **Server done** | 16x16 tiles, XOR delta + RLE compression |
| Navigation (address bar) | **Server done** | Server handles navigation; DOS UI pending |
| Back / Forward / Reload / Stop | **Server done** | Server handles nav actions; DOS UI pending |
| Click links and buttons | **Server done** | Server injects clicks; DOS interaction map pending |
| Standard text inputs | **Server done** | Server injects text; DOS local editing pending |
| Password fields | **Server done** | Server-side ready; DOS asterisk display pending |
| Checkboxes and radio buttons | **Server done** | Server injects clicks; DOS local toggle pending |
| Select/dropdown menus | **Server done** | Pi-rendered (dropdown as frame update) |
| Rich text editors (contenteditable) | **Server done** | Server injects keys; DOS forwarding mode pending |
| Scrolling (wheel, keyboard, page) | **Server done** | Server injects scroll; DOS input pending |
| Hover states | **Server done** | Server injects mouse.move; DOS throttling pending |
| Full JS/AJAX/WebSocket support | **Server done** | All handled Pi-side by Chromium |
| Auth flows (login, OAuth, 2FA) | **Server done** | Rendered as normal pages |
| Streaming content (ChatGPT, etc.) | **Server done** | MutationObserver triggers frame updates |
| Session persistence across reconnect | **Server done** | 5-minute session timeout |
| Status bar (loading, URL, etc.) | **Server done** | Server sends STATUS messages; DOS display pending |
| DOS TCP networking | **Verified** | Watt-32 TCP, protocol encode/decode, handshake tested in DOSBox-X |
| Software mouse cursor | Planned | With cursor shape changes from Pi |
| Connection error handling | Planned | Graceful reconnect with session resume |

### 11.2 V1 Excluded Features (Tracked for Future)

| Feature | Priority | Complexity | Notes |
|---------|----------|------------|-------|
| Copy/paste | High | Medium | Need clipboard sync between DOS and Pi |
| File uploads | High | Medium | DOS file -> Pi -> browser upload |
| File downloads | High | Medium | Browser download -> Pi -> DOS file |
| Right-click context menus | Medium | Low | Send right-click to Pi, render result |
| Multiple tabs | Medium | High | Memory management, tab bar UI |
| Bookmarks | Medium | Low | Local file on DOS machine |
| Browsing history | Medium | Low | Local file on DOS machine |
| Find on page (Ctrl+F) | Medium | Medium | Pi-side search + highlight |
| Print | Low | Medium | Pi renders to printable format |
| Video playback | Low | Very High | Would need streaming decode on Pi |
| Audio playback | Low | Very High | Would need Sound Blaster streaming |
| Drag and drop | Low | High | Complex coordinate tracking |
| Keyboard shortcuts | Low | Low | Configurable via RETROSURF.CFG |
| Pi web admin panel | Low | Medium | Configure Pi settings from any browser |
| HTTPS certificate warnings | Low | Low | Pi-side handling + user notification |
| Cookie/cache management | Low | Medium | Pi-side with DOS UI for clear/view |
| Form auto-fill | Low | Medium | Pi-side storage + DOS prompt |

---

## 12. Future Enhancements

This section tracks ideas and features for post-V1 releases. Items move from Section 11.2 to here when implementation begins.

### 12.1 Performance Optimizations
- **Adaptive compression**: Switch between RLE and raw based on tile entropy
- **Tile priority**: Send tiles near the cursor first for perceived responsiveness
- **Screenshot region clipping**: Only screenshot dirty regions instead of full viewport
- **JPEG quality adaptation**: Lower quality during fast scrolling, higher when static
- **Packet coalescing**: Batch multiple small client->server messages per TCP send

### 12.2 Feature Ideas
- **Reader mode**: Strip page to text content, render at DOS-native text resolution
- **Image zoom**: Click to zoom into a specific region at higher resolution
- **Split screen**: Show two pages side-by-side (for reference browsing)
- **Macro recording**: Record and replay interaction sequences
- **Theme support**: Multiple DOS-side color schemes (amber, green, blue terminal)
- **Sound alerts**: PC speaker beep for notifications/errors

---

## 13. Changelog

All design changes, implementation progress, and decisions are tracked here in reverse chronological order.

### 2026-03-06 -- DOS Client Environment Ready (v0.3.0)

**DOS client development environment fully set up on Windows.** All tools run
natively -- no WSL, no Linux, no admin privileges required.

**Change from original design:** Section 10.2 originally specified building
from Pi or Linux with DJGPP compiled from source (~2 hours). Instead, we use
prebuilt Windows-native DJGPP binaries (andrewwutw/build-djgpp MinGW standalone
build). Watt-32 was cross-compiled on Windows using its own configur.bat +
gnumake. This means the entire development workflow is Windows-only.

**Tools downloaded and configured:**
- DJGPP cross-compiler v3.4 (GCC 12.2.0) -> `tools/djgpp/`
- Watt-32 TCP/IP library (compiled libwatt.a) -> `tools/watt32/`
- DOSBox-X portable (v2026.01.02) -> `tools/dosbox-x/`
- Crynwr NE2000 packet driver -> `dos_client/drivers/NE2000.COM`
- CWSDPMI DPMI host -> `dos_client/build/CWSDPMI.EXE`

**DOS client code (minimal network test):**
- `dos_client/src/protocol.h/c` -- C port of protocol.py (packed structs)
- `dos_client/src/network.h/c` -- Watt-32 TCP layer (non-blocking recv)
- `dos_client/src/main.c` -- Handshake test + receive loop with stats
- Compiles clean (zero warnings) with `build.bat`

### 2026-03-06 -- End-to-End Networking Verified (v0.4.0)

**DOSBox-X SLIRP networking fully working.** The DOS client successfully connects
to the Pi server running on the Windows host, completes the full handshake
(CLIENT_HELLO -> SERVER_HELLO -> PALETTE), and receives frame data.

**Test result:** 7 frames received, 271,583 bytes total over 10-second window.

**Issues resolved during debugging:**
- `sock_wait_established()` macro unreliable in DOSBox-X SLIRP -- replaced with
  manual polling loop using `tcp_tick()` + `sock_established()` (works perfectly)
- `inet_addr()` from `<arpa/inet.h>` vs Watt-32's `resolve()` -- use `resolve()`
  for IP resolution as it returns the correct format for `tcp_open()`
- `tcp_Socket` is opaque in Watt-32 -- no `.state` member; use `sockstate()` or
  `tcp_simple_state()` instead
- `net_init()` (calls `sock_init()`) must be called before `net_connect()`
- DOSBox-X NE2000 IRQ must be 10 (not 3, which conflicts with serial2)

**Configuration that works:**
- DOSBox-X: NE2000 + SLIRP backend, nicirq=10, nicbase=300
- WATTCP.CFG: static IP 10.0.2.15, gateway 10.0.2.2
- NE2000.COM loaded as: `NE2000.COM 0x60 10 0x300`
- Server bound to 0.0.0.0:8086 (reachable from SLIRP at 10.0.2.2:8086)

### 2026-03-06 -- Pi Server Implemented (v0.2.0)

**Pi server fully implemented and tested on Windows.** All server-side components
are working end-to-end: Playwright screenshot capture, image pipeline (dither, LUT,
tile, delta, RLE), protocol encoding, TCP server, interactive element detection,
event injection (mouse, keyboard, scroll, text, navigation).

**Files created:**
- `pi_server/palette.py` -- Fixed palette + 3D LUT
- `pi_server/image_pipeline.py` -- Dither, tile, delta, RLE pipeline
- `pi_server/protocol.py` -- Binary protocol encode/decode
- `pi_server/interaction_detector.py` -- Page element detection via JS
- `pi_server/session.py` -- Browser session management
- `pi_server/server.py` -- Asyncio TCP server
- `pi_server/config.py` -- Configuration with defaults
- `pi_server/test_pipeline.py` -- Visual quality verification
- `pi_server/test_client.py` -- Simulated DOS client for e2e testing

**Verified test results (Windows, localhost):**
- 256-color Google.com rendering: fully readable, visually clean
- 16-color fallback: functional, rougher dithering as expected
- RLE round-trip: lossless across all test cases
- Full frame (1160 tiles): ~247KB compressed, ~689ms estimated NE2000 transfer
- Click delta (129 tiles): ~19KB, fast
- Protocol encode/decode: verified lossless
- Scroll, click, navigation all produce correct delta updates
- Wikipedia: 262 interactive elements detected correctly

**No design changes required** -- implementation matched the design document.

### 2026-03-06 -- Initial Design (v0.1.0)

- Created initial design document
- Architecture: Pi 4 as rendering proxy, DOS thin client over Ethernet
- DOS client: C with DJGPP, Watt-32 TCP/IP, VESA graphics
- Pi server: Python with Playwright (headless Chromium), NumPy image pipeline
- Protocol: Custom binary over TCP, little-endian, 8-byte fixed header
- Display: 256-color ordered dithering with fixed palette and precomputed 3D LUT
- Compression: 16x16 tile grid, XOR delta + byte RLE
- Interactive elements: Hybrid approach (local editing for standard forms, keystroke forwarding for rich editors)
- Defined V1 scope (full browser functionality) and future enhancement backlog
- Target hardware: 25MHz 486, 4MB RAM, NE2000 Ethernet, VGA/VESA display

**Key design decisions made:**
1. DJGPP over Watcom C (better open-source tooling, 32-bit flat model)
2. Watt-32 over mTCP (BSD socket API, non-blocking I/O)
3. Fixed palette + LUT over adaptive quantization (enables fast delta compression)
4. Ordered dithering over error diffusion (more consistent for delta encoding, "retro" aesthetic)
5. Tile-based delta over full-frame streaming (critical for NE2000 bandwidth limits)
6. Local text editing over full keystroke forwarding (zero-latency typing)
7. uclock() polling over PIT reprogramming (simpler, safer, sufficient)
8. Software cursor over hardware cursor (required for VESA modes)
9. CDP Screencast + MutationObserver over periodic polling (efficient change detection)
10. Python + NumPy over pure Python (10-50x faster image processing)
