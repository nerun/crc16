/* SPDX-License-Identifier: GPL-2.0-or-later
 * crc16 - compute many CRC-16 variants of a file in a single pass
 * Copyright (C) 2026 Daniel Dias Rodrigues <danieldiasr@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, see <https://www.gnu.org/licenses/>.
 */

/*
 * simd_x86.c - CRC-16 by carry-less multiply folding (x86-64 PCLMULQDQ).
 *
 * Technique: Intel, "Fast CRC Computation for Generic Polynomials Using
 * PCLMULQDQ Instruction", with a simplification: instead of a Barrett
 * reduction, the final 128-bit accumulator is fed (as 16 stream-order
 * bytes, register 0) to the table engine, which is already validated.
 *
 * Invariant. A CRC from register 0 depends on the message polynomial M(x)
 * only modulo P. Keep a 128-bit accumulator A congruent to everything
 * consumed so far; to append a 128-bit block B at distance D bits:
 *     A' = A * x^D + B  (mod P)
 * Splitting A = H*x^64 + L gives  A * x^D = H*x^(D+64) + L*x^D, and each
 * x^k mod P is a 16-bit constant, so each half needs one 64x16 carry-less
 * product (PCLMULQDQ) and the result stays below 128 bits.
 *
 * Normal (MSB-first) data: bytes are reversed (pshufb) so the register is
 * a plain integer with bit i = coefficient of x^i; constants are
 * (x^(D+64), x^D).
 * Reflected (LSB-first) data: memory order is used as is; bit j of the
 * register is the coefficient of x^(127-j), so the product lands 49 bits
 * lower than the target position and the constants are pre-multiplied by
 * x^-49: (x^(D-49) for the low-degree half in lane 1, x^(D+15) for the
 * high-degree half in lane 0), stored bit-reversed.
 *
 * Four independent accumulators (512-bit distance) hide the latency of the
 * multiplies; they are merged with three 128-bit folds. Any arrangement is
 * verified against the slicing engine by the unit tests.
 */
#include "crc16.h"

#if !defined(CRC16_NO_SIMD) && defined(__x86_64__) && (defined(__GNUC__) || defined(__clang__))
#define CRC16_SIMD_X86 1
#include <immintrin.h>
#endif

#ifndef CRC16_SIMD_X86

int crc16_simd_available(void) { return 0; }
void crc16_simd_setup(crc16_engine *e) { (void)e; }

uint16_t crc16_simd_update(const crc16_engine *e, uint16_t reg, const void *data, size_t len)
{
    return crc16_engine_update_slicing(e, reg, data, len);
}

#else

#define TARGET __attribute__((target("pclmul,sse4.1")))
#define INLINE static inline __attribute__((always_inline))

int crc16_simd_available(void)
{
    static int cached = -1;
    if (cached < 0) {
        __builtin_cpu_init();
        cached = __builtin_cpu_supports("pclmul") && __builtin_cpu_supports("sse4.1");
    }
    return cached;
}

/* Fold constants for distance D bits: lane 0 multiplies the low 64-bit half. */
static void fold_constants(uint64_t out[2], uint16_t poly, int reflected, unsigned d)
{
    if (reflected) {
        out[0] = crc16_reflect16(crc16_xpow_bits((uint64_t)d + 15, poly));
        out[1] = crc16_reflect16(crc16_xpow_bits((uint64_t)d - 49, poly));
    } else {
        out[0] = crc16_xpow_bits(d, poly);
        out[1] = crc16_xpow_bits((uint64_t)d + 64, poly);
    }
}

void crc16_simd_setup(crc16_engine *e)
{
    fold_constants(e->fold[0], e->poly, e->reflected, 128);
    fold_constants(e->fold[1], e->poly, e->reflected, 512);
}

TARGET INLINE __m128i load_block(const unsigned char *p, int swap, __m128i swap_mask)
{
    __m128i v = _mm_loadu_si128((const __m128i *)(const void *)p);
    return swap ? _mm_shuffle_epi8(v, swap_mask) : v;
}

TARGET INLINE __m128i fold_step(__m128i acc, __m128i k, __m128i next)
{
    __m128i lo = _mm_clmulepi64_si128(acc, k, 0x00);
    __m128i hi = _mm_clmulepi64_si128(acc, k, 0x11);
    return _mm_xor_si128(_mm_xor_si128(lo, hi), next);
}

TARGET INLINE uint16_t run(const crc16_engine *e, uint16_t reg, const unsigned char *p, size_t len,
                           int swap)
{
    const __m128i swap_mask = _mm_set_epi8(0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15);
    const __m128i k128 = _mm_set_epi64x((long long)e->fold[0][1], (long long)e->fold[0][0]);
    const __m128i k512 = _mm_set_epi64x((long long)e->fold[1][1], (long long)e->fold[1][0]);
    __m128i x0, x1, x2, x3, regv;
    unsigned char tmp[16];

    /* The incoming register is XORed into the first two message bytes. */
    regv = swap ? _mm_set_epi64x((long long)((uint64_t)reg << 48), 0) : _mm_cvtsi32_si128(reg);

    x0 = _mm_xor_si128(load_block(p, swap, swap_mask), regv);
    x1 = load_block(p + 16, swap, swap_mask);
    x2 = load_block(p + 32, swap, swap_mask);
    x3 = load_block(p + 48, swap, swap_mask);
    p += 64;
    len -= 64;
    while (len >= 64) {
        x0 = fold_step(x0, k512, load_block(p, swap, swap_mask));
        x1 = fold_step(x1, k512, load_block(p + 16, swap, swap_mask));
        x2 = fold_step(x2, k512, load_block(p + 32, swap, swap_mask));
        x3 = fold_step(x3, k512, load_block(p + 48, swap, swap_mask));
        p += 64;
        len -= 64;
    }
    x0 = fold_step(x0, k128, x1);
    x0 = fold_step(x0, k128, x2);
    x0 = fold_step(x0, k128, x3);
    while (len >= 16) {
        x0 = fold_step(x0, k128, load_block(p, swap, swap_mask));
        p += 16;
        len -= 16;
    }

    if (swap) x0 = _mm_shuffle_epi8(x0, swap_mask); /* back to stream order */
    _mm_storeu_si128((__m128i *)(void *)tmp, x0);
    reg = crc16_engine_update_slicing(e, 0, tmp, 16);
    return crc16_engine_update_slicing(e, reg, p, len);
}

TARGET static uint16_t run_normal(const crc16_engine *e, uint16_t reg, const unsigned char *p,
                                  size_t len)
{
    return run(e, reg, p, len, 1);
}

TARGET static uint16_t run_reflected(const crc16_engine *e, uint16_t reg, const unsigned char *p,
                                     size_t len)
{
    return run(e, reg, p, len, 0);
}

uint16_t crc16_simd_update(const crc16_engine *e, uint16_t reg, const void *data, size_t len)
{
    const unsigned char *p = (const unsigned char *)data;
    return e->reflected ? run_reflected(e, reg, p, len) : run_normal(e, reg, p, len);
}

#endif /* CRC16_SIMD_X86 */
