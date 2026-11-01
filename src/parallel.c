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
 * parallel.c - chunk-parallel update using a persistent C11 thread pool.
 *
 * The buffer is cut into pieces; each task runs every group over one piece
 * starting from a zero register. The per-piece registers are then merged in
 * order with the CRC combine identity (the same GF(2) machinery as in gf2.c):
 *
 *   reg(0, A || B) = reg(0, A) * x^(8*|B|)  XOR  reg(0, B)
 *
 * Without <threads.h> (or before C11) every entry point degrades to a
 * serial equivalent.
 */
#define _POSIX_C_SOURCE 200112L /* sysconf() for the CPU count */

#include "crc16.h"

#include <stdlib.h>
#include <string.h>

#if defined(__unix__) || defined(__APPLE__)
#include <unistd.h>
#endif

#if !defined(CRC16_NO_THREADS) && defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L && \
    !defined(__STDC_NO_THREADS__)
#if defined(__has_include)
#if __has_include(<threads.h>)
#define CRC16_HAVE_THREADS 1
#endif
#else
#define CRC16_HAVE_THREADS 1
#endif
#endif

#ifndef DEFAULT_PIECE_SIZE
/* `make bench` (pre-faulted memory) prefers 32-64 KiB, but the CLI on
 * mapped files is 2-4% faster with 128 KiB; 256 KiB loses ~15% in both. */
#define DEFAULT_PIECE_SIZE ((size_t)128 * 1024)
#endif

unsigned crc16_default_threads(void)
{
#if defined(_SC_NPROCESSORS_ONLN)
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    if (n > 1) return n > 256 ? 256u : (unsigned)n;
#endif
    return 1;
}

#ifndef CRC16_HAVE_THREADS

int crc16_threads_supported(void) { return 0; }
crc16_pool *crc16_pool_create(unsigned nthreads) { (void)nthreads; return NULL; }
void crc16_pool_destroy(crc16_pool *pool) { (void)pool; }
void crc16_pool_set_piece_size(crc16_pool *pool, size_t bytes) { (void)pool; (void)bytes; }

void crc16_ctx_update_begin(crc16_ctx *ctx, crc16_pool *pool, const void *data, size_t len)
{
    (void)pool;
    crc16_ctx_update(ctx, data, len);
}

void crc16_ctx_update_end(crc16_ctx *ctx, crc16_pool *pool)
{
    (void)ctx;
    (void)pool;
}

#else /* CRC16_HAVE_THREADS */

static int worker_main(void *arg);

/*
 * Thin OS layer. Default: C11 <threads.h>. ThreadSanitizer does not see
 * glibc's C11 thread/mutex/condvar calls, so `make tsan` builds with
 * -DCRC16_TSAN_PTHREAD to map the same operations onto pthreads instead.
 * All wrappers return 0 on success.
 */
#ifdef CRC16_TSAN_PTHREAD
#include <pthread.h>

typedef pthread_t os_thread;
typedef pthread_mutex_t os_mutex;
typedef pthread_cond_t os_cond;

static void *worker_trampoline(void *arg)
{
    worker_main(arg);
    return NULL;
}

static int os_thread_start(os_thread *t, void *arg) { return pthread_create(t, NULL, worker_trampoline, arg); }
static void os_thread_join(os_thread t) { pthread_join(t, NULL); }
static int os_mutex_init(os_mutex *m) { return pthread_mutex_init(m, NULL); }
static void os_mutex_destroy(os_mutex *m) { pthread_mutex_destroy(m); }
static void os_lock(os_mutex *m) { pthread_mutex_lock(m); }
static void os_unlock(os_mutex *m) { pthread_mutex_unlock(m); }
static int os_cond_init(os_cond *c) { return pthread_cond_init(c, NULL); }
static void os_cond_destroy(os_cond *c) { pthread_cond_destroy(c); }
static void os_cond_wait(os_cond *c, os_mutex *m) { pthread_cond_wait(c, m); }
static void os_cond_signal(os_cond *c) { pthread_cond_signal(c); }
static void os_cond_broadcast(os_cond *c) { pthread_cond_broadcast(c); }
#else
#include <threads.h>

typedef thrd_t os_thread;
typedef mtx_t os_mutex;
typedef cnd_t os_cond;

static int os_thread_start(os_thread *t, void *arg) { return thrd_create(t, worker_main, arg) == thrd_success ? 0 : -1; }
static void os_thread_join(os_thread t) { thrd_join(t, NULL); }
static int os_mutex_init(os_mutex *m) { return mtx_init(m, mtx_plain) == thrd_success ? 0 : -1; }
static void os_mutex_destroy(os_mutex *m) { mtx_destroy(m); }
static void os_lock(os_mutex *m) { mtx_lock(m); }
static void os_unlock(os_mutex *m) { mtx_unlock(m); }
static int os_cond_init(os_cond *c) { return cnd_init(c) == thrd_success ? 0 : -1; }
static void os_cond_destroy(os_cond *c) { cnd_destroy(c); }
static void os_cond_wait(os_cond *c, os_mutex *m) { cnd_wait(c, m); }
static void os_cond_signal(os_cond *c) { cnd_signal(c); }
static void os_cond_broadcast(os_cond *c) { cnd_broadcast(c); }
#endif

struct crc16_pool {
    unsigned nthreads; /* workers actually started */
    os_thread *threads;
    os_mutex mu;
    os_cond job_cv;  /* a new job (or shutdown) is available */
    os_cond done_cv; /* all pieces of the job are finished */
    int shutdown;
    unsigned long generation;
    size_t piece_size;

    /* current job; written by the owner only while no job is running */
    int active;
    crc16_ctx *ctx;
    const unsigned char *data;
    size_t len;
    size_t npieces;
    size_t next; /* next piece to hand out (protected by mu) */
    size_t done; /* finished pieces (protected by mu) */
    uint16_t *regs; /* npieces * ngroups piece registers */
    size_t regs_cap;
};

int crc16_threads_supported(void) { return 1; }

static void compute_piece(const crc16_pool *pool, size_t j)
{
    const crc16_ctx *ctx = pool->ctx;
    size_t off = j * pool->piece_size;
    size_t plen = pool->len - off;
    uint16_t *out = pool->regs + j * ctx->ngroups;
    size_t g;

    if (plen > pool->piece_size) plen = pool->piece_size;
    for (g = 0; g < ctx->ngroups; g++) out[g] = 0;
    crc16_ctx_advance(ctx, out, pool->data + off, plen);
}

static void run_pieces(crc16_pool *pool)
{
    for (;;) {
        size_t j;
        os_lock(&pool->mu);
        j = pool->next < pool->npieces ? pool->next++ : pool->npieces;
        os_unlock(&pool->mu);
        if (j >= pool->npieces) return;

        compute_piece(pool, j);

        os_lock(&pool->mu);
        if (++pool->done == pool->npieces) os_cond_signal(&pool->done_cv);
        os_unlock(&pool->mu);
    }
}

static int worker_main(void *arg)
{
    crc16_pool *pool = (crc16_pool *)arg;
    unsigned long seen = 0;

    for (;;) {
        os_lock(&pool->mu);
        while (!pool->shutdown && pool->generation == seen) os_cond_wait(&pool->job_cv, &pool->mu);
        if (pool->shutdown) {
            os_unlock(&pool->mu);
            return 0;
        }
        seen = pool->generation;
        os_unlock(&pool->mu);
        run_pieces(pool);
    }
}

crc16_pool *crc16_pool_create(unsigned nthreads)
{
    crc16_pool *pool;
    unsigned i;

    if (nthreads < 2) return NULL;
    pool = (crc16_pool *)calloc(1, sizeof *pool);
    if (pool == NULL) return NULL;
    pool->threads = (os_thread *)calloc(nthreads, sizeof *pool->threads);
    pool->piece_size = DEFAULT_PIECE_SIZE;
    if (pool->threads == NULL || os_mutex_init(&pool->mu) != 0) {
        free(pool->threads);
        free(pool);
        return NULL;
    }
    if (os_cond_init(&pool->job_cv) != 0) {
        os_mutex_destroy(&pool->mu);
        free(pool->threads);
        free(pool);
        return NULL;
    }
    if (os_cond_init(&pool->done_cv) != 0) {
        os_cond_destroy(&pool->job_cv);
        os_mutex_destroy(&pool->mu);
        free(pool->threads);
        free(pool);
        return NULL;
    }
    for (i = 0; i < nthreads; i++) {
        if (os_thread_start(&pool->threads[i], pool) != 0) break;
        pool->nthreads++;
    }
    if (pool->nthreads == 0) {
        crc16_pool_destroy(pool);
        return NULL;
    }
    return pool;
}

void crc16_pool_destroy(crc16_pool *pool)
{
    unsigned i;

    if (pool == NULL) return;
    os_lock(&pool->mu);
    pool->shutdown = 1;
    os_cond_broadcast(&pool->job_cv);
    os_unlock(&pool->mu);
    for (i = 0; i < pool->nthreads; i++) os_thread_join(pool->threads[i]);
    os_cond_destroy(&pool->done_cv);
    os_cond_destroy(&pool->job_cv);
    os_mutex_destroy(&pool->mu);
    free(pool->regs);
    free(pool->threads);
    free(pool);
}

void crc16_pool_set_piece_size(crc16_pool *pool, size_t bytes)
{
    if (pool != NULL && bytes > 0) pool->piece_size = bytes;
}

void crc16_ctx_update_begin(crc16_ctx *ctx, crc16_pool *pool, const void *data, size_t len)
{
    size_t npieces, need;

    if (pool == NULL || ctx->ngroups == 0 || len <= pool->piece_size) {
        crc16_ctx_update(ctx, data, len);
        return;
    }
    npieces = (len + pool->piece_size - 1) / pool->piece_size;
    need = npieces * ctx->ngroups;
    if (need > pool->regs_cap) {
        uint16_t *bigger = (uint16_t *)realloc(pool->regs, need * sizeof *bigger);
        if (bigger == NULL) { /* out of memory: stay correct, run serially */
            crc16_ctx_update(ctx, data, len);
            return;
        }
        pool->regs = bigger;
        pool->regs_cap = need;
    }

    os_lock(&pool->mu);
    pool->ctx = ctx;
    pool->data = (const unsigned char *)data;
    pool->len = len;
    pool->npieces = npieces;
    pool->next = 0;
    pool->done = 0;
    pool->active = 1;
    pool->generation++;
    os_cond_broadcast(&pool->job_cv);
    os_unlock(&pool->mu);
}

void crc16_ctx_update_end(crc16_ctx *ctx, crc16_pool *pool)
{
    uint16_t full[CRC16_MAX_VARIANTS];
    size_t g, j, last_len;

    if (pool == NULL || !pool->active) return;

    os_lock(&pool->mu);
    while (pool->done < pool->npieces) os_cond_wait(&pool->done_cv, &pool->mu);
    os_unlock(&pool->mu);
    pool->active = 0;

    last_len = pool->len - (pool->npieces - 1) * pool->piece_size;
    for (g = 0; g < ctx->ngroups; g++)
        full[g] = crc16_xpow8n(pool->piece_size, ctx->groups[g].engine.poly,
                               ctx->groups[g].engine.reflected);
    for (j = 0; j < pool->npieces; j++) {
        const uint16_t *piece_regs = pool->regs + j * ctx->ngroups;
        int is_last = j + 1 == pool->npieces;
        for (g = 0; g < ctx->ngroups; g++) {
            crc16_group *grp = &ctx->groups[g];
            uint16_t shift = is_last ? crc16_xpow8n(last_len, grp->engine.poly, grp->engine.reflected)
                                     : full[g];
            grp->reg = (uint16_t)(crc16_mulmod(grp->reg, shift, grp->engine.poly,
                                               grp->engine.reflected) ^ piece_regs[g]);
        }
    }
    ctx->length += pool->len;
}

#endif /* CRC16_HAVE_THREADS */
