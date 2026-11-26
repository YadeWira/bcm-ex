/*

BCM-ex - the extended line of BCM

LZP stage for the v2 container.

Why this is not a plain LZP. Measured on Silesia and enwik8 (see the wiki's
Benchmarks page), a literal-for-literal LZP in front of the BWT *loses* on data
that is already redundant: nci went from 1,227,055 to 1,521,590 bytes and xml
from 394,641 to 437,134. The BWT and the context model already exploit those
repetitions, and replacing a run with a length removes the context they use.

So this stage is a predictor, not a compressor:

  - It predicts the next byte from a hash of the previous `order` bytes.
  - A bit says whether the prediction was right, and only when it is right does
    a match length follow, packed as a varint.
  - The flags are a separate bit stream, so a miss costs one bit, not one byte.

And, above all, it is *tried and kept only if it helps*: encoding runs it for
several orders and keeps the best result, or none. The chosen order travels in
the stream, so decoding is exact. That is the part BCM 2.03 leaves to the user
as `-l#`, where the right value depends on the data; here it is decided per
file, from the data.

The encoder and the decoder update the hash table on exactly the same
positions: after every byte the decoder has emitted, the context that *ends* at
that position is registered. Keeping that rule identical on both sides is what
makes the transform reversible; it is checked by tests/stage-tests.cpp.

*/

#ifndef BCM_LZP_H
#define BCM_LZP_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

namespace bcmext
{

static const int LZP_MIN_MATCH = 32;
static const int LZP_MIN_ORDER = 1;
static const int LZP_MAX_ORDER = 4;
static const uint32_t LZP_HLOG = 22;

struct LzpBuf
{
    uint8_t* data;
    size_t size;
    size_t cap;

    LzpBuf() : data(0), size(0), cap(0) {}
    ~LzpBuf() { free(data); }

    bool Reserve(size_t n)
    {
        if (n <= cap)
            return true;
        uint8_t* p = (uint8_t*)realloc(data, n);
        if (!p)
            return false;
        data = p;
        cap = n;
        return true;
    }

private:
    LzpBuf(const LzpBuf&);
    LzpBuf& operator=(const LzpBuf&);
};

// Hash of the `order` bytes ending just before `p`.
inline uint32_t LzpHash(const uint8_t* p, int order, uint32_t mask)
{
    uint32_t h = 0;
    for (int i = order; i >= 1; i--)
        h = (h ^ p[-i]) * 0x9E3779B1u;
    return h & mask;
}

// Shared table update: called once per byte that the decoder has produced.
// `pos` is the number of bytes produced so far; the context ending at `pos` is
// registered when there are enough bytes for that context.
struct LzpTable
{
    uint32_t* tab;
    uint8_t* seen;
    int order;
    uint32_t mask;

    LzpTable() : tab(0), seen(0), order(0), mask(0) {}

    bool Init(int ord)
    {
        order = ord;
        mask = (1u << LZP_HLOG) - 1;
        tab = (uint32_t*)calloc(size_t(mask) + 1, sizeof(uint32_t));
        seen = (uint8_t*)calloc(size_t(mask) + 1, 1);
        return tab && seen;
    }

    void Free() { free(tab); free(seen); tab = 0; seen = 0; }

    // Register the context that ends at byte position `pos`.
    inline void Advance(const uint8_t* buf, size_t pos)
    {
        if (pos < size_t(order))
            return;
        const uint32_t h = LzpHash(buf + pos, order, mask);
        seen[h] = 1;
        tab[h] = uint32_t(pos);
    }

    inline bool Get(const uint8_t* buf, size_t pos, uint32_t* out) const
    {
        if (pos < size_t(order))
            return false;
        const uint32_t h = LzpHash(buf + pos, order, mask);
        if (!seen[h])
            return false;
        *out = tab[h];
        return true;
    }
};

inline bool LzpEncode(const uint8_t* src, size_t n, int order, LzpBuf& dst)
{
    if (n <= size_t(order) || order < 1 || order > 16)
        return false;

    const size_t bits_cap = n / 8 + 64;
    uint8_t* bits = (uint8_t*)calloc(bits_cap, 1);
    uint8_t* pay = (uint8_t*)malloc(n + 1024);
    LzpTable t;

    if (!bits || !pay || !t.Init(order))
    {
        free(bits); free(pay); t.Free();
        return false;
    }

    size_t nb = 0, npa = 0, produced = 0;

    // The first `order` bytes cannot be predicted: they are literals, and each
    // one advances the table exactly as the decoder will.
    for (size_t i = 0; i < size_t(order); i++)
    {
        pay[npa++] = src[i];
        nb++;                        // the flag bit is already zero
        produced++;
    }

    while (produced < n)
    {
        uint32_t a = 0;
        size_t len = 0;

        if (t.Get(src, produced, &a))
        {
            while (produced + len < n && a + len < n &&
                   src[a + len] == src[produced + len])
                len++;
        }

        if (len >= size_t(LZP_MIN_MATCH))
        {
            bits[nb >> 3] |= uint8_t(1u << (nb & 7));
            nb++;

            size_t r = len - size_t(LZP_MIN_MATCH);
            while (r >= 254)
            {
                pay[npa++] = 254;
                r -= 254;
            }
            pay[npa++] = uint8_t(r);

            for (size_t j = 0; j < len; j++)
            {
                t.Advance(src, produced);
                produced++;
            }
        }
        else
        {
            pay[npa++] = src[produced];
            nb++;
            t.Advance(src, produced);
            produced++;
        }
    }

    const size_t nbits = (nb + 7) / 8;
    const size_t total = 16 + nbits + npa;

    bool ok = false;
    if (total < n && dst.Reserve(total))
    {
        dst.data[0] = 'L';
        dst.data[1] = uint8_t(order);
        dst.data[2] = uint8_t(LZP_MIN_MATCH);
        dst.data[3] = 0;
        uint32_t v;
        v = uint32_t(nb);  memcpy(dst.data + 4, &v, 4);
        v = uint32_t(npa); memcpy(dst.data + 8, &v, 4);
        v = uint32_t(n);   memcpy(dst.data + 12, &v, 4);
        memcpy(dst.data + 16, bits, nbits);
        memcpy(dst.data + 16 + nbits, pay, npa);
        dst.size = total;
        ok = true;
    }

    free(bits);
    free(pay);
    t.Free();
    return ok;
}

inline bool LzpDecode(const uint8_t* src, size_t n, LzpBuf& dst)
{
    if (n < 16 || src[0] != 'L')
        return false;

    const int order = src[1];
    const int min_match = src[2];
    uint32_t nb, npa, orig;
    memcpy(&nb, src + 4, 4);
    memcpy(&npa, src + 8, 4);
    memcpy(&orig, src + 12, 4);

    if (order < 1 || order > 16 || min_match < 1 || orig == 0)
        return false;

    const size_t nbits = (size_t(nb) + 7) / 8;
    if (16 + nbits + size_t(npa) > n)
        return false;

    const uint8_t* bits = src + 16;
    const uint8_t* pay = src + 16 + nbits;

    if (!dst.Reserve(size_t(orig) + 64))
        return false;

    LzpTable t;
    if (!t.Init(order))
        return false;

    uint8_t* o = dst.data;
    size_t produced = 0, bi = 0, pi = 0;

    while (produced < size_t(orig))
    {
        const int is_match = (bits[bi >> 3] >> (bi & 7)) & 1;
        bi++;

        if (!is_match)
        {
            if (pi >= size_t(npa))
                goto bad;
            o[produced] = pay[pi++];
            t.Advance(o, produced);
            produced++;
        }
        else
        {
            size_t len = size_t(min_match);
            for (;;)
            {
                if (pi >= size_t(npa))
                    goto bad;
                const uint8_t b = pay[pi++];
                len += b;
                if (b != 254)
                    break;
            }

            uint32_t a = 0;
            if (!t.Get(o, produced, &a))
                goto bad;

            for (size_t j = 0; j < len && produced < size_t(orig); j++)
            {
                o[produced] = o[a];
                t.Advance(o, produced);
                a++;
                produced++;
            }
        }
    }

    t.Free();
    dst.size = produced;
    return produced == size_t(orig);

bad:
    t.Free();
    dst.size = produced;
    return false;
}

} // namespace bcmext

#endif // BCM_LZP_H
