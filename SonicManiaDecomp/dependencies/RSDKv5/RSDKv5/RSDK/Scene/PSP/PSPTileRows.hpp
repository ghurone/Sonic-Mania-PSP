#ifndef RSDK_PSP_TILE_ROWS_HPP
#define RSDK_PSP_TILE_ROWS_HPP

#include <stdint.h>

namespace RSDK {
// Geometry flags only: never store converted colors or palette versions.
// Tile pixels are 16-byte aligned in the PSP engine.
inline void PSPClassifyTileRows(const uint8_t *pixels, uint16_t &empty, uint16_t &opaque, uint16_t &solid)
{
    empty = opaque = solid = 0;
    const uint32_t *words = (const uint32_t *)pixels;
    for (unsigned row = 0; row < 16; ++row, words += 4) {
        uint32_t a = words[0], b = words[1], c = words[2], d = words[3];
        if (!(a | b | c | d)) {
            empty |= 1U << row;
        }
        else {
            uint32_t zeros = ((a - 0x01010101U) & ~a) | ((b - 0x01010101U) & ~b)
                           | ((c - 0x01010101U) & ~c) | ((d - 0x01010101U) & ~d);
            if (!(zeros & 0x80808080U)) {
                opaque |= 1U << row;
                // A word equal to its byte rotation has four equal indices.
                if (a == b && a == c && a == d && a == ((a >> 8) | (a << 24)))
                    solid |= 1U << row;
            }
        }
    }
}

__attribute__((always_inline)) inline void PSPFillSolidTileRow(uint16_t *__restrict__ dst, uint16_t color)
{
    uint32_t pair = (uint32_t)color | ((uint32_t)color << 16);
    if (!((uintptr_t)dst & 3)) {
        uint32_t *out = (uint32_t *)dst;
        out[0] = pair; out[1] = pair; out[2] = pair; out[3] = pair;
        out[4] = pair; out[5] = pair; out[6] = pair; out[7] = pair;
    }
    else {
        // Odd scroll: keep word stores aligned, with a halfword at each edge.
        dst[0] = color;
        dst[15] = color;
        uint32_t *out = (uint32_t *)(dst + 1);
        out[0] = pair; out[1] = pair; out[2] = pair; out[3] = pair;
        out[4] = pair; out[5] = pair; out[6] = pair;
    }
}

__attribute__((always_inline)) inline void PSPDrawOpaqueTileRow(uint16_t *__restrict__ dst,
                                                              const uint8_t *__restrict__ pixels,
                                                              const uint16_t *__restrict__ palette)
{
    // Independent palette lookups before each group of stores. These buffers
    // never overlap; restrict lets the compiler schedule the loads accordingly.
    uint16_t a = palette[pixels[0]], b = palette[pixels[1]], c = palette[pixels[2]], d = palette[pixels[3]];
    dst[0] = a; dst[1] = b; dst[2] = c; dst[3] = d;
    a = palette[pixels[4]]; b = palette[pixels[5]]; c = palette[pixels[6]]; d = palette[pixels[7]];
    dst[4] = a; dst[5] = b; dst[6] = c; dst[7] = d;
    a = palette[pixels[8]]; b = palette[pixels[9]]; c = palette[pixels[10]]; d = palette[pixels[11]];
    dst[8] = a; dst[9] = b; dst[10] = c; dst[11] = d;
    a = palette[pixels[12]]; b = palette[pixels[13]]; c = palette[pixels[14]]; d = palette[pixels[15]];
    dst[12] = a; dst[13] = b; dst[14] = c; dst[15] = d;
}

// Textured rows use two temporaries, keeping the layer loop's live values out
// of the wide palette-gather path. Each byte/color load is separated from its
// use by the independent pixel beside it. Halfword stores support odd scroll.
__attribute__((always_inline)) inline void PSPDrawScheduledOpaqueTileRow(uint16_t *dst, const uint8_t *pixels,
                                                                       const uint16_t *palette)
{
#if defined(__mips__)
#define PSP_OPAQUE_PAIR(p0, p1, d0, d1) \
    "lbu %[a], " #p0 "(%[pixels])\n\t" \
    "lbu %[b], " #p1 "(%[pixels])\n\t" \
    "sll %[a], %[a], 1\n\t" \
    "sll %[b], %[b], 1\n\t" \
    "addu %[a], %[palette], %[a]\n\t" \
    "addu %[b], %[palette], %[b]\n\t" \
    "lhu %[a], 0(%[a])\n\t" \
    "lhu %[b], 0(%[b])\n\t" \
    "sh %[a], " #d0 "(%[dst])\n\t" \
    "sh %[b], " #d1 "(%[dst])\n\t"
    uint32_t a, b;
    __asm__ volatile(
        ".set push\n\t.set noreorder\n\t"
        PSP_OPAQUE_PAIR(0, 1, 0, 2)
        PSP_OPAQUE_PAIR(2, 3, 4, 6)
        PSP_OPAQUE_PAIR(4, 5, 8, 10)
        PSP_OPAQUE_PAIR(6, 7, 12, 14)
        PSP_OPAQUE_PAIR(8, 9, 16, 18)
        PSP_OPAQUE_PAIR(10, 11, 20, 22)
        PSP_OPAQUE_PAIR(12, 13, 24, 26)
        PSP_OPAQUE_PAIR(14, 15, 28, 30)
        ".set pop\n\t"
        : [a] "=&r"(a), [b] "=&r"(b)
        : [dst] "r"(dst), [pixels] "r"(pixels), [palette] "r"(palette)
        : "memory");
#undef PSP_OPAQUE_PAIR
#else
    // Portable reference for differential checks; PSP uses the scheduled code.
    for (unsigned i = 0; i < 16; ++i)
        dst[i] = palette[pixels[i]];
#endif
}

__attribute__((always_inline)) inline void PSPDrawMixedTileRow(uint16_t *dst, const uint8_t *pixels, const uint16_t *palette)
{
    // Full tile rows are aligned. Skip a transparent half with two word loads,
    // preserving the destination and avoiding eight byte tests. No new table.
    const uint32_t *words = (const uint32_t *)pixels;
#define PSP_MIXED_PIXEL(i) do { uint8_t index = pixels[i]; if (index) dst[i] = palette[index]; } while (0)
    if (words[0] | words[1]) {
        PSP_MIXED_PIXEL(0); PSP_MIXED_PIXEL(1); PSP_MIXED_PIXEL(2); PSP_MIXED_PIXEL(3);
        PSP_MIXED_PIXEL(4); PSP_MIXED_PIXEL(5); PSP_MIXED_PIXEL(6); PSP_MIXED_PIXEL(7);
    }
    if (words[2] | words[3]) {
        PSP_MIXED_PIXEL(8); PSP_MIXED_PIXEL(9); PSP_MIXED_PIXEL(10); PSP_MIXED_PIXEL(11);
        PSP_MIXED_PIXEL(12); PSP_MIXED_PIXEL(13); PSP_MIXED_PIXEL(14); PSP_MIXED_PIXEL(15);
    }
#undef PSP_MIXED_PIXEL
}
} // namespace RSDK

#endif
