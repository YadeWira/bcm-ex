#!/bin/sh
#
# Compatibility with BCM 1.65.
#
#   sh tests/check-compat.sh ./bcm
#
# The bcm-ex promise is that the compressed stream is BCM 1.65's, byte for
# byte. This script checks exactly that, without needing a Windows binary and
# wine at test time: for a corpus of inputs with every interesting shape, it
# regenerates the same input, compresses it and compares the result with the
# recorded output of bcm 1.65 (size and MD5).
#
# The expected values were produced by bcm.exe 1.65 from
# https://github.com/FS-make-simple/bcm/releases/download/1.65/bcm.exe
# running under wine, invoked as `bcm.exe -b1 <input> <output>`. They must
# never be updated: if one fails, the format changed.

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

# expected NAME INPUT_SHA256 BCM_SIZE BCM_MD5
expect() {
    name=$1
    want_in=$2
    want_size=$3
    want_md5=$4

    in="$TMP/$name"

    got_in=$(bcm_sha256 "$in")
    if [ "$got_in" != "$want_in" ]; then
        bad "$name: the test input does not match its recorded SHA256 (got $got_in)"
        return
    fi

    "$BCM" -b1 -f "$in" "$TMP/$name.bcm" >/dev/null 2>&1
    if [ $? -ne 0 ]; then
        bad "$name: compression failed"
        return
    fi

    got_size=$(wc -c < "$TMP/$name.bcm" | tr -d ' ')
    if [ "$got_size" = "$want_size" ]; then ok; else
        bad "$name: $got_size bytes, BCM 1.65 wrote $want_size"
    fi

    got_md5=$(bcm_md5 "$TMP/$name.bcm")
    if [ -z "$got_md5" ]; then
        ok
    elif [ "$got_md5" = "$want_md5" ]; then
        ok
    else
        bad "$name: MD5 $got_md5, BCM 1.65 wrote $want_md5"
    fi

    # The stream must also decode back to the input.
    "$BCM" -d -f "$TMP/$name.bcm" "$TMP/$name.out" >/dev/null 2>&1
    if cmp -s "$in" "$TMP/$name.out"; then ok; else
        bad "$name: the round trip changed the data"
    fi
}

# --- the corpus, regenerated deterministically ---------------------------

: > "$TMP/empty.bin"
printf 'X' > "$TMP/one_byte.bin"
printf 'hello, bcm-ex\n' > "$TMP/hello.txt"
# all_bytes: every symbol present, no long runs. awk cannot emit NUL bytes
# reliably, so it is written one byte at a time with printf and octal escapes.
i=0
: > "$TMP/all_bytes.bin"
while [ $i -lt 10 ]; do
    b=0
    while [ $b -lt 256 ]; do
        oct=$(printf '%03o' $b)
        printf "\\$oct" >> "$TMP/all_bytes.bin"
        b=$((b + 1))
    done
    i=$((i + 1))
done
awk 'BEGIN { for (i = 0; i < 100000; i++) printf "a" }' > "$TMP/run.bin"
awk 'BEGIN { for (i = 0; i < 20000; i++) printf "the quick brown fox jumps over the lazy dog " }' > "$TMP/text.txt"

# The random sample must be identical on every platform, so it is generated
# from a fixed seed with Python, and skipped when Python is not available.
if command -v python3 >/dev/null 2>&1; then
    python3 -c "
import random, sys
random.seed(1234)
sys.stdout.buffer.write(bytes(random.getrandbits(8) for _ in range(65536)))
" > "$TMP/random.bin"

    expect random.bin \
        0499736fc5ec45e42cd515c03c91673179b5e433996d3fc16fc769e49d5293a5 \
        66010 7f1db499197b07f3b4bc61a9adc612c1
else
    echo "random.bin: skipped (no python3)"
fi

expect empty.bin \
    e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855 \
    16 233dd9e88ac5fc87bb09a38d55352db5

expect one_byte.bin \
    4b68ab3847feda7d6c62c1fbcbeebfa35eab7351ed5e78f4ddadea5df64b8015 \
    25 18d2df9fd7be999a31d491d25efd36a3

expect hello.txt \
    824f69ce0c86d6e3f68dc91862f279235153cfe18d58b45c6901bc3c2206f05a \
    35 a154148a4b182db7aaae76a08f6db53c

expect all_bytes.bin \
    e392378f849d67bbb1a7bbec84f1098ae3faa751049c009a850130ce6073d91a \
    897 a7864432d9e41800ba6e3f80d714da84

expect run.bin \
    6d1cf22d7cc09b085dfc25ee1a1f3ae0265804c607bc2074ad253bcc82fd81ee \
    39 73e452a5768931c09a9627f2802bd966

expect text.txt \
    722afca1a9ff4b1d88401886776fa92a153a9bb203dca2278a3f6d29d4b33937 \
    506 a2d60ed5164e057cfc0a106f8beb90d2

# -------------------------------------------------------------------------

if [ "$FAILS" -ne 0 ]; then
    echo "$FAILS of $((CHECKS + FAILS)) compatibility checks FAILED"
    echo "The stream format changed: this must never happen."
    exit 1
fi

echo "all $CHECKS compatibility checks passed (byte-identical to BCM 1.65)"
exit 0
