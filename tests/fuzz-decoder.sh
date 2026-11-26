#!/bin/sh
#
# Decoder fuzzing.
#
#   sh tests/fuzz-decoder.sh ./bcm [ITERATIONS]
#
# A valid stream is damaged in every way that matters and the decoder must
# always exit cleanly: either it decodes (exit 0, which is only correct when
# the damage was in the padding) or it reports an error (exit 1). A crash, a
# hang or an out-of-memory kill is a bug.
#
# This is the test that found ptr[-1] in the inverse BWT of BCM 1.65: a
# corrupted stream made it read before the start of the array, which segfaults
# on Windows and can be exploited on any target.
#
# The check values are the ones in tests/check-compat.sh, so this runs on the
# same corpus. Python 3 is only needed to generate the damage; without it the
# script says so and exits 0.

set -u

BCM=${1:-./bcm}
ITER=${2:-200}
TMP=${TMPDIR:-/tmp}/bcm-ex-fuzz-$$
FAILS=0

mkdir -p "$TMP" || exit 1
trap 'rm -rf "$TMP"' EXIT INT TERM

if ! command -v python3 >/dev/null 2>&1; then
    echo "fuzz-decoder: skipped (no python3)"
    exit 0
fi

# A stream large enough that every part of the decoder runs, small enough to
# fuzz hundreds of times.
python3 -c "
import sys, random
random.seed(7)
words = [''.join(random.choice('abcdefghijklmnopqrstuvwxyz') for _ in range(random.randint(2, 9)))
         for _ in range(500)]
sys.stdout.buffer.write((' '.join(random.choice(words) for _ in range(40000))).encode())
" > "$TMP/seed.bin"

"$BCM" -b1 -f "$TMP/seed.bin" "$TMP/seed.bcm" >/dev/null 2>&1 || {
    echo "error: could not create the seed stream" >&2
    exit 2
}

SEED_SIZE=$(wc -c < "$TMP/seed.bcm" | tr -d ' ')

python3 - "$BCM" "$TMP/seed.bcm" "$TMP/seed.bin" "$TMP" "$ITER" <<'PY'
import os
import random
import subprocess
import sys

bcm, seed_path, orig_path, tmp, iterations = sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4], int(sys.argv[5])
seed = open(seed_path, 'rb').read()
original = open(orig_path, 'rb').read()
random.seed(20260101)

def run(data):
    path = os.path.join(tmp, 'case.bcm')
    out = os.path.join(tmp, 'case.out')
    if os.path.exists(out):
        os.unlink(out)
    with open(path, 'wb') as f:
        f.write(data)
    try:
        # A decoder that hangs is a bug too, so give it a limit.
        rc = subprocess.run([bcm, '-d', '-f', path, out],
                            capture_output=True, timeout=120).returncode
    except subprocess.TimeoutExpired:
        return 'timeout', None
    decoded = None
    if rc == 0:
        try:
            decoded = open(out, 'rb').read()
        except OSError:
            decoded = None
    return rc, decoded

crashes = []
wrong = []
rejected = 0
accepted = 0

for i in range(iterations):
    kind = i % 5
    data = bytearray(seed)

    if kind == 0:                      # one flipped bit
        p = random.randrange(len(data))
        data[p] ^= 1 << random.randrange(8)
    elif kind == 1:                    # one random byte
        data[random.randrange(len(data))] = random.getrandbits(8)
    elif kind == 2:                    # truncated
        data = data[:random.randrange(len(data))]
    elif kind == 3:                    # a run of garbage
        p = random.randrange(len(data))
        n = min(len(data) - p, random.randrange(1, 64))
        data[p:p + n] = bytes(random.getrandbits(8) for _ in range(n))
    else:                              # bytes appended
        data += bytes(random.getrandbits(8) for _ in range(random.randrange(1, 32)))

    rc, decoded = run(bytes(data))

    if rc == 0:
        accepted += 1
        # Accepting damage is only correct when the damaged bytes were never
        # part of the stream (trailing bytes, the encoder's final padding).
        if decoded != original:
            wrong.append((i, kind))
    elif rc == 1:
        rejected += 1
    else:
        crashes.append((i, kind, rc))

if crashes:
    for i, kind, rc in crashes[:10]:
        print(f'FAIL: iteration {i} (kind {kind}) returned {rc}')
if wrong:
    for i, kind in wrong[:10]:
        print(f'FAIL: iteration {i} (kind {kind}) was accepted but decoded the wrong data')
if crashes or wrong:
    sys.exit(1)

print(f'{iterations} damaged streams: {rejected} rejected, '
      f'{accepted} accepted with the correct output, 0 crashes')
PY
status=$?

if [ "$status" -ne 0 ]; then
    echo "the decoder crashed or hung on damaged input"
    exit 1
fi

exit 0
