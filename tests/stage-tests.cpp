/*

Tests for the v2 preprocessing stages, before they are wired into the
compressor.

  make -C tests stage-tests

Every stage must be exactly reversible, and the numbers it prints are the ones
quoted in the documentation.

*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../src/x86f.h"
#include "../src/lzp.h"

using namespace bcmext;

static unsigned char* Slurp(const char* path, size_t* n)
{
    FILE* f = fopen(path, "rb");
    if (!f) { perror(path); exit(1); }
    fseek(f, 0, SEEK_END);
    const long sz = ftell(f);
    rewind(f);
    unsigned char* b = (unsigned char*)malloc(size_t(sz) ? size_t(sz) : 1);
    if (fread(b, 1, size_t(sz), f) != size_t(sz)) { perror("read"); exit(1); }
    fclose(f);
    *n = size_t(sz);
    return b;
}

static int fails = 0;

#define CHECK(cond, msg) do { if (!(cond)) { printf("  FAIL: %s\n", msg); fails++; } } while (0)

// --- x86 filter -----------------------------------------------------------

static void TestX86(const char* path)
{
    size_t n;
    unsigned char* orig = Slurp(path, &n);
    unsigned char* buf = (unsigned char*)malloc(n + 16);
    memcpy(buf, orig, n);

    // Block sizes that are not multiples of the filter's lookahead exercise the
    // carried state across blocks, which is what a real compressor will do.
    const size_t blocks[] = { n, 65536, 4096, 100, 7 };
    for (size_t bi = 0; bi < sizeof(blocks) / sizeof(blocks[0]); bi++)
    {
        const size_t bs = blocks[bi] < 5 ? 0 : blocks[bi];
        if (!bs)
            continue;

        memcpy(buf, orig, n);

        uint32_t st = 0;
        for (size_t off = 0; off < n; off += bs)
        {
            const size_t len = (n - off < bs) ? (n - off) : bs;
            st = X86Convert(buf + off, len, uint32_t(off), st, true);
        }
        st = 0;
        for (size_t off = 0; off < n; off += bs)
        {
            const size_t len = (n - off < bs) ? (n - off) : bs;
            st = X86Convert(buf + off, len, uint32_t(off), st, false);
        }

        char msg[128];
        snprintf(msg, sizeof(msg), "%s: x86 round trip with %zu-byte blocks", path, bs);
        CHECK(memcmp(buf, orig, n) == 0, msg);
    }

    // How many bytes does the filter change? That is the signal the model sees.
    // buf currently holds the round-tripped data, which equals orig by now, so
    // encode once more from a clean copy.
    memcpy(buf, orig, n);
    {
        uint32_t st = 0;
        for (size_t off = 0; off < n; off += 65536)
        {
            const size_t len = (n - off < 65536) ? (n - off) : 65536;
            st = X86Convert(buf + off, len, uint32_t(off), st, true);
        }
    }
    size_t changed = 0;
    for (size_t i = 0; i < n; i++)
        if (buf[i] != orig[i])
            changed++;

    printf("  %-28s %10zu -> %10zu bytes changed (%5.2f%%)\n",
           path, n, changed, 100.0 * double(changed) / double(n));

    free(orig);
    free(buf);
}

// --- LZP ------------------------------------------------------------------

static void TestLzp(const char* path)
{
    size_t n;
    unsigned char* orig = Slurp(path, &n);

    for (int order = LZP_MIN_ORDER; order <= LZP_MAX_ORDER; order++)
    {
        LzpBuf enc, dec;

        if (!LzpEncode(orig, n, order, enc))
        {
            printf("  %-20s order %d: not worth it\n", path, order);
            continue;
        }

        const bool ok = LzpDecode(enc.data, enc.size, dec);
        CHECK(ok, "lzp decode returned false");

        char msg[160];
        snprintf(msg, sizeof(msg), "%s: lzp round trip, order %d", path, order);
        CHECK(ok && dec.size == n && memcmp(dec.data, orig, n) == 0, msg);

        printf("  %-20s order %d: %10zu -> %10zu (%6.2f%%)%s\n",
               path, order, n, enc.size,
               100.0 * double(enc.size) / double(n),
               (enc.size < n) ? "" : "  [grows]");
    }

    free(orig);
}

int main(int argc, char** argv)
{
    printf("=== x86 BCJ filter ===\n");
    for (int i = 1; i < argc; i++)
        TestX86(argv[i]);

    printf("=== LZP ===\n");
    for (int i = 1; i < argc; i++)
        TestLzp(argv[i]);

    if (fails)
    {
        printf("%d FAILURES\n", fails);
        return 1;
    }
    printf("all stage tests passed\n");
    return 0;
}
