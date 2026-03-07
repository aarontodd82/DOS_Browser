# RetroSurf - Claude Development Guide

## What This Project Is

RetroSurf is a web browser for DOS 6.22 that uses a rendering proxy (Raspberry Pi 4 or Windows PC). The proxy runs real Chrome (headed, not headless) via Playwright, captures screenshots, compresses them to 256-color RLE tiles, and streams them over TCP to a DOS thin client. The DOS client displays the frames, handles mouse/keyboard input, and sends interactions back to the server.

Target hardware: 25MHz 486, 4MB RAM, NE2000 Ethernet, VGA/VESA display.

Historical design document: `DESIGN.md` — many sections are outdated. This file (CLAUDE.md) is the current source of truth.

## Project Layout

```
C:\Users\aaron\OneDrive\Documents\DOS_Browser\
├── DESIGN.md                    # Historical design doc (outdated in many areas)
├── CLAUDE.md                    # This file - current source of truth
│
├── pi_server/                   # Python rendering server
│   ├── server.py                # Asyncio TCP server, ACK-paced push/receive loops
│   ├── session.py               # BrowserSession (Playwright, stealth, CSS injection)
│   ├── image_pipeline.py        # Bayer dither, LUT, tile change detection, RLE
│   ├── palette.py               # 256/16 color palettes, 3D LUT builder
│   ├── protocol.py              # Binary protocol encode/decode (THE reference)
│   ├── interaction_detector.py  # JS-based interactive element detection
│   ├── config.py                # Config loader with defaults + Chromium args
│   ├── test_pipeline.py         # Visual quality tests
│   ├── test_client.py           # Simulated DOS client for e2e testing
│   ├── requirements.txt         # playwright, Pillow, numpy
│   └── setup.bat                # Windows venv setup
│
├── dos_client/                  # C DOS client (DJGPP cross-compiled)
│   ├── src/
│   │   ├── main.c               # Full browser main loop (splash, connect, render, input)
│   │   ├── protocol.h/c         # C port of protocol.py (packed structs, encode/decode)
│   │   ├── network.h/c          # Watt-32 TCP (connect, send, non-blocking recv, 48KB rx buf)
│   │   ├── video.h/c            # VESA/VGA mode setting, LFB/banked, backbuffer, palette
│   │   ├── render.h/c           # RLE decompress, tile blit to backbuffer (NO XOR)
│   │   ├── font.h/c             # BIOS ROM fonts (8x8, 8x14, 8x16), string drawing
│   │   ├── config.h/c           # RETROSURF.CFG parser (server_ip, video_mode, etc.)
│   │   ├── input.h/c            # INT 33h mouse, keyboard polling, 30Hz throttle
│   │   ├── cursor.h/c           # Software cursor (save-under/draw/restore, 4 shapes)
│   │   ├── chrome.h/c           # Address bar, nav buttons [<][>][R][X], status bar
│   │   └── interact.h/c         # Interaction map parsing, hit testing, cursor shapes
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

1. Start the server: `cd pi_server && .\venv\Scripts\activate && python server.py`
   - A Chrome window will open — minimize it, don't close it
2. Launch DOSBox-X: `cd dos_client && run.bat`
3. At the DOS prompt: `RETRO.EXE`

DOSBox-X SLIRP networking: DOS guest at 10.0.2.15, host at 10.0.2.2.
The client connects to 10.0.2.2:8086 which routes to localhost:8086.

## Architecture Overview

### Frame Streaming (No XOR)

Tiles are **raw palette-indexed pixels**, NOT XOR-encoded. XOR delta encoding was
removed because any client/server state desync caused permanent visual corruption.

The server still tracks `prev_indexed` for **change detection** (only send tiles that
actually changed), but the tile data sent is raw RLE-compressed pixels. The client
simply decompresses and blits — no prev_tiles buffer, no state to sync.

### ACK-Paced Push Loop (server.py)

The server has two concurrent async loops:
- **push_loop**: Captures frames and sends them, gated by client ACKs
- **receive_loop**: Processes client input (mouse, keyboard, navigation)

Push loop has two modes:
- **ACTIVE**: Recent frames had tile changes. Capture immediately after each ACK
  with zero delay. Maximizes throughput during page loads.
- **IDLE**: 3+ consecutive frames with no tile changes. Polls for dirty state
  with 10ms intervals to avoid wasting CPU on unnecessary screenshots.

### ACK Pipelining (client main.c)

The client sends FRAME_ACK **immediately after receiving** the frame data, BEFORE
flushing to VGA. This lets the server start capturing the next frame while the
client renders the current one to VGA (overlapping capture with rendering).

### TCP Throughput (client network.c)

- 48KB receive buffer via `sock_setbuf()` — increases TCP window so server can
  send a full frame without mid-transfer ACK stalls
- `tcp.recv_win = 49152` also set in WATTCP.CFG as belt-and-suspenders
- Progress-based receive retry: retries up to 50 times with no new data, but
  resets counter whenever bytes actually arrive (handles 10Mbps links gracefully)

### Browser Stealth (session.py)

Sites like Reddit and Google detect headless Playwright. We counter with:
- **Headed real Chrome** (`channel='chrome'`, `headless=False`) — not headless at all
- **Realistic User-Agent** (Chrome 131 on Windows 10)
- **navigator.webdriver removed** via init script
- **Fake plugins/languages/chrome object** injected
- **`--disable-blink-features=AutomationControlled`** Chromium arg
- **`--force-device-scale-factor=1`** — prevents Windows DPI scaling from breaking clicks
- **`--disable-renderer-backgrounding`** — prevents throttling when Chrome is minimized
- Fallback: if real Chrome isn't installed, uses Playwright's bundled Chromium (headed)
- On Pi without monitor: use Xvfb virtual framebuffer (`Xvfb :99 -screen 0 640x480x24 &`)

### Browser Optimizations (session.py)

CSS injected into every page:
- `animation-duration: 0s` / `transition-duration: 0s` — everything appears instantly
- `backdrop-filter: none` — kills frosted glass/blur effects
- Video/audio/YouTube iframes hidden (can't play on DOS)
- Scrollbar hidden (reduces dirty tiles during scroll)

Playwright context options:
- `reduced_motion='reduce'` — sites disable their own animations
- `color_scheme='light'` — forces light mode (dithers better to 256 colors)
- `--autoplay-policy=user-gesture-required` — no autoplaying media

## Protocol Reference

The protocol is defined in `pi_server/protocol.py` (Python, the canonical version)
and mirrored in `dos_client/src/protocol.h` (C, the client version).

- 8-byte header: msg_type(u8), flags(u8), payload_len(u16), sequence(u16), reserved(i16)
- All little-endian (native on x86, Python uses struct pack '<')
- TCP port 8086
- Key message types: CLIENT_HELLO(0x01), SERVER_HELLO(0x81), PALETTE(0x82),
  FRAME_DELTA(0x84), INTERACTION_MAP(0x85), MOUSE_EVENT(0x10), KEY_EVENT(0x11),
  SCROLL_EVENT(0x12), NAVIGATE(0x14), STATUS(0x87), ACK(0x20)
- FRAME_FULL and FRAME_DELTA are handled identically by the client (both are raw tiles)
- Large frames split with FLAG_CONTINUED; ACK sent after last chunk
- header.reserved carries scroll_dy for scroll optimization

## What's Complete

- **Server**: Asyncio TCP server with ACK-paced active/idle push loop
- **Session**: Playwright browser management with stealth + CSS optimizations
- **Pipeline**: Bayer dither, palette LUT, tile change detection, RLE compression (no XOR)
- **Protocol**: Fully defined and implemented on both sides
- **DOS dev environment**: DJGPP + Watt-32 + DOSBox-X all working
- **Video/Fonts/Config**: VESA mode detection, LFB+banked, palette, BIOS fonts, config file
- **Network Rendering**: RLE decompression, raw tile blit, 48KB TCP buffer, ACK pipelining
- **Mouse/Cursor/Input**: INT 33h mouse, software cursor, click/scroll/keyboard events
- **Browser Chrome**: Nav buttons, editable URL bar, status bar, correct palette colors
- **Interaction Map**: Forwarding mode, cursor shapes (hand/I-beam), hit testing

## What's Next

1. **Polish** - Error handling, reconnect on disconnect, keepalive, double-click

## Future Roadmap

### High Priority
- **Copy/paste** - Clipboard sync between DOS and server
- **File uploads** - DOS file -> server -> browser upload dialog
- **File downloads** - Browser download -> server -> DOS file
- **Right-click context menus** - Send right-click to server, render result
- **YouTube mode** - Low-res video/audio streaming to DOS (Sound Blaster audio)

### Medium Priority
- **Multiple tabs** - Tab bar UI, memory management
- **Bookmarks** - Local file on DOS machine
- **Browsing history** - Local file on DOS machine
- **Find on page (Ctrl+F)** - Server-side search + highlight
- **Keyboard shortcuts** - Configurable via RETRO.CFG
- **Reader mode** - Strip page to text content, render at DOS-native text resolution

### Low Priority / Experimental
- **Drag and drop** - Complex coordinate tracking
- **Print** - Server renders to printable format
- **Image zoom** - Click to zoom into a region at higher resolution
- **Tile priority** - Send tiles near the cursor first for perceived responsiveness
- **Adaptive JPEG quality** - Lower quality during fast scrolling, higher when static
- **Sound alerts** - PC speaker beep for notifications/errors
- **Theme support** - Multiple DOS-side color schemes (amber, green, blue terminal)
- **Server web admin panel** - Configure server settings from any browser

## Palette (IMPORTANT)

The server palette is a 6x6x6 RGB color cube (indices 0-215), NOT CGA colors.
- Index = R*36 + G*6 + B, where R,G,B are 0-5 (mapping to 0,51,102,153,204,255)
- **Black** = index 0 (0,0,0)
- **White** = index 215 (255,255,255)
- **Light gray** = index 172 (204,204,204) -- computed as 4*36+4*6+4
- Indices 216-239: grayscale ramp. 240-249: web accents. 250-254: reserved chrome colors.
- Do NOT use CGA indices (7, 8, 15) for chrome UI - they map to blues/teals in this palette.

## CLIENT_HELLO Quirk

The CLIENT_HELLO sends `vc->height` (480) as `screen_height` and `chrome_height` (24) as the top bar height.
The server computes: `viewport = 480 - 24 = 456`. Do NOT change this without careful testing.

## Forwarding Mode (not local editing)

DESIGN.md described local text editing (DOS renders text fields locally).
This was attempted and **abandoned** because:
1. Server frame updates overwrite locally-drawn text (constant fighting/flicker)
2. Server's bg_color mapping often returns wrong palette indices (fields turn black)
3. Complexity wasn't worth it for the marginal latency improvement

Instead, **forwarding mode** is used for ALL editable elements (text inputs, textareas,
passwords, contenteditable). Keys are sent to the server as KEY_EVENT, and the visual
result comes back via frame data (~50-200ms latency). This is reliable and works everywhere.

**Playwright caret**: Must use `caret='initial'` in `page.screenshot()` -- Playwright hides
the text cursor by default. Also added `__retrosurf_input_focused` JS flag so `check_dirty()`
returns true while an input has focus (forces periodic frame captures for cursor blink).

## Performance Reference

### Network Bandwidth (NE2000 on 25MHz 486: ~300-400 KB/s)

| Scenario | Compressed Size | Time @ 350 KB/s |
|----------|----------------|-----------------|
| Full frame (all tiles) | ~150-250 KB | 0.4-0.7s |
| Scroll (40% tiles) | ~60-100 KB | 170-285ms |
| Small interaction (10%) | ~15-25 KB | 43-71ms |
| Typing feedback (5%) | ~8-13 KB | 23-37ms |
| Hover (2-3 tiles) | ~1-2 KB | 3-6ms |

### Memory Budget (640x480 mode, ~3200 KB usable)

| Allocation | Size (KB) |
|------------|-----------|
| Program code + Watt-32 | ~200 |
| Backbuffer (640x480x8) | 307 |
| TCP receive buffer | 48 |
| Network recv_buf | 65 |
| Payload buffer | 65 |
| Interaction map | 20 |
| Fonts, cursor, config, misc | ~50 |
| **Total used** | **~755** |
| **Remaining** | **~2445** |

### End-to-End Latency (user action -> visual update on DOS)
- Best case (small change): ~150ms
- Typical: ~200-300ms
- Full page load: ~700-1000ms

## Important Design Decisions

- **No XOR delta encoding**: Removed because any state desync caused permanent corruption.
  Raw tiles with RLE compression are simple, robust, and only ~2-3% more bandwidth.
- **ACK before VGA flush**: Client sends ACK immediately after receiving frame, before the
  slow VGA write. Overlaps server capture with client rendering for ~2x throughput.
- **Active/idle streaming**: No artificial delays between frames when content is changing.
  Only polls for dirty state after 3 consecutive empty frames.
- **Pipeline reset on navigation**: Server resets `pipeline.prev_indexed = None` on
  navigate/back/forward/reload so all tiles are sent for the new page.

## Important Notes

- The user is NOT a coding expert. Automate everything possible. Minimize manual steps.
- Do NOT make design decisions without asking first.
- **Always restart the server** after changing session.py or server.py. Old Python processes keep stale code.
- The server binds to 0.0.0.0:8086 (important for SLIRP to reach it).
- CWSDPMI.EXE must always be in the same directory as the compiled .EXE.
- pi_server/protocol.py is the canonical protocol definition. dos_client/src/protocol.h must match it exactly.
- Watt-32 API: `sock_init()`, `tcp_open()`, `sock_write()`, `sock_fastread()`, `sock_dataready()`, `tcp_tick()`, `sock_established()`, `sock_setbuf()`.
- Do NOT use `sock_wait_established()` -- it's unreliable under SLIRP. Use manual polling with `tcp_tick()` + `sock_established()` instead.
- `tcp_Socket` is opaque -- no `.state` member. Use `sockstate()` or `tcp_simple_state()`.
- Use `resolve()` for IP addresses, not `inet_addr()`.
- The Makefile uses `$(dir $(abspath $(lastword $(MAKEFILE_LIST))))` to auto-detect paths.
- gnumake.exe is at `tools/watt32/util/win32/gnumake.exe` (bundled with Watt-32, not a system install).
- Git repo initialized. Commit after each working milestone.
