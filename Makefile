# BCM-ex - A BWT-based file compressor
#
# Targets:
#   make                  build bcm in the current directory
#   make check            round-trip and compatibility tests
#   make windows          bcm.exe for Windows x64 with mingw-w64
#   make windows32        bcm.exe for Windows x86 with mingw-w64
#   make clean
#
# Variables: CXX, CC, CXXFLAGS, CFLAGS, LDFLAGS, OUT, BUILD, LIBSAIS_DIR

PROG  = bcm
OUT  ?= $(PROG)
SRCDIR = src
LIBSAIS_DIR ?= third_party/libsais
BUILD = build

CC  ?= cc
CXX ?= c++

# -O2 and the warnings that matter for a compressor written in C++11.
# bcm.cpp is deliberately C++11: it must build on the older toolchains that
# ship with the oldest supported systems.
CXXFLAGS ?= -O2
CXXFLAGS += -std=c++11 -Wall -Wextra -Wno-unused-parameter
CFLAGS   ?= -O2
LDFLAGS  ?=
LIBS     ?=

# libsais is C; it is compiled once per build and linked statically, so the
# resulting binary has no dependencies beyond the C/C++ runtime.
LIBSAIS_SRC = $(LIBSAIS_DIR)/libsais.c
LIBSAIS_HDR = $(LIBSAIS_DIR)/libsais.h

OBJS = $(BUILD)/bcm.o $(BUILD)/libsais.o

.PHONY: all check clean windows windows32

all: $(OUT)

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/libsais.o: $(LIBSAIS_SRC) $(LIBSAIS_HDR) | $(BUILD)
	$(CC) $(CFLAGS) -I$(LIBSAIS_DIR) -c -o $@ $(LIBSAIS_SRC)

$(BUILD)/bcm.o: $(SRCDIR)/bcm.cpp $(SRCDIR)/platform.h $(LIBSAIS_HDR) | $(BUILD)
	$(CXX) $(CXXFLAGS) -I$(LIBSAIS_DIR) -c -o $@ $(SRCDIR)/bcm.cpp

$(OUT): $(OBJS)
	$(CXX) $(CXXFLAGS) $(OBJS) -o $@ $(LDFLAGS) $(LIBS)

# Windows x64 and x86: statically linked, so the .exe needs no DLL and runs on
# Windows 7 SP1 or newer.
WINCC  ?= x86_64-w64-mingw32-gcc
WINCXX ?= x86_64-w64-mingw32-g++
WINCC32  ?= i686-w64-mingw32-gcc
WINCXX32 ?= i686-w64-mingw32-g++

windows:
	$(MAKE) OUT=$(PROG).exe CC=$(WINCC) CXX=$(WINCXX) \
		CFLAGS="$(CFLAGS) -D_WIN32_WINNT=0x0601" \
		LDFLAGS="-static -static-libgcc -static-libstdc++ -s" \
		BUILD=build/win64

windows32:
	$(MAKE) OUT=$(PROG).exe CC=$(WINCC32) CXX=$(WINCXX32) \
		CFLAGS="$(CFLAGS) -D_WIN32_WINNT=0x0601" \
		LDFLAGS="-static -static-libgcc -static-libstdc++ -s" \
		BUILD=build/win32

check: $(OUT)
	sh tests/run-tests.sh ./$(OUT)
	sh tests/check-libsais.sh
	sh tests/check-compat.sh ./$(OUT)

clean:
	rm -rf $(BUILD) $(PROG) $(PROG).exe
