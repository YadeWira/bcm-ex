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
