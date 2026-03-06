# RetroSurf DOS Client - Development Setup

## What's Already Done

Everything is set up and ready to use. No additional installation needed.

**Tools (in `../tools/`):**
- `djgpp/` - DJGPP cross-compiler (GCC 12.2.0, Windows native)
- `watt32/` - Watt-32 TCP/IP library (already compiled for DJGPP)
- `dosbox-x/` - DOSBox-X emulator (portable, no install)

**Build output (in `build/`):**
- `RETRO.EXE` - The DOS client (already compiled)
- `CWSDPMI.EXE` - DPMI host (required to run 32-bit DOS programs)
- `WATTCP.CFG` - TCP/IP configuration for SLIRP networking

**Drivers (in `drivers/`):**
- `NE2000.COM` - Crynwr NE2000 packet driver

## How to Build

Double-click `build.bat` or run from command prompt:
```
build.bat
```

This compiles all `.c` files in `src/` and produces `build/RETRO.EXE`.

## How to Test

### 1. Start the Pi server (in one command prompt window)
```
cd ..\pi_server
python server.py
```

### 2. Launch DOSBox-X (in another window or double-click)
```
run.bat
```

DOSBox-X will boot, mount the build directory as C:, load the NE2000
packet driver, and give you a DOS prompt.

### 3. Run the client
At the DOS prompt:
```
RETRO.EXE
```

It will connect to the server at 10.0.2.2:8086 (DOSBox-X SLIRP routes
this to localhost on the Windows host).

## Development Cycle

1. Edit `.c` / `.h` files in `src/` with any text editor
2. Run `build.bat`
3. In DOSBox-X, type `RETRO.EXE` again (no need to restart DOSBox-X)

## How Networking Works

DOSBox-X emulates an NE2000 Ethernet card with SLIRP (software NAT):
- DOS guest IP: 10.0.2.15 (assigned by SLIRP DHCP)
- Host/gateway IP: 10.0.2.2 (this reaches your Windows machine)
- The Pi server listens on port 8086 on all interfaces (0.0.0.0)
- DOS connects to 10.0.2.2:8086 -> reaches localhost:8086

On real hardware, the DOS client connects to the Pi's IP address instead.

## Troubleshooting

**"sock_init() failed"** - Packet driver didn't load. Check that DOSBox-X
config has `ne2000=true` and `backend=slirp` in the `[ne2000]` section.

**"Connection failed"** - Server not running, or Windows Firewall blocking.
Start `python server.py` first. You may need to allow it through the firewall.

**"No DPMI" or crash on startup** - CWSDPMI.EXE must be in the same
directory as RETRO.EXE (both should be in `build/`).
