# BCM-ex - A BWT-based file compressor
#
# Targets:
#   make                  build bcm in the current directory
#   make check            round-trip and compatibility tests
#   make windows          bcm.exe for Windows x64 with mingw-w64
#   make windows32        bcm.exe for Windows x86 with mingw-w64
#   make clean
#
# Variables: CXX, CC, CXXFLAGS, CFLAGS, LDFLAGS, OUT, BUILD, LIBSAIS_DIR,
#            OPENMP (1/0; default: use it if the compiler has it)

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

# Threads (-j#) come from OpenMP, in libsais and in bcm.cpp. By default it is
# used when the compiler can build and link an OpenMP program with LDFLAGS
# (checked once with a test build, so a static build needs a static libgomp);
# OPENMP=0 builds without it, OPENMP=1 insists on it. Without OpenMP the
# program is the same, single-threaded, and -j is accepted and ignored.
ifeq ($(origin OPENMP),undefined)
OPENMP := $(shell t=$$(mktemp 2>/dev/null || echo omptest.$$$$); \
	printf '\043include <omp.h>\nint main(){return omp_get_num_procs()<1;}\n' | \
	$(CXX) -fopenmp -x c++ - -o "$$t" $(LDFLAGS) >/dev/null 2>&1 && r=1 || r=0; \
	rm -f "$$t" "$$t.exe"; echo $$r)
endif
# Kept apart from CFLAGS/CXXFLAGS so that the windows targets, which pass
# CFLAGS down, decide again with their own compiler.
ifeq ($(OPENMP),1)
OMP_CXXFLAGS = -fopenmp
OMP_CFLAGS   = -fopenmp -DLIBSAIS_OPENMP
endif

# libsais is C; it is compiled once per build and linked statically, so the
# resulting binary has no dependencies beyond the C/C++ runtime.
LIBSAIS_SRC = $(LIBSAIS_DIR)/libsais.c
LIBSAIS_HDR = $(LIBSAIS_DIR)/libsais.h

OBJS = $(BUILD)/bcm.o $(BUILD)/libsais.o

.PHONY: all check clean windows windows32 stage-tests

all: $(OUT)

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/libsais.o: $(LIBSAIS_SRC) $(LIBSAIS_HDR) | $(BUILD)
	$(CC) $(CFLAGS) $(OMP_CFLAGS) -I$(LIBSAIS_DIR) -c -o $@ $(LIBSAIS_SRC)

$(BUILD)/bcm.o: $(SRCDIR)/bcm.cpp $(SRCDIR)/platform.h $(SRCDIR)/x86f.h $(SRCDIR)/cm1.h $(SRCDIR)/cm2.h $(SRCDIR)/cm1_tables.h $(LIBSAIS_HDR) | $(BUILD)
	$(CXX) $(CXXFLAGS) $(OMP_CXXFLAGS) -I$(LIBSAIS_DIR) -c -o $@ $(SRCDIR)/bcm.cpp

$(OUT): $(OBJS)
	$(CXX) $(CXXFLAGS) $(OMP_CXXFLAGS) $(OBJS) -o $@ $(LDFLAGS) $(LIBS)

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

$(BUILD)/stage-tests: tests/stage-tests.cpp tests/../$(SRCDIR)/x86f.h tests/../$(SRCDIR)/lzp.h | $(BUILD)
	$(CXX) $(CXXFLAGS) -o $@ tests/stage-tests.cpp

stage-tests: $(BUILD)/stage-tests

check: $(OUT)
	sh tests/run-tests.sh ./$(OUT)
	sh tests/check-libsais.sh
	sh tests/check-compat.sh ./$(OUT)

clean:
	rm -rf $(BUILD) $(PROG) $(PROG).exe
