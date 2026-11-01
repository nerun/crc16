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
 * crc16.h - public types and API of the multi-variant CRC-16 calculator.
 *
 * Model: Rocksoft/RevEng (width=16, poly, init, refin, refout, xorout) with
 * poly and init in normal form. See https://reveng.sourceforge.io/crc-catalogue/16.htm
 */
#ifndef CRC16_H
#define CRC16_H

#include <stddef.h>
#include <stdint.h>

#define CRC16_VERSION "1.0.0"
#define CRC16_MIN_ISO_C "C11 (C99 when built with -DCRC16_NO_THREADS)"

#define CRC16_NOT_FOUND ((size_t)-1)
#define CRC16_MAX_VARIANTS 64u
#define CRC16_SUBBLOCK 32768u /* bytes run through every group before rotating */

/* ---- variant table (variants.c) ---- */

typedef struct crc16_variant {
    const char *name;           /* full name, e.g. "CRC-16/ARC" */
    const char *const *aliases; /* NULL-terminated list, or NULL */
    uint16_t poly;
    uint16_t init;
    uint16_t xorout;
    uint16_t check; /* CRC of the ASCII string "123456789" */
    int refin;
    int refout;
} crc16_variant;

extern const crc16_variant crc16_variants[];
extern const size_t crc16_variant_count;

/* Case-insensitive lookup by name or alias, with or without "CRC-16/". */
size_t crc16_find_variant(const char *name);

/* ---- helpers and bitwise reference (gf2.c / engine.c) ---- */

uint16_t crc16_reflect16(uint16_t v);

/* Generic bit-by-bit reference; handles refin != refout. For tests only. */
uint16_t crc16_bitwise(const crc16_variant *v, const void *data, size_t len);

/* ---- table-driven engines (engine.c) ---- */

typedef struct crc16_engine {
    uint16_t poly; /* normal form */
    int reflected; /* LSB-first engine */
    /* table[0] is the classic Sarwate table; table[k][b] is the register
     * effect of byte b followed by k zero bytes (slicing-by-8). */
    uint16_t table[8][256];
    /* Carry-less multiply folding (simd_x86.c); simd == 0 uses slicing only.
     * Tests may clear `simd` after init to compare both paths. */
    int simd;
    uint64_t fold[2][2]; /* constants for 128-bit and 512-bit fold distances */
} crc16_engine;

void crc16_engine_init(crc16_engine *e, uint16_t poly, int reflected);

/* Advances a raw register (reflected domain when e->reflected).
 * Slicing-by-8 for the bulk, Sarwate for the head/tail bytes. */
uint16_t crc16_engine_update(const crc16_engine *e, uint16_t reg,
                             const void *data, size_t len);

/* Slicing-by-8 only (never SIMD); the SIMD code uses it for head/tail bytes. */
uint16_t crc16_engine_update_slicing(const crc16_engine *e, uint16_t reg,
                                     const void *data, size_t len);

/* Byte-at-a-time Sarwate update; reference for the tests. */
uint16_t crc16_engine_update_bytewise(const crc16_engine *e, uint16_t reg,
                                      const void *data, size_t len);

/* ---- GF(2)[x] arithmetic modulo the polynomial (gf2.c) ---- */

/* a*b mod P; operands/result use the register representation of the domain. */
uint16_t crc16_mulmod(uint16_t a, uint16_t b, uint16_t poly, int reflected);

/* x^bits mod P in normal representation (bit i = coefficient of x^i). */
uint16_t crc16_xpow_bits(uint64_t bits, uint16_t poly);

/* x^(8*nbytes) mod P in the register representation of the domain. */
uint16_t crc16_xpow8n(uint64_t nbytes, uint16_t poly, int reflected);

/* ---- carry-less multiply folding (simd_x86.c) ---- */

#define CRC16_SIMD_MIN_LEN 64u /* shorter inputs always use slicing-by-8 */

/* 1 when this build and CPU support the SIMD path (x86-64 PCLMULQDQ). */
int crc16_simd_available(void);
/* Computes e->fold from e->poly; call after the tables are built. */
void crc16_simd_setup(crc16_engine *e);
/* len must be >= CRC16_SIMD_MIN_LEN. */
uint16_t crc16_simd_update(const crc16_engine *e, uint16_t reg, const void *data, size_t len);

/* ---- streaming multi-variant context (multi.c) ---- */

typedef struct crc16_group {
    crc16_engine engine;
    uint16_t reg; /* register started from zero */
} crc16_group;

typedef struct crc16_member {
    size_t variant; /* index into crc16_variants */
    size_t group;   /* index into ctx->groups */
} crc16_member;

typedef struct crc16_ctx {
    size_t ngroups;
    size_t nmembers;
    uint64_t length;
    crc16_group groups[CRC16_MAX_VARIANTS];
    crc16_member members[CRC16_MAX_VARIANTS];
} crc16_ctx;

/* Returns 0 on success, -1 on invalid arguments. */
int crc16_ctx_init(crc16_ctx *ctx, const size_t *variant_ids, size_t n);
/* Starts a new message with the same selection; keeps the built tables. */
void crc16_ctx_reset(crc16_ctx *ctx);
void crc16_ctx_update(crc16_ctx *ctx, const void *data, size_t len);
/* Advances caller-owned group registers (regs[ngroups]) over data without
 * touching the context; building block of the parallel path. */
void crc16_ctx_advance(const crc16_ctx *ctx, uint16_t *regs, const void *data, size_t len);
/* results[i] corresponds to variant_ids[i] given to crc16_ctx_init. */
void crc16_ctx_final(const crc16_ctx *ctx, uint16_t *results);

/* ---- optional multithreading (parallel.c, C11 <threads.h>) ---- */

typedef struct crc16_pool crc16_pool;

/* 1 when built with thread support, 0 otherwise (pool calls become no-ops). */
int crc16_threads_supported(void);
/* Number of online CPUs when it can be detected, else 1. */
unsigned crc16_default_threads(void);
/* Creates nthreads workers; returns NULL when nthreads < 2, threads are
 * unsupported, or no thread could be started (callers then run serially). */
crc16_pool *crc16_pool_create(unsigned nthreads);
void crc16_pool_destroy(crc16_pool *pool);
/* Bytes handled per task (tuning/testing only); must be called while idle. */
void crc16_pool_set_piece_size(crc16_pool *pool, size_t bytes);

/*
 * Same result as crc16_ctx_update, computed by the pool. The buffer must
 * stay untouched until the matching _end call, so the caller can overlap
 * I/O with the computation. With pool == NULL, _begin does the work
 * synchronously and _end is a no-op.
 */
void crc16_ctx_update_begin(crc16_ctx *ctx, crc16_pool *pool, const void *data, size_t len);
void crc16_ctx_update_end(crc16_ctx *ctx, crc16_pool *pool);

#endif /* CRC16_H */
