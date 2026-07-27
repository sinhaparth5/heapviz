/* heapviz - minimal allocation workload.
 *
 * Copyright (C) 2026 Parth Sinha
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * A deliberately small target to point heapviz at. Exercises the brk path
 * (small chunks), the mmap path (>128 KB, above glibc's default mmap
 * threshold), and a realloc growth chain.
 *
 * The full configurable workload (steady, bursty, fragmenting, multi-threaded)
 * lands in M1.8 as examples/churn.c. This one exists so `examples/` holds
 * something real from M0 onward, and so there is a trivial target to sanity
 * check the interceptor against the moment M1 starts.
 *
 *   ./hello_alloc [iterations]
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SMALL_COUNT 64
#define MMAP_BYTES  (512 * 1024) /* comfortably over the 128 KB threshold */

int main(int argc, char **argv) {
    long iterations = 4;
    void *small[SMALL_COUNT];
    unsigned char *big;
    char *grow;
    size_t grow_len = 16;
    long it;
    int i;

    if (argc > 1) {
        iterations = strtol(argv[1], NULL, 10);
        if (iterations < 1) iterations = 1;
    }

    for (it = 0; it < iterations; it++) {
        /* Small chunks from the main arena, then freed in a pattern that
         * leaves gaps for the allocator to reuse. */
        for (i = 0; i < SMALL_COUNT; i++) {
            small[i] = malloc((size_t)(i + 1) * 24);
            if (!small[i]) return 1;
            memset(small[i], i, (size_t)(i + 1) * 24);
        }
        for (i = 0; i < SMALL_COUNT; i += 2) free(small[i]);
        for (i = 1; i < SMALL_COUNT; i += 2) free(small[i]);

        /* Large enough to go through mmap rather than brk. */
        big = calloc(1, MMAP_BYTES);
        if (!big) return 1;
        big[0] = 1;
        big[MMAP_BYTES - 1] = 2;
        free(big);

        /* A realloc chain: repeated growth forces the allocator to move the
         * block once it can no longer extend in place. */
        grow = malloc(grow_len);
        if (!grow) return 1;
        for (i = 0; i < 12; i++) {
            char *next;
            grow_len *= 2;
            next = realloc(grow, grow_len);
            if (!next) { free(grow); return 1; }
            grow = next;
            grow[grow_len - 1] = (char)i;
        }
        free(grow);
    }

    printf("hello_alloc: %ld iteration(s) done\n", iterations);
    return 0;
}
