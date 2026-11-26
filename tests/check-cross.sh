#!/bin/sh
#
# Cross-build check: build bcm for every target this host can cross-compile,
# and verify that all of them write exactly the same bytes and can read each
# other's output.
#
#   sh tests/check-cross.sh
#
# Skips (without failing) the targets whose toolchain is not installed, so it
# works on a developer machine and in CI alike.

set -u

ROOT=$(cd "$(dirname "$0")/.." && pwd)
TMP=${TMPDIR:-/tmp}/bcm-ex-cross-$$
FAILS=0
CHECKS=0
BUILT=0

mkdir -p "$TMP" || exit 1
trap 'rm -rf "$TMP"' EXIT INT TERM

. "$(dirname "$0")/lib.sh"

bad() { echo "FAIL: $*"; FAILS=$((FAILS + 1)); }
ok() { CHECKS=$((CHECKS + 1)); }

have() { bcm_have "$1"; }

# The reference stream: produced here, and every other build must match it.
REF="$TMP/ref.bcm"
SAMPLE="$TMP/sample.bin"

{
    printf 'bcm-ex cross build check\n'
    awk 'BEGIN { for (i = 0; i < 3000; i++) printf "the quick brown fox jumps over the lazy dog " }'
    awk 'BEGIN { for (i = 0; i < 2000; i++) printf "%c", i % 256 }'
} > "$SAMPLE"

echo "=== native build ==="
(cd "$ROOT" && make clean >/dev/null 2>&1 && make >/dev/null 2>&1) || {
    echo "error: the native build failed" >&2
    exit 2
}

"$ROOT/bcm" -b1 -f "$SAMPLE" "$REF" >/dev/null 2>&1
check_exit=$?
if [ "$check_exit" -ne 0 ] || [ ! -s "$REF" ]; then
    echo "error: the native build could not create the reference stream" >&2
    exit 2
fi

REF_MD5=$(bcm_md5 "$REF")
echo "reference: $REF_MD5 ($(wc -c < "$REF" | tr -d ' ') bytes)"

# compare NAME RUNNER BINARY
compare() {
    name=$1
    runner=$2
    bin=$3

    echo "=== $name ==="

    # The runner is either empty (native) or a qemu/wine command.
    # shellcheck disable=SC2086
    $runner "$bin" -b1 -f "$SAMPLE" "$TMP/$name.bcm" >/dev/null 2>&1
    if [ $? -ne 0 ]; then
        bad "$name: compression failed"
        return
    fi

    md5=$(bcm_md5 "$TMP/$name.bcm")
    if [ "$md5" = "$REF_MD5" ]; then ok; else
        bad "$name: wrote $md5, native wrote $REF_MD5"
    fi

    # It must also decode the native stream.
    # shellcheck disable=SC2086
    $runner "$bin" -d -f "$REF" "$TMP/$name.out" >/dev/null 2>&1
    if [ $? -ne 0 ]; then
        bad "$name: could not decode the native stream"
        return
    fi

    if bcm_same "$SAMPLE" "$TMP/$name.out"; then ok; else
        bad "$name: decoded the native stream incorrectly"
    fi
}

# --- Windows -------------------------------------------------------------

if have x86_64-w64-mingw32-g++ && have x86_64-w64-mingw32-gcc; then
    (cd "$ROOT" && make CC=x86_64-w64-mingw32-gcc CXX=x86_64-w64-mingw32-g++ \
        OUT=bcm-win64.exe BUILD=build/win64 \
        LDFLAGS="-static -static-libgcc -static-libstdc++ -s" >/dev/null 2>&1)
    if [ -f "$ROOT/bcm-win64.exe" ]; then
        BUILT=$((BUILT + 1))
        if have wine; then
            compare windows-x64 "wine" "$ROOT/bcm-win64.exe"
        else
            echo "=== windows-x64 === (built; wine not installed, not run)"
        fi
    else
        bad "windows-x64: build failed"
    fi
else
    echo "=== windows-x64 === (skipped, no mingw-w64 x86_64)"
fi

if have i686-w64-mingw32-g++ && have i686-w64-mingw32-gcc; then
    (cd "$ROOT" && make CC=i686-w64-mingw32-gcc CXX=i686-w64-mingw32-g++ \
        OUT=bcm-win32.exe BUILD=build/win32 \
        LDFLAGS="-static -static-libgcc -static-libstdc++ -s" >/dev/null 2>&1)
    if [ -f "$ROOT/bcm-win32.exe" ]; then
        BUILT=$((BUILT + 1))
        if have wine; then
            compare windows-x86 "wine" "$ROOT/bcm-win32.exe"
        else
            echo "=== windows-x86 === (built; wine not installed, not run)"
        fi
    else
        bad "windows-x86: build failed"
    fi
else
    echo "=== windows-x86 === (skipped, no mingw-w64 i686)"
fi

# --- Linux i386 ----------------------------------------------------------

if have gcc && echo 'int main(void){return 0;}' | gcc -m32 -x c - -o "$TMP/m32" 2>/dev/null; then
    (cd "$ROOT" && make CC="gcc -m32" CXX="g++ -m32" OUT=bcm-i386 \
        BUILD=build/i386 >/dev/null 2>&1)
    if [ -f "$ROOT/bcm-i386" ]; then
        BUILT=$((BUILT + 1))
        compare linux-i386 "" "$ROOT/bcm-i386"
    else
        bad "linux-i386: build failed"
    fi
else
    echo "=== linux-i386 === (skipped, no 32-bit toolchain)"
fi

# --- Linux arm64 ---------------------------------------------------------

if have aarch64-linux-gnu-g++ && have aarch64-linux-gnu-gcc; then
    (cd "$ROOT" && make CC=aarch64-linux-gnu-gcc CXX=aarch64-linux-gnu-g++ \
        OUT=bcm-arm64 BUILD=build/arm64 LDFLAGS="-static -s" >/dev/null 2>&1)
    if [ -f "$ROOT/bcm-arm64" ]; then
        BUILT=$((BUILT + 1))
        if have qemu-aarch64; then
            compare linux-arm64 "qemu-aarch64" "$ROOT/bcm-arm64"
        elif have qemu-aarch64-static; then
            compare linux-arm64 "qemu-aarch64-static" "$ROOT/bcm-arm64"
        else
            echo "=== linux-arm64 === (built; qemu not installed, not run)"
        fi
    else
        bad "linux-arm64: build failed"
    fi
else
    echo "=== linux-arm64 === (skipped, no aarch64 toolchain)"
fi

# --- macOS ---------------------------------------------------------------
# Not cross-compilable without an SDK; CI builds and tests it natively.

echo "=== macos-arm64 === (built and tested natively in CI)"

# ---------------------------------------------------------------------------

if [ "$FAILS" -ne 0 ]; then
    echo "$FAILS checks FAILED"
    exit 1
fi

echo "all $CHECKS cross-build checks passed ($BUILT binaries built)"
exit 0
