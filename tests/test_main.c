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

/* test_main.c - unit tests: check values, empty input, chunking, grouping,
 * GF(2) arithmetic and name lookup. */
#include "crc16.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failures;
static int g_checks;

#define CHECK(cond)                                                              \
    do {                                                                         \
        g_checks++;                                                              \
        if (!(cond)) {                                                           \
            g_failures++;                                                        \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);      \
        }                                                                        \
    } while (0)

static uint64_t rng_state = 0x9E3779B97F4A7C15ull;

static uint32_t rnd(void) /* xorshift64* */
{
    rng_state ^= rng_state >> 12;
    rng_state ^= rng_state << 25;
    rng_state ^= rng_state >> 27;
    return (uint32_t)((rng_state * 0x2545F4914F6CDD1Dull) >> 32);
}

static void fill(unsigned char *p, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) p[i] = (unsigned char)rnd();
}

/* Ungrouped table-driven result for one variant (phase 1 style). */
static uint16_t table_one(const crc16_variant *v, const void *data, size_t len)
{
    crc16_engine e;
    uint16_t reg = v->refin ? crc16_reflect16(v->init) : v->init;
    crc16_engine_init(&e, v->poly, v->refin);
    reg = crc16_engine_update_bytewise(&e, reg, data, len);
    return (uint16_t)(reg ^ v->xorout);
}

static void ctx_all(crc16_ctx *ctx, size_t *ids)
{
    size_t i;
    for (i = 0; i < crc16_variant_count; i++) ids[i] = i;
    CHECK(crc16_ctx_init(ctx, ids, crc16_variant_count) == 0);
}

static void test_check_values(void)
{
    static const char s[] = "123456789";
    static crc16_ctx ctx;
    size_t ids[CRC16_MAX_VARIANTS], i;
    uint16_t res[CRC16_MAX_VARIANTS];

    CHECK(crc16_variant_count == 31);
    ctx_all(&ctx, ids);
    crc16_ctx_update(&ctx, s, 9);
    crc16_ctx_final(&ctx, res);
    for (i = 0; i < crc16_variant_count; i++) {
        const crc16_variant *v = &crc16_variants[i];
        if (crc16_bitwise(v, s, 9) != v->check) fprintf(stderr, "bitwise %s\n", v->name);
        if (table_one(v, s, 9) != v->check) fprintf(stderr, "table %s\n", v->name);
        if (res[i] != v->check) fprintf(stderr, "ctx %s\n", v->name);
        CHECK(crc16_bitwise(v, s, 9) == v->check);
        CHECK(table_one(v, s, 9) == v->check);
        CHECK(res[i] == v->check);
    }
    CHECK(ctx.ngroups == 15);
}

static void test_empty(void)
{
    static crc16_ctx ctx;
    size_t ids[CRC16_MAX_VARIANTS], i;
    uint16_t res[CRC16_MAX_VARIANTS];

    ctx_all(&ctx, ids);
    crc16_ctx_final(&ctx, res);
    for (i = 0; i < crc16_variant_count; i++) {
        const crc16_variant *v = &crc16_variants[i];
        uint16_t expect = (uint16_t)((v->refin ? crc16_reflect16(v->init) : v->init) ^ v->xorout);
        CHECK(res[i] == expect);
        CHECK(crc16_bitwise(v, "", 0) == expect);
    }
}

static void test_table_vs_bitwise(void)
{
    unsigned char buf[512];
    size_t i, len;

    fill(buf, sizeof buf);
    for (i = 0; i < crc16_variant_count; i++)
        for (len = 0; len <= sizeof buf; len += (len < 40 ? 1 : 37))
            CHECK(table_one(&crc16_variants[i], buf, len) ==
                  crc16_bitwise(&crc16_variants[i], buf, len));
}

/* Slicing-by-8 must equal Sarwate for every alignment and length. */
static void test_slicing(void)
{
    enum { BIG = 70000 };
    static unsigned char buf[BIG + 16];
    size_t i, off, len;

    fill(buf, sizeof buf);
    for (i = 0; i < crc16_variant_count; i++) {
        const crc16_variant *v = &crc16_variants[i];
        crc16_engine e;
        crc16_engine_init(&e, v->poly, v->refin);
        CHECK(e.simd == crc16_simd_available());
        for (off = 0; off < 16; off++) {
            for (len = 0; len <= 300; len++) {
                uint16_t init = (uint16_t)rnd();
                CHECK(crc16_engine_update(&e, init, buf + off, len) ==
                      crc16_engine_update_bytewise(&e, init, buf + off, len));
            }
            CHECK(crc16_engine_update(&e, v->init, buf + off, BIG) ==
                  crc16_engine_update_bytewise(&e, v->init, buf + off, BIG));
            CHECK(crc16_engine_update(&e, 0xFFFF, buf + off, BIG - 1) ==
                  crc16_engine_update_bytewise(&e, 0xFFFF, buf + off, BIG - 1));
        }
    }
}

/* Pool results must equal the serial path for any size, piece size and
 * thread count, also when begin/end calls are chained. */
static void test_parallel(void)
{
    enum { N = 300000 };
    static unsigned char buf[N];
    static crc16_ctx serial, par;
    size_t ids[CRC16_MAX_VARIANTS], i;
    uint16_t want[CRC16_MAX_VARIANTS], got[CRC16_MAX_VARIANTS];
    static const size_t pieces[] = {1, 7, 100, 4096, 32768, 100000, 1000000};
    static const unsigned threads[] = {2, 3, 8};
    static const size_t lens[] = {0, 1, 99, 100, 101, 5000, 65537, 299999, N};
    size_t pi, li, ti;

    fill(buf, N);
    if (!crc16_threads_supported()) {
        CHECK(crc16_pool_create(4) == NULL);
        return;
    }
    CHECK(crc16_pool_create(1) == NULL);
    for (ti = 0; ti < sizeof threads / sizeof threads[0]; ti++) {
        crc16_pool *pool = crc16_pool_create(threads[ti]);
        CHECK(pool != NULL);
        if (pool == NULL) continue;
        for (pi = 0; pi < sizeof pieces / sizeof pieces[0]; pi++) {
            crc16_pool_set_piece_size(pool, pieces[pi]);
            for (li = 0; li < sizeof lens / sizeof lens[0]; li++) {
                size_t half = lens[li] / 2;
                if (pieces[pi] < 100 && lens[li] > 5000) continue; /* keep the test fast */
                ctx_all(&serial, ids);
                crc16_ctx_update(&serial, buf, lens[li]);
                crc16_ctx_final(&serial, want);
                /* two chained parallel updates over the two halves */
                ctx_all(&par, ids);
                crc16_ctx_update_begin(&par, pool, buf, half);
                crc16_ctx_update_end(&par, pool);
                crc16_ctx_update_begin(&par, pool, buf + half, lens[li] - half);
                crc16_ctx_update_end(&par, pool);
                crc16_ctx_final(&par, got);
                CHECK(par.length == lens[li]);
                for (i = 0; i < crc16_variant_count; i++) CHECK(got[i] == want[i]);
            }
        }
        crc16_pool_destroy(pool);
    }
    /* A single-group selection must also parallelise correctly. */
    {
        crc16_pool *pool = crc16_pool_create(4);
        ids[0] = crc16_find_variant("ARC");
        crc16_pool_set_piece_size(pool, 1000);
        CHECK(crc16_ctx_init(&par, ids, 1) == 0);
        crc16_ctx_update_begin(&par, pool, buf, N);
        crc16_ctx_update_end(&par, pool);
        crc16_ctx_final(&par, got);
        CHECK(got[0] == crc16_bitwise(&crc16_variants[ids[0]], buf, N));
        crc16_pool_destroy(pool);
    }
    /* NULL pool: _begin is synchronous and _end a no-op. */
    ctx_all(&par, ids);
    crc16_ctx_update_begin(&par, NULL, buf, 1234);
    crc16_ctx_update_end(&par, NULL);
    ctx_all(&serial, ids);
    crc16_ctx_update(&serial, buf, 1234);
    crc16_ctx_final(&par, got);
    crc16_ctx_final(&serial, want);
    for (i = 0; i < crc16_variant_count; i++) CHECK(got[i] == want[i]);
}

/* The SIMD path, when present, must equal slicing-by-8 on the same engine. */
static void test_simd_vs_slicing(void)
{
    static unsigned char buf[1 << 20];
    size_t i, len;

    fill(buf, sizeof buf);
    printf("simd available: %d\n", crc16_simd_available());
    for (i = 0; i < crc16_variant_count; i++) {
        const crc16_variant *v = &crc16_variants[i];
        crc16_engine e;
        crc16_engine_init(&e, v->poly, v->refin);
        for (len = 0; len < 2000; len += (len < 600 ? 1 : 97)) {
            uint16_t init = (uint16_t)rnd();
            CHECK(crc16_simd_available() == 0 || len < CRC16_SIMD_MIN_LEN ||
                  crc16_simd_update(&e, init, buf + (len & 15), len) ==
                      crc16_engine_update_slicing(&e, init, buf + (len & 15), len));
        }
        for (len = sizeof buf - 70; len <= sizeof buf; len++)
            CHECK(crc16_engine_update(&e, 0xA5A5, buf, len) ==
                  crc16_engine_update_slicing(&e, 0xA5A5, buf, len));
    }
}

static void test_chunk_splits(void)
{
    enum { N = 100000 };
    static unsigned char buf[N];
    static crc16_ctx ctx;
    size_t ids[CRC16_MAX_VARIANTS], i;
    uint16_t res[CRC16_MAX_VARIANTS];
    int round;

    fill(buf, N);
    for (round = 0; round < 20; round++) {
        size_t pos = 0;
        ctx_all(&ctx, ids);
        while (pos < N) {
            size_t n = rnd() % (round < 10 ? 300 : 70000);
            if (n > N - pos) n = N - pos;
            crc16_ctx_update(&ctx, buf + pos, n);
            pos += n;
        }
        CHECK(ctx.length == N);
        crc16_ctx_final(&ctx, res);
        for (i = 0; i < crc16_variant_count; i++)
            CHECK(res[i] == table_one(&crc16_variants[i], buf, N));
    }
    /* Reference on the full buffer for a few variants. */
    for (i = 0; i < crc16_variant_count; i += 5)
        CHECK(res[i] == crc16_bitwise(&crc16_variants[i], buf, N));
}

static void test_grouped_vs_ungrouped(void)
{
    static unsigned char buf[40000];
    static crc16_ctx ctx;
    size_t ids[CRC16_MAX_VARIANTS], i, len;
    uint16_t res[CRC16_MAX_VARIANTS];

    fill(buf, sizeof buf);
    for (len = 0; len < 600; len += (len < 130 ? 1 : 17)) {
        ctx_all(&ctx, ids);
        crc16_ctx_update(&ctx, buf, len);
        crc16_ctx_final(&ctx, res);
        for (i = 0; i < crc16_variant_count; i++)
            CHECK(res[i] == table_one(&crc16_variants[i], buf, len));
    }
    for (len = 32760; len <= sizeof buf; len += 777) { /* around sub-blocks */
        ctx_all(&ctx, ids);
        crc16_ctx_update(&ctx, buf, len);
        crc16_ctx_final(&ctx, res);
        for (i = 0; i < crc16_variant_count; i++)
            CHECK(res[i] == table_one(&crc16_variants[i], buf, len));
    }
}

static void test_subset_and_order(void)
{
    static crc16_ctx ctx;
    size_t ids[3];
    uint16_t res[3];
    static const char s[] = "123456789";

    ids[0] = crc16_find_variant("XMODEM");
    ids[1] = crc16_find_variant("ARC");
    ids[2] = crc16_find_variant("GENIBUS");
    CHECK(crc16_ctx_init(&ctx, ids, 3) == 0);
    CHECK(ctx.ngroups == 2); /* XMODEM and GENIBUS share a group */
    crc16_ctx_update(&ctx, s, 9);
    crc16_ctx_final(&ctx, res);
    CHECK(res[0] == 0x31C3 && res[1] == 0xBB3D && res[2] == 0xD64E);
    /* Reset reuses the tables: a second message must not see the first. */
    crc16_ctx_reset(&ctx);
    CHECK(ctx.length == 0);
    crc16_ctx_update(&ctx, "garbage", 7);
    crc16_ctx_reset(&ctx);
    crc16_ctx_update(&ctx, s, 9);
    crc16_ctx_final(&ctx, res);
    CHECK(res[0] == 0x31C3 && res[1] == 0xBB3D && res[2] == 0xD64E);
    ids[0] = crc16_variant_count;
    CHECK(crc16_ctx_init(&ctx, ids, 1) == -1);
}

/* x^(8n) computed naively by repeated multiplication by x^8. */
static uint16_t xpow_naive(uint64_t n, uint16_t poly, int refl)
{
    uint16_t r = refl ? 0x8000 : 0x0001;
    uint16_t x8 = refl ? 0x0080 : 0x0100;
    while (n-- > 0) r = crc16_mulmod(r, x8, poly, refl);
    return r;
}

static void test_gf2(void)
{
    static const uint16_t polys[] = {0x8005, 0x1021, 0x0589, 0x5935, 0x3D65, 0x080B};
    size_t k;
    int refl;
    uint64_t n;

    for (k = 0; k < sizeof polys / sizeof polys[0]; k++)
        for (refl = 0; refl < 2; refl++) {
            uint16_t p = polys[k];
            uint64_t big = (1ull << 33) + 12345;
            for (n = 0; n < 40; n++) CHECK(crc16_xpow8n(n, p, refl) == xpow_naive(n, p, refl));
            /* Composition law, including lengths beyond 2^32. */
            CHECK(crc16_xpow8n(big, p, refl) ==
                  crc16_mulmod(crc16_xpow8n(1ull << 33, p, refl), crc16_xpow8n(12345, p, refl), p, refl));
            CHECK(crc16_xpow8n(1ull << 33, p, refl) ==
                  crc16_mulmod(crc16_xpow8n(1ull << 32, p, refl), crc16_xpow8n(1ull << 32, p, refl), p, refl));
        }
    CHECK(crc16_reflect16(0x0001) == 0x8000 && crc16_reflect16(0x1234) == 0x2C48);
}

static void test_lookup(void)
{
    size_t i;
    const char *const *a;

    CHECK(crc16_find_variant("arc") == crc16_find_variant("CRC-16/ARC"));
    CHECK(crc16_find_variant("crc-16/x-25") == crc16_find_variant("IBM-SDLC"));
    CHECK(crc16_find_variant("CCITT-FALSE") == crc16_find_variant("ibm-3740"));
    CHECK(crc16_find_variant("nonsense") == CRC16_NOT_FOUND);
    CHECK(crc16_find_variant("") == CRC16_NOT_FOUND);
    CHECK(crc16_find_variant("CRC-16/") == CRC16_NOT_FOUND);
    for (i = 0; i < crc16_variant_count; i++) {
        CHECK(crc16_find_variant(crc16_variants[i].name) == i);
        for (a = crc16_variants[i].aliases; a != NULL && *a != NULL; a++)
            CHECK(crc16_find_variant(*a) == i);
    }
}

int main(void)
{
    test_check_values();
    test_empty();
    test_table_vs_bitwise();
    test_slicing();
    test_simd_vs_slicing();
    test_chunk_splits();
    test_parallel();
    test_grouped_vs_ungrouped();
    test_subset_and_order();
    test_gf2();
    test_lookup();
    printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
