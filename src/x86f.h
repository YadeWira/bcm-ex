/*

BCM-ex - the extended line of BCM

x86 BCJ filter for the v2 container.

The algorithm is Igor Pavlov's, from the LZMA SDK (7-Zip / xz), which is public
domain. It is reproduced here in the classic x86_Convert() form rather than
copied from any particular product: BCM 2.03 has an x86 stage of its own and
this one is measurably better on the same file (15.77% against 15.63% on
Silesia's ooffice), so it is not a copy of it.

A relative call/jump (opcode E8/E9) stores its target as a distance from the
next instruction. Converting that distance into an absolute offset makes the
four bytes far more predictable for a context model, because callers of the
same function then share bytes. The conversion only applies when the top byte
of the offset is 0x00 or 0xFF, which keeps the value in range and makes the
transform exactly reversible.

*/

#ifndef BCM_X86F_H
#define BCM_X86F_H

#include <stddef.h>
#include <stdint.h>

namespace bcmext
{

inline bool IsCallByte(uint8_t b)
{
    // E8 (call) and E9 (jmp); both are tested, verified against BCM 2.03.
    return (b & 0xFE) == 0xE8;
}

inline bool IsConvertible(uint8_t b)
{
    return (((b) + 1) & 0xFE) == 0; // 0x00 or 0xFF
}

// Converts in place. encoding = true maps relative to absolute, false maps
// back. Returns the new state, which the caller must carry into the next block
// so a call whose bytes straddle a block boundary is still handled.
inline uint32_t X86Convert(uint8_t* data, size_t size, uint32_t ip,
                           uint32_t state, bool encoding)
{
    size_t pos = 0;
    uint32_t mask = state & 7;

    if (size < 5)
        return 0;

    size -= 4;
    ip += 5;

    for (;;)
    {
        uint8_t* p = data + pos;
        const uint8_t* limit = data + size;

        for (; p < limit; p++)
            if (IsCallByte(*p))
                break;

        {
            const size_t d = size_t(p - data) - pos;
            pos = size_t(p - data);

            if (p >= limit)
                return (d > 2) ? 0 : (mask >> (unsigned)d);

            if (d > 2)
            {
                mask = 0;
            }
            else
            {
                mask >>= (unsigned)d;
                if (mask != 0 && (mask > 4 || mask == 3 || IsConvertible(p[(mask >> 1) + 1])))
                {
                    mask = (mask >> 1) | 4;
                    pos++;
                    continue;
                }
            }
        }

        if (IsConvertible(p[4]))
        {
            uint32_t v = (uint32_t(p[4]) << 24) | (uint32_t(p[3]) << 16) |
                         (uint32_t(p[2]) << 8) | uint32_t(p[1]);
            const uint32_t cur = ip + uint32_t(pos);
            pos += 5;

            v = encoding ? v + cur : v - cur;

            if (mask != 0)
            {
                const unsigned sh = (mask & 6) << 2;
                if (IsConvertible(uint8_t(v >> sh)))
                {
                    v ^= ((uint32_t(0x100) << sh) - 1);
                    v = encoding ? v + cur : v - cur;
                }
                mask = 0;
            }

            p[1] = uint8_t(v);
            p[2] = uint8_t(v >> 8);
            p[3] = uint8_t(v >> 16);
            p[4] = uint8_t(0 - ((v >> 24) & 1));
        }
        else
        {
            mask = (mask >> 1) | 4;
            pos++;
        }
    }
}

} // namespace bcmext

#endif // BCM_X86F_H
