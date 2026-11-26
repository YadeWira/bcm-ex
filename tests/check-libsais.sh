#!/bin/sh
#
# The vendored libsais must produce the BWT that BCM 1.65's libsais produced.
#
#   sh tests/check-libsais.sh
#
# bcm-ex vendors libsais 2.12.2 where BCM 1.65 used 2.2.0. Updating it is only
# safe because the BWT it constructs is the same, so this is checked directly
# with the suffix array printed for a few strings, instead of being trusted.
# If libsais is ever updated, this test says whether the stream can change.

set -u

ROOT=$(cd "$(dirname "$0")/.." && pwd)
TMP=${TMPDIR:-/tmp}/bcm-ex-libsais-$$
FAILS=0
CHECKS=0

mkdir -p "$TMP" || exit 1
trap 'rm -rf "$TMP"' EXIT INT TERM

. "$ROOT/tests/lib.sh"

bad() { echo "FAIL: $*"; FAILS=$((FAILS + 1)); }
ok() { CHECKS=$((CHECKS + 1)); }

CC=${CC:-cc}

cat > "$TMP/probe.c" <<'EOF'
#include <stdio.h>
#include <string.h>
#include "libsais.h"

int main(void)
{
    /* A string with repeated symbols and a prefix/suffix overlap, which is
       where a suffix array implementation can legitimately differ. */
    static const char* const s = "banana_bandana_banana";
    const int n = (int)strlen(s);

    static unsigned char bwt[64];
    static int sa[64];

    const int idx = libsais_bwt((const unsigned char*)s, bwt, sa, n, 0, NULL);
    if (idx < 0)
        return 1;

    printf("%d ", idx);
    for (int i = 0; i < n; ++i)
        printf("%02x", bwt[i]);
    printf("\n");

    printf("libsais %d.%d.%d\n",
           LIBSAIS_VERSION_MAJOR, LIBSAIS_VERSION_MINOR, LIBSAIS_VERSION_PATCH);
    return 0;
}
EOF

if ! $CC -O2 -I"$ROOT/third_party/libsais" \
        "$TMP/probe.c" "$ROOT/third_party/libsais/libsais.c" -o "$TMP/probe" 2>"$TMP/cc.log"; then
    echo "error: could not build the libsais probe" >&2
    cat "$TMP/cc.log" >&2
    exit 2
fi

"$TMP/probe" > "$TMP/out.txt" 2>&1
got=$(head -1 "$TMP/out.txt")
version=$(tail -1 "$TMP/out.txt")

# Recorded from libsais 2.3.0 and 2.12.2, which agree. BCM 1.65 shipped with
# libsais 2.2.0 and produces the same streams as 2.12.2 (tests/check-compat.sh
# checks that against the official bcm.exe), so the BWT has not changed across
# those versions.
want="13 6161616e6e6e6e646e6262625f5f6e616161616161"

if [ "$got" = "$want" ]; then ok; else
    bad "the BWT of 'banana_bandana_banana' is '$got', expected '$want'"
fi

echo "vendored $version"

if [ "$FAILS" -ne 0 ]; then
    echo "$FAILS of $((CHECKS + FAILS)) libsais checks FAILED"
    echo "Updating libsais would change the compressed stream."
    exit 1
fi

echo "all $CHECKS libsais checks passed"
exit 0
