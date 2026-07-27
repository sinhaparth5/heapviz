/* heapviz - configurable allocation workload.
 *
 * Copyright (C) 2026 Parth Sinha
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The target program for exercising the interceptor (ROADMAP.md M1.8) and,
 * later, for having something interesting on screen while building the TUI.
 *
 *   ./churn [--mode steady|bursty|fragment|mmap|mixed]
 *           [--threads N] [--seconds S] [--rate PER_SEC]
 *
 * Modes:
 *   steady    even allocation rate, mixed sizes, prompt frees
 *   bursty    idle stretches punctuated by large batches
 *   fragment  allocate many, free every other one, leaving holes
 *   mmap      blocks over the 128 KB threshold, so glibc uses mmap not brk
 *   mixed     all of the above interleaved (default)
 */

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define MAX_THREADS  32
#define LIVE_SLOTS   512
#define MMAP_BYTES   (256 * 1024)

typedef enum { M_STEADY, M_BURSTY, M_FRAGMENT, M_MMAP, M_MIXED } Mode;

static Mode     g_mode = M_MIXED;
static int      g_threads = 1;
static double   g_seconds = 5.0;
static long     g_rate = 20000; /* allocations per second per thread */
static volatile int g_stop = 0;

/* usleep was removed in POSIX.1-2008 and we build with extensions off. */
static void sleep_us(long us) {
    struct timespec ts;
    ts.tv_sec  = us / 1000000L;
    ts.tv_nsec = (us % 1000000L) * 1000L;
    nanosleep(&ts, NULL);
}

static double now_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

/* xorshift: we want a reproducible, allocation-free PRNG. rand() is not
 * thread-safe and random_r needs state we would rather not manage. */
static uint32_t xrand(uint32_t *s) {
    uint32_t x = *s;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    return (*s = x);
}

typedef struct {
    void  *ptr;
    size_t size;
} Slot;

/* At -O3, GCC deletes an allocation whose contents are never read: an
 * alloc/write/free sequence with no observable effect is dead code, and the
 * whole pair disappears. That silently removed the entire mmap path from
 * release builds. Routing one byte through a volatile sink makes the
 * allocation observable, so it survives. */
static volatile unsigned char g_sink;

static void do_steady(Slot *live, uint32_t *seed, long *ops) {
    const int idx = (int)(xrand(seed) % LIVE_SLOTS);
    const size_t sz = 16u + (xrand(seed) % 4096u);

    if (live[idx].ptr != NULL) { free(live[idx].ptr); live[idx].ptr = NULL; }
    live[idx].ptr = malloc(sz);
    live[idx].size = sz;
    if (live[idx].ptr) memset(live[idx].ptr, (int)(sz & 0xFF), sz > 64 ? 64 : sz);
    *ops += 2;
}

static void do_bursty(Slot *live, uint32_t *seed, long *ops) {
    int i;
    for (i = 0; i < 64; i++) do_steady(live, seed, ops);
    /* The idle gap is what makes the burst visible as a pulse in the TUI. */
    sleep_us(2000);
}

static void do_fragment(Slot *live, uint32_t *seed, long *ops) {
    int i;
    for (i = 0; i < LIVE_SLOTS; i++) {
        if (live[i].ptr == NULL) {
            const size_t sz = 32u + (xrand(seed) % 512u);
            live[i].ptr = malloc(sz);
            live[i].size = sz;
            (*ops)++;
        }
    }
    /* Free every other slot: the classic way to leave the allocator holding a
     * lot of unusable space between live chunks. */
    for (i = 0; i < LIVE_SLOTS; i += 2) {
        if (live[i].ptr) { free(live[i].ptr); live[i].ptr = NULL; (*ops)++; }
    }
}

static void do_mmap(uint32_t *seed, long *ops) {
    const size_t sz = MMAP_BYTES + (xrand(seed) % MMAP_BYTES);
    unsigned char *p = calloc(1, sz);
    if (p) {
        p[0] = 1;
        p[sz - 1] = 2;
        g_sink = (unsigned char)(p[0] + p[sz - 1]);
        free(p);
    }
    *ops += 2;
}

static void do_realloc_chain(uint32_t *seed, long *ops) {
    size_t sz = 32u + (xrand(seed) % 64u);
    char *p = malloc(sz);
    int i;
    if (!p) return;
    (*ops)++;
    for (i = 0; i < 10; i++) {
        char *next;
        sz *= 2;
        next = realloc(p, sz);
        if (!next) break;
        p = next;
        p[sz - 1] = (char)i;
        g_sink = (unsigned char)p[sz - 1];
        (*ops)++;
    }
    free(p);
    (*ops)++;
}

static void *worker(void *arg) {
    const int tid = (int)(intptr_t)arg;
    uint32_t seed = (uint32_t)(0x9E3779B9u ^ (uint32_t)(tid + 1) * 2654435761u);
    Slot *live = calloc(LIVE_SLOTS, sizeof(Slot));
    const double deadline = now_sec() + g_seconds;
    const double per_op = (g_rate > 0) ? 1.0 / (double)g_rate : 0.0;
    double next_op = now_sec();
    long ops = 0;
    int i;

    if (!live) return NULL;

    while (!g_stop && now_sec() < deadline) {
        switch (g_mode) {
        case M_STEADY:   do_steady(live, &seed, &ops); break;
        case M_BURSTY:   do_bursty(live, &seed, &ops); break;
        case M_FRAGMENT: do_fragment(live, &seed, &ops); break;
        case M_MMAP:     do_mmap(&seed, &ops); break;
        case M_MIXED:
        default:
            switch (xrand(&seed) % 10u) {
            case 0: case 1: do_fragment(live, &seed, &ops); break;
            case 2:         do_mmap(&seed, &ops); break;
            case 3:         do_realloc_chain(&seed, &ops); break;
            case 4:         do_bursty(live, &seed, &ops); break;
            default:        do_steady(live, &seed, &ops); break;
            }
            break;
        }

        if (per_op > 0.0) {
            next_op += per_op;
            for (;;) {
                const double slack = next_op - now_sec();
                if (slack <= 0.0) break;
                if (slack > 0.001) sleep_us((long)(slack * 1e6));
                else break;
            }
        }
    }

    for (i = 0; i < LIVE_SLOTS; i++) free(live[i].ptr);
    free(live);
    printf("  thread %d: %ld allocator calls\n", tid, ops);
    return NULL;
}

static Mode parse_mode(const char *s) {
    if (strcmp(s, "steady")   == 0) return M_STEADY;
    if (strcmp(s, "bursty")   == 0) return M_BURSTY;
    if (strcmp(s, "fragment") == 0) return M_FRAGMENT;
    if (strcmp(s, "mmap")     == 0) return M_MMAP;
    return M_MIXED;
}

int main(int argc, char **argv) {
    pthread_t threads[MAX_THREADS];
    int i;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--mode") == 0 && i + 1 < argc)
            g_mode = parse_mode(argv[++i]);
        else if (strcmp(argv[i], "--threads") == 0 && i + 1 < argc)
            g_threads = atoi(argv[++i]);
        else if (strcmp(argv[i], "--seconds") == 0 && i + 1 < argc)
            g_seconds = atof(argv[++i]);
        else if (strcmp(argv[i], "--rate") == 0 && i + 1 < argc)
            g_rate = atol(argv[++i]);
        else {
            fprintf(stderr,
                "usage: churn [--mode steady|bursty|fragment|mmap|mixed]\n"
                "             [--threads N] [--seconds S] [--rate PER_SEC]\n");
            return 2;
        }
    }
    if (g_threads < 1) g_threads = 1;
    if (g_threads > MAX_THREADS) g_threads = MAX_THREADS;

    printf("churn: %d thread(s), %.1fs, rate %ld/s/thread\n",
           g_threads, g_seconds, g_rate);

    for (i = 0; i < g_threads; i++) {
        if (pthread_create(&threads[i], NULL, worker, (void *)(intptr_t)i) != 0) {
            perror("pthread_create");
            return 1;
        }
    }
    for (i = 0; i < g_threads; i++) pthread_join(threads[i], NULL);

    printf("churn: done\n");
    return 0;
}
