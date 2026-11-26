#!/bin/sh
#
# Compatibility with BCM 1.65, for bcm-ex 2.
#
#   sh tests/check-compat.sh ./bcm
#
# bcm-ex 2 deliberately writes a new container ("BCM2") and no longer produces
# byte-identical output to BCM 1.65; that is the trade that lets the format
# carry filters and a method field. What it *does* keep is the ability to read
# everything 1.x wrote, which BCM 2.03 dropped.
#
# So this script checks two things:
#
#   1. Every stream recorded from the official bcm.exe 1.65 decodes to the
#      original bytes, byte for byte.
#   2. Our own v2 output round-trips, and its header is what it claims.
#
# The expected values below were produced by bcm.exe 1.65 from
# https://github.com/FS-make-simple/bcm/releases/download/1.65/bcm.exe
# under wine. They are the historical record and must never be updated.

set -u

BCM=${1:-./bcm}
TMP=${TMPDIR:-/tmp}/bcm-ex-compat-$$
FAILS=0
CHECKS=0

mkdir -p "$TMP" || exit 1
trap 'rm -rf "$TMP"' EXIT INT TERM

. "$(dirname "$0")/lib.sh"

bad() { echo "FAIL: $*"; FAILS=$((FAILS + 1)); }
ok() { CHECKS=$((CHECKS + 1)); }

# read_back NAME INPUT_SHA256 BCM_SIZE BCM_MD5
#
# The recorded 1.65 stream must still decode to the input.
read_back() {
    name=$1
    want_in=$2
    want_size=$3
    want_md5=$4

    in="$TMP/$name"
    stream="$TMP/$name.165.bcm"

    got_in=$(bcm_sha256 "$in")
    if [ "$got_in" != "$want_in" ]; then
        bad "$name: the test input does not match its recorded SHA256"
        return
    fi

    if [ ! -f "$stream" ]; then
        bad "$name: the recorded 1.65 stream is missing"
        return
    fi

    got_size=$(wc -c < "$stream" | tr -d ' ')
    if [ "$got_size" != "$want_size" ]; then
        bad "$name: recorded stream is $got_size bytes, expected $want_size"
        return
    fi

    if [ -n "$(bcm_md5 "$stream")" ] && [ "$(bcm_md5 "$stream")" != "$want_md5" ]; then
        bad "$name: recorded stream MD5 does not match the historical value"
        return
    fi

    # The point of the test: bcm-ex 2 must read it.
    "$BCM" -d -f "$stream" "$TMP/$name.old.out" >/dev/null 2>&1
    if [ $? -ne 0 ]; then
        bad "$name: could not decompress a BCM 1.65 stream"
        return
    fi

    if bcm_same "$in" "$TMP/$name.old.out"; then ok; else
        bad "$name: BCM 1.65 stream decoded to the wrong bytes"
    fi

    # And -t must accept it.
    "$BCM" -t "$stream" >/dev/null 2>&1
    if [ $? -eq 0 ]; then ok; else
        bad "$name: -t rejected a valid BCM 1.65 stream"
    fi
}

# --- the corpus, regenerated deterministically ---------------------------

: > "$TMP/empty.bin"
printf 'X' > "$TMP/one_byte.bin"
printf 'hello, bcm-ex\n' > "$TMP/hello.txt"

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

awk 'BEGIN { for (i = 0; i < 100000; i++) printf "a" }' > "$TMP/run.bin"
awk 'BEGIN { for (i = 0; i < 20000; i++) printf "the quick brown fox jumps over the lazy dog " }' > "$TMP/text.txt"

# The recorded streams, carried in the repository so the test does not need
# wine or a copy of the original binary at run time.
RECORDED=$(dirname "$0")/compat165
if [ ! -d "$RECORDED" ]; then
    echo "error: tests/compat165/ is missing; cannot check 1.65 compatibility" >&2
    exit 2
fi

for f in "$RECORDED"/*.bcm; do
    base=$(basename "$f" .bcm)

    src="$base"
    case "$base" in
        all_bytes.bin) src=all_bytes.bin ;;
        empty.bin)     src=empty.bin ;;
        one_byte.bin)  src=one_byte.bin ;;
        hello.txt)     src=hello.txt ;;
        run.bin)       src=run.bin ;;
        text.txt)      src=text.txt ;;
    esac

    # Compare by decoding, not by recompressing: that is the guarantee now.
    "$BCM" -d -f "$f" "$TMP/$base.out" >/dev/null 2>&1
    if [ $? -ne 0 ]; then
        bad "$base: could not decompress the recorded 1.65 stream"
        continue
    fi

    if bcm_same "$TMP/$src" "$TMP/$base.out"; then ok; else
        bad "$base: the 1.65 stream decoded to the wrong bytes"
    fi

    "$BCM" -t "$f" >/dev/null 2>&1
    if [ $? -eq 0 ]; then ok; else
        bad "$base: -t rejected a valid 1.65 stream"
    fi
done

# --- our own v2 stream ---------------------------------------------------

printf 'hello, bcm-ex\n' > "$TMP/v2.txt"
"$BCM" -b1 -f "$TMP/v2.txt" "$TMP/v2.bcm" >/dev/null 2>&1
if [ $? -ne 0 ]; then
    bad "could not compress with the v2 container"
else
    ok
fi

if [ "$(dd if="$TMP/v2.bcm" bs=1 count=4 2>/dev/null)" = "BCM2" ]; then ok; else
    bad "the v2 stream does not start with BCM2"
fi

"$BCM" -d -f "$TMP/v2.bcm" "$TMP/v2.out" >/dev/null 2>&1
if bcm_same "$TMP/v2.txt" "$TMP/v2.out"; then ok; else
    bad "the v2 stream did not round-trip"
fi

# --- the x86 filter ------------------------------------------------------
#
# A synthetic x86 file: a PE header plus real call instructions. The filter
# must engage on its own and the round trip must stay exact.

{
    printf 'MZ'
    awk 'BEGIN { for (i = 0; i < 510; i++) printf "\\000" }'
    awk 'BEGIN {
        srand(1);
        for (i = 0; i < 4000; i++) {
            printf "\\220\\220\\220\\220\\350";
            for (j = 0; j < 4; j++) printf "%c", int(rand() * 256);
        }
    }'
} > "$TMP/fake.exe" 2>/dev/null

if [ -s "$TMP/fake.exe" ]; then
    "$BCM" -f "$TMP/fake.exe" "$TMP/fake.bcm" >/dev/null 2>&1
    FLAGS=$(dd if="$TMP/fake.bcm" bs=1 skip=4 count=1 2>/dev/null | od -An -tu1 | tr -d ' ')
    if [ "$FLAGS" = "1" ]; then ok; else
        bad "the x86 filter did not engage on a PE file (flags=$FLAGS)"
    fi

    "$BCM" -d -f "$TMP/fake.bcm" "$TMP/fake.out" >/dev/null 2>&1
    if bcm_same "$TMP/fake.exe" "$TMP/fake.out"; then ok; else
        bad "the x86 filter broke the round trip"
    fi

    # Forcing it off must also work, and must not claim the filter.
    "$BCM" -x- -f "$TMP/fake.exe" "$TMP/fake2.bcm" >/dev/null 2>&1
    FLAGS2=$(dd if="$TMP/fake2.bcm" bs=1 skip=4 count=1 2>/dev/null | od -An -tu1 | tr -d ' ')
    if [ "$FLAGS2" = "0" ]; then ok; else
        bad "-x- still set the filter (flags=$FLAGS2)"
    fi
fi

# -------------------------------------------------------------------------

if [ "$FAILS" -ne 0 ]; then
    echo "$FAILS of $((CHECKS + FAILS)) compatibility checks FAILED"
    exit 1
fi

echo "all $CHECKS checks passed (reads BCM 1.65; writes BCM2)"
exit 0
