/* heapviz - libheapviz.so smoke test.
 *
 * Copyright (C) 2026 Parth Sinha
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * dlopen()s the interceptor and calls its version probe. Without this, the
 * "no libstdc++" check passes trivially on a library that does not even load:
 * ldd prints "statically linked" both for a clean .so with no dependencies and
 * for one that is broken. This asserts the artifact is real.
 *
 * Note this only proves the library LOADS. It is not LD_PRELOADed here, because
 * from M1 onward that would put the malloc hook inside the test's own process.
 * Interception is covered by M1.8 with a dedicated target program.
 */

#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>

#include "common/heapviz_abi.h"

int main(int argc, char **argv) {
    uint32_t (*abi_version)(void);
    void *handle;
    uint32_t got;

    if (argc != 2) {
        fprintf(stderr, "usage: preload_load <path-to-libheapviz.so>\n");
        return 2;
    }

    handle = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!handle) {
        fprintf(stderr, "dlopen failed: %s\n", dlerror());
        return 1;
    }

    /* POSIX requires this cast dance to convert void* to a function pointer. */
    *(void **)(&abi_version) = dlsym(handle, "heapviz_preload_abi_version");
    if (!abi_version) {
        fprintf(stderr, "dlsym(heapviz_preload_abi_version) failed: %s\n",
                dlerror());
        dlclose(handle);
        return 1;
    }

    got = abi_version();
    if (got != HEAPVIZ_ABI_VERSION) {
        fprintf(stderr, "ABI mismatch: .so reports v%u, test built against v%u\n",
                got, HEAPVIZ_ABI_VERSION);
        dlclose(handle);
        return 1;
    }

    /* Internal helpers must stay hidden; only the interposed entry points and
     * the version probe are exported. */
    if (dlsym(handle, "hv_emit") != NULL) {
        fprintf(stderr, "hv_emit is exported; visibility=hidden is not working\n");
        dlclose(handle);
        return 1;
    }

    dlclose(handle);
    printf("libheapviz.so loads, reports ABI v%u, keeps internals hidden\n", got);
    return 0;
}
