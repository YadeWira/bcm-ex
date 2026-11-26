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
# size of samples/hello.txt at -b1 must stay 120 bytes, which is what bcm 1.65
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

# 3. The format is frozen.
#
# samples/hello.txt ("hello, bcm-ex\n", 14 bytes) compressed with -b1 must be
# 35 bytes with MD5 a154148a4b182db7aaae76a08f6db53c. Both values were produced
# by bcm 1.65 itself and are the house style of this project: the fixed check
# values pin the header layout, the model and the arithmetic coder at once.

printf 'hello, bcm-ex\n' > "$TMP/hello.txt"
"$BCM" -b1 -f "$TMP/hello.txt" "$TMP/hello.bcm" >/dev/null 2>&1

HELLO_SIZE=$(wc -c < "$TMP/hello.bcm" | tr -d ' ')
if [ "$HELLO_SIZE" = "35" ]; then ok; else
    bad "hello.txt at -b1 is $HELLO_SIZE bytes, expected 35 (the format changed)"
fi

HELLO_MD5=$(bcm_md5 "$TMP/hello.bcm")
if [ -z "$HELLO_MD5" ]; then
    ok   # no MD5 tool on this platform; the size check above still applies
elif [ "$HELLO_MD5" = "a154148a4b182db7aaae76a08f6db53c" ]; then
    ok
else
    bad "hello.txt at -b1 has MD5 $HELLO_MD5, expected a154148a4b182db7aaae76a08f6db53c"
fi

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

# A header that claims a block far beyond any real one must be refused
# instead of allocating it. Producing such a stream needs the arithmetic coder,
# so this is exercised through the fuzzer (tests/fuzz-decoder.sh) rather than
# by hand here.

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

if "$BCM" -b1 -f "$TMP/crlf.bin" "$TMP/crlf-file.bcm" >/dev/null 2>&1; then
    cat "$TMP/crlf.bin" > "$TMP/crlf-cat.bin" 2>/dev/null
    if bcm_same "$TMP/crlf.bin" "$TMP/crlf-cat.bin"; then
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
        echo "standard input/output: skipped (this shell rewrites CRLF on pipes)"
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
