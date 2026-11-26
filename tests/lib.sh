# Shared shell helpers for the bcm-ex test scripts.
#
# Sourced, not executed. Provides a portable MD5 and SHA-256 so the same
# scripts run on Linux (md5sum, sha256sum), macOS (md5 -q, shasum) and the
# MSYS bash on Windows (md5sum).

# bcm_md5 FILE -> prints the lowercase hex digest, or nothing if unavailable.
bcm_md5() {
    _f=$1

    if command -v md5sum >/dev/null 2>&1; then
        md5sum "$_f" 2>/dev/null | cut -d' ' -f1
    elif command -v md5 >/dev/null 2>&1; then
        md5 -q "$_f" 2>/dev/null
    fi
}

# bcm_sha256 FILE -> prints the lowercase hex digest, or nothing.
bcm_sha256() {
    _f=$1

    if command -v sha256sum >/dev/null 2>&1; then
        sha256sum "$_f" 2>/dev/null | cut -d' ' -f1
    elif command -v shasum >/dev/null 2>&1; then
        shasum -a 256 "$_f" 2>/dev/null | cut -d' ' -f1
    fi
}

# bcm_have COMMAND -> true when the command exists.
bcm_have() {
    command -v "$1" >/dev/null 2>&1
}

# bcm_same FILE1 FILE2 -> true when the two files are byte-identical.
# cmp is not present in every shell environment (MSYS2 minimal, for one), so it
# is used when it works and the digest path is used when it does not.
bcm_same() {
    if cmp -s "$1" "$2" 2>/dev/null; then
        return 0
    fi

    # cmp failed or is missing: fall back to comparing the digests.
    _a=$(bcm_md5 "$1")
    _b=$(bcm_md5 "$2")
    if [ -n "$_a" ] && [ -n "$_b" ]; then
        [ "$_a" = "$_b" ]
        return $?
    fi

    # No digest tool either. Comparing only the sizes would silently pass a
    # wrong file, so read both files and compare them with the shell instead.
    if [ "$(cat "$1" 2>/dev/null)" = "$(cat "$2" 2>/dev/null)" ]; then
        return 0
    fi
    return 1
}
