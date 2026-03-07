# RetroSurf DOS Client - Cross-compilation Makefile
# Uses DJGPP cross-compiler on Windows (no WSL needed)
#
# Usage: build.bat          (from Windows)
#        gnumake -f Makefile (if gnumake is on PATH)

# Project root (auto-detected from Makefile location)
PROJECT_ROOT = $(dir $(abspath $(lastword $(MAKEFILE_LIST))))
TOOLS_ROOT   = $(PROJECT_ROOT)../tools

# DJGPP cross-compiler (Windows native)
DJGPP_BIN  = $(TOOLS_ROOT)/djgpp/bin
CC         = $(DJGPP_BIN)/i586-pc-msdosdjgpp-gcc
AR         = $(DJGPP_BIN)/i586-pc-msdosdjgpp-ar
STRIP      = $(DJGPP_BIN)/i586-pc-msdosdjgpp-strip

# Watt-32 TCP/IP library
WATT32_ROOT = $(TOOLS_ROOT)/watt32
WATT32_INC  = $(WATT32_ROOT)/inc
WATT32_LIB  = $(WATT32_ROOT)/lib

# Compiler flags
CFLAGS  = -O2 -Wall -Wextra -Wno-unused-parameter
CFLAGS += -I$(WATT32_INC)
CFLAGS += -DRETROSURF_VERSION=\"0.1.0\"

# Linker flags - Watt-32 library
LDFLAGS = -L$(WATT32_LIB) -lwatt -lemu

# Source files
SRCS = $(wildcard src/*.c)
OBJS = $(SRCS:src/%.c=obj/%.o)

# Output
TARGET = build/RETRO.EXE

# ---- Targets ----

.PHONY: all clean

all: $(TARGET)
	@echo ""
	@echo "Build complete: $(TARGET)"

$(TARGET): $(OBJS) | build
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

obj/%.o: src/%.c | obj
	$(CC) $(CFLAGS) -c -o $@ $<

obj:
	mkdir -p obj

build:
	mkdir -p build

clean:
	rm -rf obj
	rm -f build/RETRO.EXE
	@echo "Cleaned."
