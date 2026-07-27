/* heapviz - LD_PRELOAD allocator interceptor.
 *
 * Copyright (C) 2026 Parth Sinha
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * M0 scaffold. The interposed allocator functions land in M1; this file
 * currently exists to lock in the build shape that M1 depends on:
 *
 *   - compiled as C11, never C++, so the .so cannot pull in libstdc++ (its
 *     static initialisers allocate, and we are the allocator)
 *   - default-hidden visibility, so only symbols we explicitly export can be
 *     interposed
 *   - no libc call in any path that could reach malloc
 *
 * See ROADMAP.md M1 for the implementation plan, and M1.1 in particular for the
 * dlsym/calloc bootstrap problem that has to be solved before any hook is safe.
 */

#include "common/heapviz_abi.h"

#include <unistd.h>

#define HV_EXPORT __attribute__((visibility("default")))

/* Version probe. Lets the TUI confirm which build of the .so a target loaded
 * without parsing the binary. */
HV_EXPORT uint32_t heapviz_preload_abi_version(void) {
    return HEAPVIZ_ABI_VERSION;
}

/* Emergency diagnostics. write(2) rather than fprintf: this may run from inside
 * an allocator hook, where stdio would recurse straight back into malloc.
 * Unused until M1.1 needs it for the arena-exhaustion abort path. */
__attribute__((unused))
static void hv_emit(const char *msg, size_t len) {
    ssize_t n;
    size_t off = 0;
    while (off < len) {
        n = write(STDERR_FILENO, msg + off, len - off);
        if (n <= 0) return;
        off += (size_t)n;
    }
}

__attribute__((constructor(101)))
static void heapviz_preload_init(void) {
    /* M1.7 attaches the shared-memory ring here. Priority 101 runs ahead of
     * ordinary user constructors so the ring is live for early allocations. */
}

__attribute__((destructor))
static void heapviz_preload_fini(void) {
    /* M1.7 sets producer_exited and unlinks the segment here. */
}
