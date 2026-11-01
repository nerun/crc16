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
 * bench.c - in-memory throughput of the multi-variant context.
 *
 * Usage: bench [threads [piece_KiB [buffer_MiB]]]
 *   threads    0 = one per CPU (default), 1 = serial only
 *   piece_KiB  bytes per parallel task (default: library default)
 *   buffer_MiB size of each update call (default 4), as in the CLI
 * Disk I/O is excluded: data comes from memory.
 */
#include "crc16.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static double now(void)
{
#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L && defined(TIME_UTC)
    struct timespec ts;
    if (timespec_get(&ts, TIME_UTC) == TIME_UTC) return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
#endif
    return (double)clock() / CLOCKS_PER_SEC; /* CPU time: wrong for threads */
}

static double run(crc16_ctx *ctx, crc16_pool *pool, const unsigned char *buf, size_t total,
                  size_t step)
{
    size_t off;
    double t0 = now();
    for (off = 0; off < total; off += step) {
        size_t n = total - off < step ? total - off : step;
        crc16_ctx_update_begin(ctx, pool, buf + off, n);
        crc16_ctx_update_end(ctx, pool);
    }
    return now() - t0;
}

int main(int argc, char **argv)
{
    const size_t total = (size_t)256 * 1024 * 1024;
    unsigned threads = argc > 1 ? (unsigned)atoi(argv[1]) : 0;
    size_t piece = argc > 2 ? (size_t)atoi(argv[2]) * 1024 : 0;
    size_t step = (argc > 3 ? (size_t)atoi(argv[3]) : 4) * 1024 * 1024;
    static const size_t counts[] = {1, 2, 4, 8, 16, 31};
    static crc16_ctx ctx;
    size_t ids[CRC16_MAX_VARIANTS], n, i, c;
    unsigned char *buf = (unsigned char *)malloc(total);
    uint16_t res[CRC16_MAX_VARIANTS], first[CRC16_MAX_VARIANTS];
    crc16_pool *pool;

    if (buf == NULL) return 1;
    for (i = 0; i < total; i++) buf[i] = (unsigned char)(i * 2654435761u >> 24);
    if (threads == 0) threads = crc16_default_threads();
    pool = crc16_pool_create(threads);
    if (pool != NULL && piece != 0) crc16_pool_set_piece_size(pool, piece);
    printf("threads=%u (supported=%d) piece=%zu KiB step=%zu MiB\n", pool != NULL ? threads : 1,
           crc16_threads_supported(), piece / 1024, step / (1024 * 1024));

    for (c = 0; c < sizeof counts / sizeof counts[0]; c++) {
        double s1, sm = 0;
        n = counts[c];
        for (i = 0; i < n; i++) ids[i] = i;
        crc16_ctx_init(&ctx, ids, n);
        s1 = run(&ctx, NULL, buf, total, step);
        crc16_ctx_final(&ctx, res);
        memcpy(first, res, n * sizeof res[0]);
        printf("%2u variants, %2u groups: serial %8.1f MB/s", (unsigned)n, (unsigned)ctx.ngroups,
               (double)total / 1e6 / s1);
        if (pool != NULL) {
            crc16_ctx_init(&ctx, ids, n);
            sm = run(&ctx, pool, buf, total, step);
            crc16_ctx_final(&ctx, res);
            printf("  threaded %8.1f MB/s  (x%.1f)%s", (double)total / 1e6 / sm, s1 / sm,
                   memcmp(res, first, n * sizeof res[0]) == 0 ? "" : "  MISMATCH");
        }
        printf("\n");
    }
    crc16_pool_destroy(pool);
    free(buf);
    return 0;
}
