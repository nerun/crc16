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
 * multi.c - streaming context that computes many variants in one pass.
 *
 * Groups are derived at init time from the variant table: variants with
 * the same (poly, refin) share one register that starts at zero; each
 * member is fixed up at the end using the linearity of the CRC.
 * Every supported variant has refin == refout, so no output reflection is
 * needed: result = register XOR xorout.
 */
#include "crc16.h"

int crc16_ctx_init(crc16_ctx *ctx, const size_t *variant_ids, size_t n)
{
    size_t i, g;

    if (n > CRC16_MAX_VARIANTS) return -1;
    ctx->ngroups = 0;
    ctx->nmembers = 0;
    ctx->length = 0;

    for (i = 0; i < n; i++) {
        const crc16_variant *v;
        if (variant_ids[i] >= crc16_variant_count) return -1;
        v = &crc16_variants[variant_ids[i]];
        if (v->refin != v->refout) return -1; /* unsupported by the fast path */

        for (g = 0; g < ctx->ngroups; g++)
            if (ctx->groups[g].engine.poly == v->poly &&
                ctx->groups[g].engine.reflected == v->refin)
                break;
        if (g == ctx->ngroups) {
            crc16_engine_init(&ctx->groups[g].engine, v->poly, v->refin);
            ctx->groups[g].reg = 0;
            ctx->ngroups++;
        }
        ctx->members[i].variant = variant_ids[i];
        ctx->members[i].group = g;
        ctx->nmembers++;
    }
    return 0;
}

void crc16_ctx_reset(crc16_ctx *ctx)
{
    size_t g;

    for (g = 0; g < ctx->ngroups; g++) ctx->groups[g].reg = 0;
    ctx->length = 0;
}

void crc16_ctx_advance(const crc16_ctx *ctx, uint16_t *regs, const void *data, size_t len)
{
    const unsigned char *p = (const unsigned char *)data;

    /* Run all groups over a sub-block so the data stays hot in cache. */
    while (len > 0) {
        size_t n = len < CRC16_SUBBLOCK ? len : CRC16_SUBBLOCK;
        size_t g;
        for (g = 0; g < ctx->ngroups; g++)
            regs[g] = crc16_engine_update(&ctx->groups[g].engine, regs[g], p, n);
        p += n;
        len -= n;
    }
}

void crc16_ctx_update(crc16_ctx *ctx, const void *data, size_t len)
{
    uint16_t regs[CRC16_MAX_VARIANTS];
    size_t g;

    for (g = 0; g < ctx->ngroups; g++) regs[g] = ctx->groups[g].reg;
    crc16_ctx_advance(ctx, regs, data, len);
    for (g = 0; g < ctx->ngroups; g++) ctx->groups[g].reg = regs[g];
    ctx->length += len;
}

void crc16_ctx_final(const crc16_ctx *ctx, uint16_t *results)
{
    size_t i;

    for (i = 0; i < ctx->nmembers; i++) {
        const crc16_variant *v = &crc16_variants[ctx->members[i].variant];
        const crc16_group *g = &ctx->groups[ctx->members[i].group];
        uint16_t init_reg = v->refin ? crc16_reflect16(v->init) : v->init;
        uint16_t shift = crc16_xpow8n(ctx->length, v->poly, v->refin);
        uint16_t reg = (uint16_t)(g->reg ^ crc16_mulmod(init_reg, shift, v->poly, v->refin));
        results[i] = (uint16_t)(reg ^ v->xorout);
    }
}
