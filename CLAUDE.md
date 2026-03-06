# RetroSurf - Claude Development Guide

## What This Project Is

RetroSurf is a web browser for DOS 6.22 that uses a Raspberry Pi 4 as a rendering proxy. The Pi runs headless Chromium, captures screenshots, compresses them to 256-color tiled frames, and streams them over TCP to a DOS thin client. The DOS client displays the frames, handles mouse/keyboard input, and sends interactions back to the Pi.

Target hardware: 25MHz 486, 4MB RAM, NE2000 Ethernet, VGA/VESA display.

Full design: `DESIGN.md` (2200+ lines, covers everything).

## Project Layout

```
C:\Users\aaron\OneDrive\Documents\DOS_Browser\
├── DESIGN.md                    # Full design document (the source of truth)
├── CLAUDE.md                    # This file
│
├── pi_server/                   # COMPLETE - Python rendering server
│   ├── server.py                # Asyncio TCP server, push/receive loops
│   ├── session.py               # BrowserSession (Playwright page lifecycle)
│   ├── image_pipeline.py        # Bayer dither, LUT, tile, XOR delta, RLE
│   ├── palette.py               # 256/16 color palettes, 3D LUT builder
│   ├── protocol.py              # Binary protocol encode/decode (THE reference)
│   ├── interaction_detector.py  # JS-based interactive element detection
│   ├── config.py                # Config loader with defaults
│   ├── test_pipeline.py         # Visual quality tests
│   ├── test_client.py           # Simulated DOS client for e2e testing
│   ├── requirements.txt         # playwright, Pillow, numpy
│   └── setup.bat                # Windows venv setup
│
├── dos_client/                  # IN PROGRESS - C DOS client
│   ├── src/
│   │   ├── main.c               # Full browser main loop (splash, connect, render, input)
│   │   ├── protocol.h/c         # C port of protocol.py (packed structs, encode/decode)
│   │   ├── network.h/c          # Watt-32 TCP (connect, send, non-blocking recv)
│   │   ├── video.h/c            # VESA/VGA mode setting, LFB/banked, backbuffer, palette
│   │   ├── render.h/c           # RLE decompress, XOR delta, tile grid, blit to backbuffer
│   │   ├── font.h/c             # BIOS ROM fonts (8x8, 8x14, 8x16), string drawing
│   │   ├── config.h/c           # RETROSURF.CFG parser (server_ip, video_mode, etc.)
│   │   ├── input.h/c            # INT 33h mouse, keyboard polling, 30Hz throttle
│   │   ├── cursor.h/c           # Software cursor (save-under/draw/restore, 4 shapes)
│   │   ├── chrome.h/c           # Address bar, nav buttons [<][>][R][X], status bar
│   │   └── interact.h/c        # Interaction map parsing, hit testing, cursor shapes
│   ├── build/                   # Compiled output (DOSBox-X mounts this as C:)
│   │   ├── RETRO.EXE            # The compiled DOS executable
│   │   ├── CWSDPMI.EXE          # DPMI host (must be alongside .EXE)
│   │   └── WATTCP.CFG           # Watt-32 TCP config (SLIRP: host at 10.0.2.2)
│   ├── drivers/
│   │   └── NE2000.COM           # Crynwr NE2000 packet driver
│   ├── Makefile                 # Cross-compilation (auto-detects tool paths)
│   ├── build.bat                # One-click build from Windows
│   ├── run.bat                  # Launches DOSBox-X with our config
│   ├── dosbox-x.conf            # DOSBox-X config (NE2000+SLIRP, SVGA, automount)
│   └── SETUP.md                 # Setup instructions (mostly already done)
│
└── tools/                       # Development tools (all Windows-native)
    ├── djgpp/                   # DJGPP cross-compiler v3.4 (GCC 12.2.0)
    │   └── bin/
    │       └── i586-pc-msdosdjgpp-gcc.exe   # The compiler
    ├── watt32/                  # Watt-32 TCP/IP library
    │   ├── inc/                 # Headers (tcp.h, sys/socket.h, etc.)
    │   ├── lib/libwatt.a        # Compiled library (link with -lwatt)
    │   └── util/win32/
    │       └── gnumake.exe      # GNU Make for Windows (used by build.bat)
    └── dosbox-x/                # DOSBox-X portable
        └── mingw-build/mingw/
            └── dosbox-x.exe     # The emulator
```

## How to Build

```
cd dos_client
build.bat
```

This calls `gnumake.exe` which invokes `i586-pc-msdosdjgpp-gcc.exe` to compile
all `src/*.c` files into `build/RETRO.EXE`. All paths are auto-detected from
the Makefile relative to the project root.

## How to Test

1. Start the server: `cd pi_server && python server.py`
2. Launch DOSBox-X: `cd dos_client && run.bat`
3. At the DOS prompt: `RETRO.EXE`

DOSBox-X SLIRP networking: DOS guest at 10.0.2.15, host at 10.0.2.2.
The client connects to 10.0.2.2:8086 which routes to localhost:8086.

## Protocol Reference

The protocol is defined in `pi_server/protocol.py` (Python, the canonical version)
and mirrored in `dos_client/src/protocol.h` (C, the client version).

- 8-byte header: msg_type(u8), flags(u8), payload_len(u16), sequence(u16), reserved(i16)
- All little-endian (native on x86, Python uses struct pack '<')
- TCP port 8086
- Key message types: CLIENT_HELLO(0x01), SERVER_HELLO(0x81), PALETTE(0x82),
  FRAME_DELTA(0x84), INTERACTION_MAP(0x85), MOUSE_EVENT(0x10), KEY_EVENT(0x11),
  SCROLL_EVENT(0x12), NAVIGATE(0x14), STATUS(0x87)

## What's Done

- **Pi server**: 100% complete, tested on Windows (+ animation detection, input focus cursor blink)
- **Protocol**: Fully defined and implemented on both sides
- **DOS dev environment**: DJGPP + Watt-32 + DOSBox-X all working
- **Phase 1 - Video/Fonts/Config**: VESA mode detection, LFB+banked, palette, BIOS fonts, config file
- **Phase 2 - Network Rendering**: RLE decompression, XOR delta tiles, frame display
- **Phase 3 - Mouse/Cursor/Input**: INT 33h mouse, software cursor, click/scroll/keyboard events
- **Phase 4 - Browser Chrome**: Nav buttons, editable URL bar, status bar, correct palette colors
- **Phase 5 - Interaction Map**: Forwarding mode, cursor shapes (hand/I-beam), hit testing

## What's Next

1. **Phase 6 - Polish** - Error handling, reconnect, keepalive, CONTINUED flag, double-click

## Palette (IMPORTANT)

The server palette is a 6x6x6 RGB color cube (indices 0-215), NOT CGA colors.
- Index = R*36 + G*6 + B, where R,G,B are 0-5 (mapping to 0,51,102,153,204,255)
- **Black** = index 0 (0,0,0)
- **White** = index 215 (255,255,255)
- **Light gray** = index 172 (204,204,204) — computed as 4*36+4*6+4
- Indices 216-239: grayscale ramp. 240-249: web accents. 250-254: reserved chrome colors.
- Do NOT use CGA indices (7, 8, 15) for chrome UI - they map to blues/teals in this palette.

## CLIENT_HELLO Quirk

The CLIENT_HELLO sends `vc->height` (480) as `screen_height` and `chrome_height` (24) as the top bar height.
The server computes: `viewport = 480 - 24 = 456`. Do NOT change this without careful testing.

## Phase 5 Design Decision: Forwarding Mode (not local editing)

DESIGN.md Section 8.3 describes local text editing (DOS renders text fields locally for zero-latency typing).
This was attempted and **abandoned** because:
1. Server frame deltas overwrite locally-drawn text (constant fighting/flicker)
2. Server's bg_color mapping often returns wrong palette indices (fields turn black)
3. Complexity wasn't worth it for the marginal latency improvement

Instead, **forwarding mode** is used for ALL editable elements (text inputs, textareas, passwords,
contenteditable). Keys are sent to the server as KEY_EVENT, and the visual result comes back via
frame deltas (~50-200ms latency). This is reliable and works everywhere.

**Playwright caret**: Must use `caret='initial'` in `page.screenshot()` — Playwright hides the text
cursor by default. Also added `__retrosurf_input_focused` JS flag so `check_dirty()` returns true
while an input has focus (forces periodic frame captures for cursor blink).

## Important Notes

- The user is NOT a coding expert. Automate everything possible. Minimize manual steps.
- Do NOT make design decisions without asking first.
- **Always restart the server** after changing session.py or server.py. Old Python processes keep stale code.
- The server binds to 0.0.0.0:8086 (important for SLIRP to reach it).
- CWSDPMI.EXE must always be in the same directory as the compiled .EXE.
- pi_server/protocol.py is the canonical protocol definition. dos_client/src/protocol.h must match it exactly.
- Watt-32 API: `sock_init()`, `tcp_open()`, `sock_write()`, `sock_fastread()`, `sock_dataready()`, `tcp_tick()`, `sock_established()`.
- Do NOT use `sock_wait_established()` -- it's unreliable under SLIRP. Use manual polling with `tcp_tick()` + `sock_established()` instead.
- `tcp_Socket` is opaque -- no `.state` member. Use `sockstate()` or `tcp_simple_state()`.
- Use `resolve()` for IP addresses, not `inet_addr()`.
- The Makefile uses `$(dir $(abspath $(lastword $(MAKEFILE_LIST))))` to auto-detect paths.
- gnumake.exe is at `tools/watt32/util/win32/gnumake.exe` (bundled with Watt-32, not a system install).
- Git repo initialized. Commit after each working milestone.
