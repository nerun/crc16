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
 * gf2.c - arithmetic in GF(2)[x] modulo a 16-bit polynomial.
 *
 * A CRC register is linear over GF(2):
 *   reg(init, data) = reg(0, data) XOR (init * x^(8*len) mod P)
 * so variants sharing (poly, refin) can share one pass over the data.
 * The x^n mod P by square-and-multiply is the same idea as zlib's
 * crc32_combine.
 */
#include "crc16.h"

static uint16_t mulmod_normal(uint16_t a, uint16_t b, uint16_t poly)
{
    unsigned r = 0;
    int i;
    for (i = 15; i >= 0; i--) { /* Horner over the bits of b */
        unsigned carry = r & 0x8000u;
        r = (r << 1) & 0xFFFFu;
        if (carry) r ^= poly;
        if (((unsigned)b >> i) & 1u) r ^= a;
    }
    return (uint16_t)r;
}

uint16_t crc16_mulmod(uint16_t a, uint16_t b, uint16_t poly, int reflected)
{
    if (!reflected) return mulmod_normal(a, b, poly);
    return crc16_reflect16(mulmod_normal(crc16_reflect16(a), crc16_reflect16(b), poly));
}

uint16_t crc16_xpow_bits(uint64_t bits, uint16_t poly)
{
    uint16_t result = 0x0001; /* x^0 */
    uint16_t base = 0x0002;   /* x^1 */

    while (bits != 0) {
        if (bits & 1u) result = mulmod_normal(result, base, poly);
        base = mulmod_normal(base, base, poly);
        bits >>= 1;
    }
    return result;
}

uint16_t crc16_xpow8n(uint64_t nbytes, uint16_t poly, int reflected)
{
    uint16_t result = 0x0001; /* x^0 */
    uint16_t base = 0x0100;   /* x^8, already reduced (degree < 16) */

    while (nbytes != 0) {
        if (nbytes & 1u) result = mulmod_normal(result, base, poly);
        base = mulmod_normal(base, base, poly);
        nbytes >>= 1;
    }
    return reflected ? crc16_reflect16(result) : result;
}
