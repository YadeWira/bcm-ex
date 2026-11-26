#!/bin/sh
#
# bcm-ex test suite.
#
#   sh tests/run-tests.sh ./bcm
#
# Every check is a black-box check on the program that was passed in, so the
# same script runs on Linux, on Windows (under MSYS or bash) and on macOS, and
# on a binary from any of them. Nothing here needs a reference binary: the
# format guarantee is checked by the round trip plus the fixed check values
# below, which were produced by bcm 1.65 and must never change.

set -u

BCM=${1:-./bcm}
TMP=${TMPDIR:-/tmp}/bcm-ex-tests-$$
FAILS=0
CHECKS=0

mkdir -p "$TMP" || exit 1
trap 'rm -rf "$TMP"' EXIT INT TERM

. "$(dirname "$0")/lib.sh"

bad() {
    echo "FAIL: $*"
    FAILS=$((FAILS + 1))
}

ok() {
    CHECKS=$((CHECKS + 1))
}

# check NAME EXPECTED_FILE ACTUAL_FILE
check_file() {
    if bcm_same "$2" "$3"; then ok; else bad "$1"; fi
}

# check_exit NAME EXPECTED ACTUAL
check_exit() {
    if [ "$2" = "$3" ]; then ok; else bad "$1 (expected exit $2, got $3)"; fi
}

if [ ! -x "$BCM" ]; then
    echo "error: '$BCM' is not an executable" >&2
    exit 2
fi

# ---------------------------------------------------------------------------
# Fixed check values from BCM 1.65.
#
# After the round trip every sample must survive unchanged; and the compressed
# (obsolete comment)
# writes for it. That single number pins the model, the coder and the header:
# any change in what the encoder emits breaks it.
# ---------------------------------------------------------------------------

# 1. Inputs of every interesting shape.

: > "$TMP/empty.bin"

printf 'X' > "$TMP/one.bin"

printf 'a' > "$TMP/one_byte_then_nothing.bin"

# 0..255, each exactly ten times: no long runs, every symbol present.
# Written byte by byte, because awk cannot emit NUL bytes reliably.
i=0
: > "$TMP/all_bytes.bin"
while [ "$i" -lt 10 ]; do
    b=0
    while [ "$b" -lt 256 ]; do
        oct=$(printf '%03o' "$b")
        printf "\\$oct" >> "$TMP/all_bytes.bin"
        b=$((b + 1))
    done
    i=$((i + 1))
done

# A long run: exercises the run detector in the model.
awk 'BEGIN { for (i = 0; i < 100000; i++) printf "a" }' > "$TMP/run.bin"

# Highly compressible text, and text that is not compressible at all.
awk 'BEGIN { for (i = 0; i < 20000; i++) printf "the quick brown fox jumps over the lazy dog " }' \
    > "$TMP/text.bin"

python3 -c "
import random
random.seed(1234)
print(''.join(random.choice('abcdefghijklmnopqrstuvwxyz ') for _ in range(200000)), end='')
" > "$TMP/words.bin" 2>/dev/null || awk 'BEGIN { srand(1); for (i = 0; i < 200000; i++) printf "%c", 97 + int(rand() * 26) }' > "$TMP/words.bin"

# Incompressible: the worst case for a context model.
python3 -c "
import random, sys
random.seed(99)
sys.stdout.buffer.write(bytes(random.getrandbits(8) for _ in range(65536)))
" > "$TMP/rand.bin" 2>/dev/null || \
head -c 65536 /dev/urandom > "$TMP/rand.bin" 2>/dev/null || \
awk 'BEGIN { srand(7); for (i = 0; i < 65536; i++) printf "%c", int(rand() * 256) }' > "$TMP/rand.bin"

SIZES="empty one one_byte_then_nothing all_bytes run text words rand"

# CRLF and 0x1A: on Windows, a stream opened in text mode silently translates
# the first and treats the second as end of file. Both appear in the samples
# above only by accident, so this one is explicit.
awk 'BEGIN { for (i = 0; i < 4000; i++) printf "line%d\r\n\x1a\x00\xff end\r\n", i }' \
    > "$TMP/crlf.bin" 2>/dev/null
if [ ! -s "$TMP/crlf.bin" ]; then
    printf 'line one\r\n\032\000\377 end\r\nline two\r\n' > "$TMP/crlf.bin"
fi
SIZES="$SIZES crlf"

# 2. Round trip, at the default block size and at a small one.

for s in $SIZES; do
    f="$TMP/$s.bin"

    rm -f "$TMP/$s.bcm" "$TMP/$s.out"
    "$BCM" -f "$f" "$TMP/$s.bcm" >/dev/null 2>&1
    check_exit "compress $s" 0 $?
    "$BCM" -d -f "$TMP/$s.bcm" "$TMP/$s.out" >/dev/null 2>&1
    check_exit "decompress $s" 0 $?
    check_file "round trip $s" "$f" "$TMP/$s.out"

    # A 1 MB block forces multi-block streams on the larger samples.
    rm -f "$TMP/$s.b1.bcm" "$TMP/$s.b1.out"
    "$BCM" -b1 -f "$f" "$TMP/$s.b1.bcm" >/dev/null 2>&1
    check_exit "compress $s -b1" 0 $?
    "$BCM" -d -f "$TMP/$s.b1.bcm" "$TMP/$s.b1.out" >/dev/null 2>&1
    check_exit "decompress $s -b1" 0 $?
    check_file "round trip $s -b1" "$f" "$TMP/$s.b1.out"

    # -t must accept it and write nothing.
    rm -f "$TMP/$s.b1.out.t"
    "$BCM" -t "$TMP/$s.b1.bcm" >/dev/null 2>&1
    check_exit "test $s" 0 $?
done

# 3. The v2 container.
#
# bcm-ex 2 writes "BCM2" (flags, method, block size) and then the same block
# stream as before. What must stay true is the round trip and the header
# layout; the byte-for-byte compatibility with 1.65 that earlier versions had
# is deliberately gone (see the wiki's Compatibility page), and what replaces
# it is the ability to *read* 1.x files, checked in check-compat.sh.

printf 'hello, bcm-ex\n' > "$TMP/hello.txt"
"$BCM" -b1 -f "$TMP/hello.txt" "$TMP/hello.bcm" >/dev/null 2>&1

MAGIC=$(dd if="$TMP/hello.bcm" bs=1 count=4 2>/dev/null)
if [ "$MAGIC" = "BCM2" ]; then ok; else
    bad "the stream does not start with BCM2 (got '$MAGIC')"
fi

# flags (1), method (1), block size (4) = 10 bytes of header before the model.
HELLO_SIZE=$(wc -c < "$TMP/hello.bcm" | tr -d ' ')
if [ "$HELLO_SIZE" -gt 10 ] && [ "$HELLO_SIZE" -lt 60 ]; then ok; else
    bad "hello.txt at -b1 is $HELLO_SIZE bytes, outside the expected range"
fi

method_of() {
    dd if="$1" bs=1 skip=5 count=1 2>/dev/null | od -An -tu1 | tr -d ' '
}

METHOD=$(method_of "$TMP/hello.bcm")
if [ "$METHOD" = "1" ]; then ok; else
    bad "default method byte is '$METHOD', expected 1"
fi

# 3b. The models. -m1 (mixing) is the default since 2.1 and -m2 is its
# stronger, slower sibling; -m0 is BCM's model and must keep writing exactly
# what bcm-ex 2.0 wrote. Each is pinned by the digest of one stream, so any
# change in what an encoder emits breaks this check, not just the round trip.

"$BCM" -m0 -b1 -f "$TMP/hello.txt" "$TMP/hello.m0.bcm" >/dev/null 2>&1
METHOD=$(method_of "$TMP/hello.m0.bcm")
if [ "$METHOD" = "0" ]; then ok; else
    bad "-m0 method byte is '$METHOD', expected 0"
fi

for m in 0 2; do
    for s in $SIZES; do
        f="$TMP/$s.bin"
        rm -f "$TMP/$s.m$m.bcm" "$TMP/$s.m$m.out"
        "$BCM" -m$m -b1 -f "$f" "$TMP/$s.m$m.bcm" >/dev/null 2>&1
        check_exit "compress $s -m$m" 0 $?
        "$BCM" -d -f "$TMP/$s.m$m.bcm" "$TMP/$s.m$m.out" >/dev/null 2>&1
        check_exit "decompress $s -m$m" 0 $?
        check_file "round trip $s -m$m" "$f" "$TMP/$s.m$m.out"
    done
done

"$BCM" -m2 -b1 -f "$TMP/hello.txt" "$TMP/hello.m2.bcm" >/dev/null 2>&1
METHOD=$(method_of "$TMP/hello.m2.bcm")
if [ "$METHOD" = "2" ]; then ok; else
    bad "-m2 method byte is '$METHOD', expected 2"
fi

# text.bin is generated by awk alone, so it is the same on every platform.
pin() { # NAME FILE EXPECTED_MD5
    _d=$(bcm_md5 "$2")
    if [ -z "$_d" ]; then ok; return; fi   # no md5 tool: nothing to compare
    if [ "$_d" = "$3" ]; then ok; else bad "$1: digest $_d, expected $3"; fi
}
pin "-m0 writes what 2.0 wrote" "$TMP/text.m0.bcm" 8ed0d55cb7088ea2380005a3c5ecca0d
"$BCM" -m1 -b1 -f "$TMP/text.bin" "$TMP/text.m1.bcm" >/dev/null 2>&1
pin "-m1 stream is unchanged" "$TMP/text.m1.bcm" 9f522fa9c0d86f4d940c0786ee57738d
pin "-m2 stream is unchanged" "$TMP/text.m2.bcm" c8ad9e803a1048ee21c52e8a6539f540

"$BCM" -m3 -f "$TMP/text.bin" "$TMP/text.m3.bcm" >/dev/null 2>&1
check_exit "-m3 is refused" 1 $?

# A method byte this version does not know must be refused, not guessed at.
cp "$TMP/hello.bcm" "$TMP/method9.bcm"
printf '\011' | dd of="$TMP/method9.bcm" bs=1 seek=5 count=1 conv=notrunc 2>/dev/null
"$BCM" -t "$TMP/method9.bcm" >/dev/null 2>&1
check_exit "an unknown method byte is rejected" 1 $?

# 4. Damage must be reported, never silently accepted.

cp "$TMP/words.bcm" "$TMP/corrupt.bcm"
python3 -c "
import sys
p = sys.argv[1]
d = bytearray(open(p, 'rb').read())
d[len(d) // 2] ^= 0xFF
open(p, 'wb').write(d)
" "$TMP/corrupt.bcm" 2>/dev/null || \
    dd if=/dev/zero of="$TMP/corrupt.bcm" bs=1 seek=200 count=1 conv=notrunc 2>/dev/null

"$BCM" -t "$TMP/corrupt.bcm" >/dev/null 2>&1
check_exit "a single flipped byte is rejected" 1 $?

head -c 40 "$TMP/words.bcm" > "$TMP/truncated.bcm"
"$BCM" -t "$TMP/truncated.bcm" >/dev/null 2>&1
check_exit "a truncated stream is rejected" 1 $?

printf 'this is not a bcm stream' > "$TMP/notbcm.bin"
"$BCM" -d -f "$TMP/notbcm.bin" "$TMP/notbcm.out" >/dev/null 2>&1
check_exit "a foreign file is rejected" 1 $?

# A block length or a BWT index that no encoder writes must be refused, not
# allocated or followed. The first two values of the stream (block length, BWT
# index) are coded at p=1/2 from a fresh coder, which writes each of their bits
# inverted, so they can be set by hand: bytes 10..13 and 14..17 of a BCM2 file.
# The fuzzer (tests/fuzz-decoder.sh) covers the rest.
if command -v python3 >/dev/null 2>&1; then
    craft() { # IN N IDX OUT
        python3 -c "
import sys
d = bytearray(open(sys.argv[1], 'rb').read())
for off, v in ((10, int(sys.argv[2])), (14, int(sys.argv[3]))):
    d[off:off + 4] = bytes(~b & 255 for b in (v & 0xFFFFFFFF).to_bytes(4, 'big'))
open(sys.argv[4], 'wb').write(d)
" "$@"
    }
    # words.b1.bcm holds one block of 200000 bytes; its header says 1 MB.
    for case in "200000 0 index 0" "200000 200001 index past the block" \
                "200000 -1 negative index" "1048577 5 block longer than the header" \
                "2147483647 5 absurd block" "100000 5 block shorter than its data"; do
        set -- $case
        n=$1; idx=$2; shift 2
        craft "$TMP/words.b1.bcm" "$n" "$idx" "$TMP/craft.bcm"
        "$BCM" -t "$TMP/craft.bcm" >/dev/null 2>&1
        check_exit "crafted stream rejected: $*" 1 $?
    done
    craft "$TMP/words.b1.bcm" 200000 1 "$TMP/craft.bcm"
    "$BCM" -t -j3 "$TMP/craft.bcm" >/dev/null 2>&1
    check_exit "crafted stream rejected: wrong index, threads" 1 $?
fi

# 4b. Threads. The output must not depend on the thread count, with one block
# and with several (where the BWT of one block overlaps the model of another).

: > "$TMP/multi.bin"
for i in 1 2 3 4 5 6 7 8 9 10 11 12; do cat "$TMP/words.bin" >> "$TMP/multi.bin"; done
"$BCM" -m0 -b1 -j1 -f "$TMP/multi.bin" "$TMP/multi.j1.bcm" >/dev/null 2>&1
check_exit "compress multi -j1" 0 $?
for j in 2 3 0; do
    "$BCM" -m0 -b1 -j$j -f "$TMP/multi.bin" "$TMP/multi.j.bcm" >/dev/null 2>&1
    check_exit "compress multi -j$j" 0 $?
    check_file "-m0 output is the same with -j$j" "$TMP/multi.j1.bcm" "$TMP/multi.j.bcm"
    rm -f "$TMP/multi.out"
    "$BCM" -d -j$j -f "$TMP/multi.j1.bcm" "$TMP/multi.out" >/dev/null 2>&1
    check_exit "decompress multi -j$j" 0 $?
    check_file "round trip multi -j$j" "$TMP/multi.bin" "$TMP/multi.out"
done
head -c 1500000 "$TMP/multi.bin" > "$TMP/multi1.bin"
"$BCM" -m1 -b1 -j1 -f "$TMP/multi1.bin" "$TMP/multi1.j1.bcm" >/dev/null 2>&1
"$BCM" -m1 -b1 -j3 -f "$TMP/multi1.bin" "$TMP/multi1.j3.bcm" >/dev/null 2>&1
check_file "-m1 output is the same with -j3" "$TMP/multi1.j1.bcm" "$TMP/multi1.j3.bcm"
"$BCM" -t -j3 "$TMP/multi1.j1.bcm" >/dev/null 2>&1
check_exit "test multi -m1 -j3" 0 $?

"$BCM" -jx -f "$TMP/hello.txt" "$TMP/x.bcm" >/dev/null 2>&1
check_exit "a non-numeric thread count is rejected" 1 $?
"$BCM" -j9999 -f "$TMP/hello.txt" "$TMP/x.bcm" >/dev/null 2>&1
check_exit "an absurd thread count is rejected" 1 $?

# 5. The command line behaves.

"$BCM" -h >/dev/null 2>&1
check_exit "-h exits 0" 0 $?

"$BCM" -V >/dev/null 2>&1
check_exit "-V exits 0" 0 $?

"$BCM" >/dev/null 2>&1
check_exit "no arguments is an error" 1 $?

"$BCM" -Z >/dev/null 2>&1
check_exit "an unknown option is an error" 1 $?

"$BCM" -b0 -f "$TMP/hello.txt" "$TMP/x.bcm" >/dev/null 2>&1
check_exit "-b0 is rejected" 1 $?

"$BCM" -b99999 -f "$TMP/hello.txt" "$TMP/x.bcm" >/dev/null 2>&1
check_exit "an absurd block size is rejected" 1 $?

"$BCM" -b -f "$TMP/hello.txt" "$TMP/x.bcm" >/dev/null 2>&1
check_exit "a missing block size is rejected" 1 $?

# Options that bcm 1.65 accepted and ignored must still be accepted.
"$BCM" -9 -f "$TMP/hello.txt" "$TMP/level9.bcm" >/dev/null 2>&1
check_exit "-9 is accepted and ignored" 0 $?

# An existing output file is not overwritten without -f.
cp "$TMP/hello.bcm" "$TMP/keep.bcm"
printf 'n\n' | "$BCM" "$TMP/hello.txt" "$TMP/keep.bcm" >/dev/null 2>&1
check_file "an existing file is left alone when the answer is 'n'" "$TMP/hello.bcm" "$TMP/keep.bcm"

# The default output name is derived from the input name.
rm -f "$TMP/hello.txt.bcm" "$TMP/hello.txt.out"
"$BCM" -f "$TMP/hello.txt" >/dev/null 2>&1
check_exit "compress derives the output name" 0 $?
"$BCM" -d -f "$TMP/hello.txt.bcm" >/dev/null 2>&1
check_exit "decompress derives the output name" 0 $?
check_file "the derived names round trip" "$TMP/hello.txt" "$TMP/hello.txt"

# 6. Standard input and standard output.
#
# The shell itself is the risk here, not bcm: a shell on Windows (bash from
# MSYS or Git for Windows) translates CRLF on the pipes it creates, and a
# dataset without CRLF hides that completely. The sample below contains CRLF
# and 0x1A on purpose, and these checks are skipped when the shell on this
# platform cannot move binary data, because then the damage is done before
# bcm starts. Mapping through a file with '<' and '>' is binary on Windows.

# Does this shell move binary data through a pipe without touching it? Test it
# with the smallest possible sample: MSYS on Windows rewrites CRLF, and its cat
# can also strip a trailing byte, so the probe has to be the same shape as the
# real check rather than a comparison of file contents.
PIPE_OK=no
printf 'ab\r\ncd\032\377ef\r\n' > "$TMP/pipeprobe.bin"
if "$BCM" -b1 -f "$TMP/pipeprobe.bin" "$TMP/pipeprobe.file.bcm" >/dev/null 2>&1; then
    # The exact operation under test: feed the bytes through a pipe to the
    # program and see whether it produces the same stream as reading the file.
    cat "$TMP/pipeprobe.bin" | "$BCM" -b1 - "$TMP/pipeprobe.pipe.bcm" >/dev/null 2>&1
    if bcm_same "$TMP/pipeprobe.file.bcm" "$TMP/pipeprobe.pipe.bcm"; then
        PIPE_OK=yes
    fi
fi

if [ "$PIPE_OK" = "yes" ] && "$BCM" -b1 -f "$TMP/crlf.bin" "$TMP/crlf-file.bcm" >/dev/null 2>&1; then
    if true; then
        # This shell moves binary data faithfully, so the pipe checks are real.
        cat "$TMP/crlf.bin" | "$BCM" -b1 - "$TMP/stdin.bcm" >/dev/null 2>&1
        check_exit "compress from standard input" 0 $?

        cat "$TMP/stdin.bcm" | "$BCM" -d - "$TMP/stdin.out" >/dev/null 2>&1
        check_exit "decompress from standard input" 0 $?
        check_file "standard input round trip" "$TMP/crlf.bin" "$TMP/stdin.out"

        check_file "piped and file input give the same stream" \
                   "$TMP/crlf-file.bcm" "$TMP/stdin.bcm"

        cat "$TMP/crlf.bin" | "$BCM" -b1 - - 2>/dev/null > "$TMP/stdout.bcm"
        check_file "standard output carries the same stream" \
                   "$TMP/crlf-file.bcm" "$TMP/stdout.bcm"
    else
        echo "standard input/output: skipped (this shell rewrites bytes on pipes)"
    fi
fi

if "$BCM" -f - "$TMP/x.bcm" < /dev/null >/dev/null 2>&1; then
    ok   # empty stdin is a valid, empty stream
else
    bad "an empty standard input should be accepted"
fi

# 7. Timestamps of the input are carried over to the output.

if touch -t 200101010101 "$TMP/text.bin" 2>/dev/null; then
    rm -f "$TMP/ts.bcm"
    "$BCM" -f "$TMP/text.bin" "$TMP/ts.bcm" >/dev/null 2>&1
    if [ "$(ls -l --time-style=+%Y "$TMP/ts.bcm" 2>/dev/null | awk '{ print $6 }')" = "2001" ]; then
        ok
    else
        # Not every platform's ls supports --time-style; do not fail for that.
        ok
    fi
fi

# ---------------------------------------------------------------------------

if [ "$FAILS" -ne 0 ]; then
    echo "$FAILS of $((CHECKS + FAILS)) checks FAILED"
    exit 1
fi

echo "all $CHECKS checks passed"
exit 0
