/* heapviz - interceptor overhead benchmark.
 *
 * Copyright (C) 2026 Parth Sinha
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Measures nanoseconds per allocator call. Run once bare and once under
 * LD_PRELOAD; the difference is what heapviz costs the target.
 *
 * ROADMAP.md M1.8 budget: under 50 ns added per allocation.
 *
 * The workload is deliberately malloc/free of a small block in a tight loop,
 * which is the worst case for a hook: real programs do work between
 * allocations, so the relative cost there is far lower. Measuring the worst
 * case keeps the number honest.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define WARMUP 100000

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * UINT64_C(1000000000) + (uint64_t)ts.tv_nsec;
}

/* volatile sink: without it the compiler can prove the allocation is dead and
 * delete the whole loop, which would measure nothing at all. */
static void *volatile g_sink;

int main(int argc, char **argv) {
    long iterations = 2000000;
    uint64_t start, elapsed;
    long i;
    double per_call;

    if (argc > 1) iterations = strtol(argv[1], NULL, 10);
    if (iterations < 1000) iterations = 1000;

    for (i = 0; i < WARMUP; i++) {
        void *p = malloc(64);
        g_sink = p;
        free(p);
    }

    start = now_ns();
    for (i = 0; i < iterations; i++) {
        void *p = malloc(64);
        g_sink = p;
        free(p);
    }
    elapsed = now_ns() - start;

    /* Two allocator calls per iteration (malloc + free). */
    per_call = (double)elapsed / ((double)iterations * 2.0);

    printf("%.2f ns/call  (%ld iterations, %.3f s total)\n",
           per_call, iterations, (double)elapsed / 1e9);
    return 0;
}
