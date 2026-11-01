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

/* main.c - entry point: file loop, self-test and exit codes. */
#define _POSIX_C_SOURCE 200112L /* fileno, fstat, mmap (optional fast path) */
#define _FILE_OFFSET_BITS 64    /* files over 2 GiB on 32-bit POSIX systems */

#include "cli.h"
#include "crc16.h"
#include "output.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

/*
 * Regular files are memory-mapped when the platform allows it: workers then
 * read straight from the page cache, with no copy into a buffer (which
 * costs a second pass over cold memory and was the bottleneck of the fread
 * path). Everything else, and any failure to map, uses plain fread.
 */
#if defined(__unix__) || defined(__APPLE__)
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#define HAVE_MMAP_IO 1
#endif

#ifndef MMAP_MIN_SIZE
#define MMAP_MIN_SIZE (4u * 1024u * 1024u) /* smaller files: fread is as fast */
#endif
#ifndef MMAP_WINDOW
#define MMAP_WINDOW ((size_t)1 << 30) /* bytes mapped at a time (multiple of the page size) */
#endif

#ifndef READ_BUFFER_SIZE
#define READ_BUFFER_SIZE (4u * 1024u * 1024u)
#endif

static crc16_ctx g_ctx; /* large; keep off the stack. Built once, reset per file. */

typedef struct run_state {
    unsigned char *buf[2]; /* double buffer: read one while the pool works on the other */
    unsigned jobs;         /* requested worker threads, 0 = auto */
    crc16_pool *pool;      /* created lazily, once a file fills a whole buffer */
    int pool_tried;
    int stdin_seen; /* setvbuf is only valid before the first I/O on a stream */
} run_state;

static void ensure_pool(run_state *rs)
{
    if (!rs->pool_tried) {
        unsigned threads = rs->jobs != 0 ? rs->jobs : crc16_default_threads();
        rs->pool_tried = 1;
        rs->pool = crc16_pool_create(threads);
    }
}

#ifdef HAVE_MMAP_IO
enum { MAP_UNUSED = 1, MAP_FAILED_REPORTED = -1 };

/* Returns 0 when the whole file was processed, MAP_UNUSED when the caller
 * should fall back to fread (nothing consumed), or -1 after an error. */
static int process_mapped(FILE *fp, const char *name, run_state *rs, const cli_options *opt)
{
    struct stat st;
    uint64_t size, off = 0;

    if (opt->no_mmap || fstat(fileno(fp), &st) != 0 || !S_ISREG(st.st_mode) || st.st_size < 0 ||
        (uint64_t)st.st_size < MMAP_MIN_SIZE)
        return MAP_UNUSED;
    size = (uint64_t)st.st_size;
    ensure_pool(rs);

    while (off < size) {
        size_t len = size - off < MMAP_WINDOW ? (size_t)(size - off) : MMAP_WINDOW;
        void *map = mmap(NULL, len, PROT_READ, MAP_PRIVATE, fileno(fp), (off_t)off);
        if (map == MAP_FAILED) {
            if (off == 0) return MAP_UNUSED;
            fprintf(stderr, "crc16: %s: %s\n", name, strerror(errno));
            return -1;
        }
        (void)posix_madvise(map, len, POSIX_MADV_SEQUENTIAL);
        crc16_ctx_update_begin(&g_ctx, rs->pool, map, len);
        crc16_ctx_update_end(&g_ctx, rs->pool);
        munmap(map, len);
        off += len;
    }
    return 0;
}
#endif

/* Returns 0 on success, -1 after reporting an error on stderr. */
static int process(const char *name, run_state *rs, const cli_options *opt,
                   uint64_t *bytes, uint16_t *results)
{
    int is_stdin = strcmp(name, "-") == 0;
    FILE *fp = is_stdin ? stdin : fopen(name, "rb");
    size_t n, m;
    int cur = 0;

    if (fp == NULL) {
        fprintf(stderr, "crc16: %s: %s\n", name, strerror(errno));
        return -1;
    }
    if (!is_stdin || !rs->stdin_seen) setvbuf(fp, NULL, _IONBF, 0); /* we read in large blocks */
    if (is_stdin) rs->stdin_seen = 1;
    crc16_ctx_reset(&g_ctx);

#ifdef HAVE_MMAP_IO
    if (!is_stdin) {
        int r = process_mapped(fp, name, rs, opt);
        if (r == 0) {
            fclose(fp);
            *bytes = g_ctx.length;
            crc16_ctx_final(&g_ctx, results);
            return 0;
        }
        if (r < 0) {
            fclose(fp);
            return -1;
        }
    }
#endif
    n = fread(rs->buf[0], 1, READ_BUFFER_SIZE, fp);
    while (n > 0) {
        if (n == READ_BUFFER_SIZE) ensure_pool(rs); /* big input: worth the threads */
        crc16_ctx_update_begin(&g_ctx, rs->pool, rs->buf[cur], n);
        /* A short read means EOF or error, so only a full buffer can have a successor. */
        m = n == READ_BUFFER_SIZE ? fread(rs->buf[cur ^ 1], 1, READ_BUFFER_SIZE, fp) : 0;
        crc16_ctx_update_end(&g_ctx, rs->pool);
        cur ^= 1;
        n = m;
    }
    if (ferror(fp)) {
        fprintf(stderr, "crc16: %s: %s\n", is_stdin ? "stdin" : name, strerror(errno));
        if (!is_stdin) fclose(fp);
        return -1;
    }
    if (!is_stdin) fclose(fp);
    *bytes = g_ctx.length;
    crc16_ctx_final(&g_ctx, results);
    return 0;
}

/*
 * Two checks per variant: the published check value (bit-by-bit reference
 * and grouped context), and a long pseudo-random buffer run through the
 * fast paths actually used on this machine (SIMD folding, thread pool and
 * CRC combine) against the bit-by-bit reference.
 */
static int self_test(const cli_options *opt)
{
    enum { LONG_LEN = 65536 + 13, LONG_PIECE = 4096 };
    static const char input[] = "123456789";
    static unsigned char buf[LONG_LEN];
    size_t all[CRC16_MAX_VARIANTS];
    uint16_t results[CRC16_MAX_VARIANTS], fast[CRC16_MAX_VARIANTS];
    crc16_pool *pool;
    uint32_t x = 1;
    size_t i;
    int failures = 0;

    for (i = 0; i < crc16_variant_count; i++) all[i] = i;
    if (crc16_ctx_init(&g_ctx, all, crc16_variant_count) != 0) {
        fprintf(stderr, "crc16: self-test: context initialisation failed\n");
        return 1;
    }
    crc16_ctx_update(&g_ctx, input, sizeof input - 1);
    crc16_ctx_final(&g_ctx, results);

    for (i = 0; i < LONG_LEN; i++) {
        x = x * 1103515245u + 12345u;
        buf[i] = (unsigned char)(x >> 24);
    }
    pool = crc16_pool_create(opt->jobs != 0 ? opt->jobs : crc16_default_threads());
    crc16_pool_set_piece_size(pool, LONG_PIECE);
    crc16_ctx_reset(&g_ctx);
    crc16_ctx_update_begin(&g_ctx, pool, buf, LONG_LEN);
    crc16_ctx_update_end(&g_ctx, pool);
    crc16_ctx_final(&g_ctx, fast);
    crc16_pool_destroy(pool);

    for (i = 0; i < crc16_variant_count; i++) {
        const crc16_variant *v = &crc16_variants[i];
        uint16_t ref = crc16_bitwise(v, input, sizeof input - 1);
        int ok = results[i] == v->check && ref == v->check &&
                 fast[i] == crc16_bitwise(v, buf, LONG_LEN);
        if (!ok) failures++;
        if (!ok || !opt->quiet)
            printf("%-25s: %s\n", v->name, ok ? "OK" : "FAIL");
    }
    if (!opt->quiet || failures > 0)
        printf("%u of %u variants OK.\n", (unsigned)(crc16_variant_count - (size_t)failures),
               (unsigned)crc16_variant_count);
    return failures > 0 ? 1 : 0;
}

int main(int argc, char **argv)
{
    cli_options opt;
    char err[256];
    run_state rs;
    output_state out;
    static const char *const stdin_only[] = {"-"};
    const char *const *files;
    size_t nfiles, i;
    int status = 0;

    if (cli_parse(argc, argv, &opt, err, sizeof err) != 0) {
        fprintf(stderr, "crc16: %s\nTry 'crc16 --help' for more information.\n", err);
        cli_free(&opt);
        return 2;
    }
    if (opt.help) {
        cli_usage(stdout);
        cli_free(&opt);
        return 0;
    }
    if (opt.version) {
        printf("crc16 %s\nMinimum ISO C standard: %s\n", CRC16_VERSION, CRC16_MIN_ISO_C);
        printf("Copyright (C) 2026 Daniel Dias Rodrigues\n"
               "License GPL-2.0-or-later: GNU GPL version 2 or later <https://gnu.org/licenses/gpl-2.0.html>.\n"
               "This is free software: you are free to change and redistribute it.\n"
               "There is NO WARRANTY, to the extent permitted by law.\n");
        cli_free(&opt);
        return 0;
    }
    if (opt.list) {
        output_list(stdout, opt.lowercase);
        cli_free(&opt);
        return 0;
    }
    if (opt.self_test) {
        status = self_test(&opt);
        cli_free(&opt);
        return status;
    }

    /* Tables are built once here and reused for every file (crc16_ctx_reset). */
    if (crc16_ctx_init(&g_ctx, opt.ids, opt.nids) != 0) {
        fprintf(stderr, "crc16: context initialisation failed\n");
        cli_free(&opt);
        return 1;
    }
    memset(&rs, 0, sizeof rs);
    rs.jobs = opt.jobs;
    if (opt.jobs > 1 && !crc16_threads_supported())
        fprintf(stderr, "crc16: warning: built without thread support; running single-threaded\n");
    rs.buf[0] = (unsigned char *)malloc(READ_BUFFER_SIZE);
    rs.buf[1] = (unsigned char *)malloc(READ_BUFFER_SIZE);
    if (rs.buf[0] == NULL || rs.buf[1] == NULL) {
        free(rs.buf[0]);
        free(rs.buf[1]);
        fprintf(stderr, "crc16: out of memory\n");
        cli_free(&opt);
        return 1;
    }

    files = opt.nfiles > 0 ? (const char *const *)opt.files : stdin_only;
    nfiles = opt.nfiles > 0 ? opt.nfiles : 1;

    out.fp = stdout;
    out.format = opt.format;
    out.lowercase = opt.lowercase;
    out.quiet = opt.quiet;
    out.title = nfiles > 1;
    output_begin(&out);
    for (i = 0; i < nfiles; i++) {
        uint16_t results[CRC16_MAX_VARIANTS];
        uint64_t bytes = 0;
        if (process(files[i], &rs, &opt, &bytes, results) != 0) {
            status = 1;
            continue;
        }
        output_file(&out, files[i], bytes, opt.ids, results, opt.nids);
    }
    output_end(&out);

    crc16_pool_destroy(rs.pool);
    free(rs.buf[0]);
    free(rs.buf[1]);
    cli_free(&opt);
    if (fflush(stdout) != 0 || ferror(stdout)) {
        fprintf(stderr, "crc16: write error: %s\n", strerror(errno));
        return 1;
    }
    return status;
}
