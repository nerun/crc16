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

/* engine.c - table engines (normal MSB-first and reflected LSB-first):
 * Sarwate byte-wise and slicing-by-8, plus the bitwise reference. */
#include "crc16.h"

static unsigned reflect8(unsigned v)
{
    unsigned r = 0;
    int i;
    for (i = 0; i < 8; i++) {
        r = (r << 1) | (v & 1u);
        v >>= 1;
    }
    return r;
}

uint16_t crc16_reflect16(uint16_t v)
{
    unsigned r = 0;
    int i;
    for (i = 0; i < 16; i++) {
        r = (r << 1) | (v & 1u);
        v = (uint16_t)(v >> 1);
    }
    return (uint16_t)r;
}

uint16_t crc16_bitwise(const crc16_variant *v, const void *data, size_t len)
{
    const unsigned char *p = (const unsigned char *)data;
    unsigned reg = v->init;
    size_t i;
    int b;

    for (i = 0; i < len; i++) {
        unsigned byte = v->refin ? reflect8(p[i]) : p[i];
        reg ^= byte << 8;
        for (b = 0; b < 8; b++)
            reg = ((reg & 0x8000u) ? (reg << 1) ^ v->poly : (reg << 1)) & 0xFFFFu;
    }
    if (v->refout != 0) reg = crc16_reflect16((uint16_t)reg);
    return (uint16_t)(reg ^ v->xorout);
}

void crc16_engine_init(crc16_engine *e, uint16_t poly, int reflected)
{
    unsigned i, k;
    int b;
    const unsigned rpoly = crc16_reflect16(poly);

    e->poly = poly;
    e->reflected = reflected;
    e->simd = 0;
    for (i = 0; i < 256; i++) {
        unsigned c;
        if (reflected) {
            c = i;
            for (b = 0; b < 8; b++) c = (c & 1u) ? (c >> 1) ^ rpoly : c >> 1;
        } else {
            c = i << 8;
            for (b = 0; b < 8; b++)
                c = ((c & 0x8000u) ? (c << 1) ^ poly : (c << 1)) & 0xFFFFu;
        }
        e->table[0][i] = (uint16_t)c;
    }
    /* Slicing tables (technique from Mark Adler's crc32 / crcspeed): each
     * level appends one more zero byte to the previous one. Built purely
     * from table[0], so no endianness dependence. */
    for (k = 1; k < 8; k++)
        for (i = 0; i < 256; i++) {
            unsigned prev = e->table[k - 1][i];
            e->table[k][i] = reflected
                ? (uint16_t)((prev >> 8) ^ e->table[0][prev & 0xFFu])
                : (uint16_t)(((prev << 8) & 0xFFFFu) ^ e->table[0][prev >> 8]);
        }
    if (crc16_simd_available()) {
        crc16_simd_setup(e);
        e->simd = 1;
    }
}

uint16_t crc16_engine_update_bytewise(const crc16_engine *e, uint16_t reg,
                                      const void *data, size_t len)
{
    const unsigned char *p = (const unsigned char *)data;
    const uint16_t *t = e->table[0];
    unsigned r = reg;
    size_t i;

    if (e->reflected) {
        for (i = 0; i < len; i++) r = (r >> 8) ^ t[(r ^ p[i]) & 0xFFu];
    } else {
        for (i = 0; i < len; i++) r = ((r << 8) & 0xFFFFu) ^ t[((r >> 8) ^ p[i]) & 0xFFu];
    }
    return (uint16_t)r;
}

/*
 * 8-byte word loads. The fast path copies with memcpy (no aliasing or
 * alignment UB) and fixes the byte order with shifts, which compilers turn
 * into a plain load or a bswap. Define CRC16_GENERIC_LOAD to force the
 * endian-independent byte-by-byte assembly (used for big-endian testing).
 */
#if !defined(CRC16_GENERIC_LOAD) && defined(__BYTE_ORDER__) && \
    defined(__ORDER_LITTLE_ENDIAN__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
#include <string.h>

static inline uint64_t load_le64(const unsigned char *p)
{
    uint64_t w;
    memcpy(&w, p, sizeof w);
    return w;
}

static inline uint64_t load_be64(const unsigned char *p)
{
    uint64_t w = load_le64(p);
    w = ((w & 0x00FF00FF00FF00FFull) << 8) | ((w >> 8) & 0x00FF00FF00FF00FFull);
    w = ((w & 0x0000FFFF0000FFFFull) << 16) | ((w >> 16) & 0x0000FFFF0000FFFFull);
    return (w << 32) | (w >> 32);
}
#else
static inline uint64_t load_le64(const unsigned char *p)
{
    return (uint64_t)p[0] | (uint64_t)p[1] << 8 | (uint64_t)p[2] << 16 |
           (uint64_t)p[3] << 24 | (uint64_t)p[4] << 32 | (uint64_t)p[5] << 40 |
           (uint64_t)p[6] << 48 | (uint64_t)p[7] << 56;
}

static inline uint64_t load_be64(const unsigned char *p)
{
    return (uint64_t)p[7] | (uint64_t)p[6] << 8 | (uint64_t)p[5] << 16 |
           (uint64_t)p[4] << 24 | (uint64_t)p[3] << 32 | (uint64_t)p[2] << 40 |
           (uint64_t)p[1] << 48 | (uint64_t)p[0] << 56;
}
#endif

uint16_t crc16_engine_update(const crc16_engine *e, uint16_t reg,
                             const void *data, size_t len)
{
    if (e->simd && len >= CRC16_SIMD_MIN_LEN) return crc16_simd_update(e, reg, data, len);
    return crc16_engine_update_slicing(e, reg, data, len);
}

uint16_t crc16_engine_update_slicing(const crc16_engine *e, uint16_t reg,
                                     const void *data, size_t len)
{
    const unsigned char *p = (const unsigned char *)data;
    const uint16_t(*t)[256] = e->table;
    unsigned r = reg;

    if (e->reflected) {
        while (len >= 8) {
            uint64_t w = load_le64(p) ^ r;
            r = (unsigned)t[7][w & 0xFFu] ^ t[6][(w >> 8) & 0xFFu] ^
                t[5][(w >> 16) & 0xFFu] ^ t[4][(w >> 24) & 0xFFu] ^
                t[3][(w >> 32) & 0xFFu] ^ t[2][(w >> 40) & 0xFFu] ^
                t[1][(w >> 48) & 0xFFu] ^ t[0][w >> 56];
            p += 8;
            len -= 8;
        }
    } else {
        while (len >= 8) {
            uint64_t w = load_be64(p) ^ ((uint64_t)r << 48);
            r = (unsigned)t[7][w >> 56] ^ t[6][(w >> 48) & 0xFFu] ^
                t[5][(w >> 40) & 0xFFu] ^ t[4][(w >> 32) & 0xFFu] ^
                t[3][(w >> 24) & 0xFFu] ^ t[2][(w >> 16) & 0xFFu] ^
                t[1][(w >> 8) & 0xFFu] ^ t[0][w & 0xFFu];
            p += 8;
            len -= 8;
        }
    }
    return crc16_engine_update_bytewise(e, (uint16_t)r, p, len);
}
