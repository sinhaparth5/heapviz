/* heapviz - LD_PRELOAD allocator interceptor.
 *
 * Copyright (C) 2026 Parth Sinha
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Interposes the glibc allocator entry points and publishes one 32-byte event
 * per call into a shared-memory ring that the heapviz TUI drains from another
 * process.
 *
 * Compiled as C11, never C++: a C++ shared object links libstdc++, whose static
 * initialisers allocate, and this library IS the allocator inside the target.
 *
 * The three invariants (ROADMAP.md ground rules 1 and 2):
 *
 *   1. No path here calls malloc. Not through printf, not through dlerror, not
 *      through a hidden libc helper. Any allocation inside a hook is a
 *      re-entrancy deadlock on a threaded target.
 *   2. Nothing here blocks. A full ring drops the event and bumps a counter.
 *   3. Environment knobs are read once, in the constructor, never per call.
 */

#include "common/heapviz_abi.h"
#include "common/heapviz_ring.h"

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define HV_EXPORT __attribute__((visibility("default")))

/* Thread-local storage must be initial-exec. The default (global-dynamic) model
 * allocates a TLS block on first access in a dlopen'd library, and we would be
 * inside malloc when that happens. LD_PRELOAD libraries are part of the initial
 * exec set, so initial-exec is always available to us. */
#define HV_TLS __thread __attribute__((tls_model("initial-exec")))

/* ------------------------------------------------------------------ */
/* Diagnostics                                                        */
/* ------------------------------------------------------------------ */

/* write(2) rather than fprintf: stdio buffers, and buffering means malloc. */
static void hv_write_err(const char *msg) {
    size_t len = 0;
    size_t off = 0;
    while (msg[len] != '\0') len++;
    while (off < len) {
        ssize_t n = write(STDERR_FILENO, msg + off, len - off);
        if (n <= 0) return;
        off += (size_t)n;
    }
}

static void hv_die(const char *msg) {
    hv_write_err(msg);
    _exit(127); /* not exit(): atexit handlers would re-enter the allocator */
}

/* ------------------------------------------------------------------ */
/* M1.1  Bootstrap arena                                              */
/* ------------------------------------------------------------------ */

/* dlsym(RTLD_NEXT, "malloc") calls calloc internally on first use, so our hook
 * runs before it knows what the real allocator is. Every naive LD_PRELOAD
 * malloc hook dies exactly here. Requests made during that window are served
 * from a static arena that is never returned to libc.
 *
 * Each block carries a 16-byte header so realloc can find the old size, which
 * also keeps payloads 16-byte aligned. */

#define HV_BOOTSTRAP_BYTES (64u * 1024u)
#define HV_BOOTSTRAP_MAGIC UINT64_C(0x484F4F5453545250) /* "HOOTSTRP" */

typedef struct {
    uint64_t size;
    uint64_t magic;
} HvBootHeader;

static unsigned char g_bootstrap[HV_BOOTSTRAP_BYTES] __attribute__((aligned(16)));
static _Atomic size_t g_bootstrap_used;

static int hv_is_bootstrap_ptr(const void *p) {
    const unsigned char *c = (const unsigned char *)p;
    return c >= g_bootstrap && c < g_bootstrap + HV_BOOTSTRAP_BYTES;
}

static void *hv_bootstrap_alloc(size_t n) {
    const size_t payload = (n + 15u) & ~(size_t)15u;
    const size_t need = sizeof(HvBootHeader) + payload;
    size_t off;
    HvBootHeader *hdr;

    if (payload < n) return NULL; /* size_t overflow on the round-up */

    off = atomic_fetch_add_explicit(&g_bootstrap_used, need, memory_order_relaxed);
    if (off + need > HV_BOOTSTRAP_BYTES) {
        /* 64 KB is far more than dlsym needs. Exhaustion means something is
         * badly wrong, and silently returning NULL would corrupt the target. */
        hv_die("heapviz: bootstrap arena exhausted; aborting rather than "
               "corrupting the target process\n");
    }

    hdr = (HvBootHeader *)(void *)(g_bootstrap + off);
    hdr->size = n;
    hdr->magic = HV_BOOTSTRAP_MAGIC;
    return g_bootstrap + off + sizeof(HvBootHeader);
}

static size_t hv_bootstrap_size(const void *p) {
    const HvBootHeader *hdr =
        (const HvBootHeader *)(const void *)((const unsigned char *)p
                                             - sizeof(HvBootHeader));
    return (hdr->magic == HV_BOOTSTRAP_MAGIC) ? (size_t)hdr->size : 0u;
}

/* ------------------------------------------------------------------ */
/* M1.2  Symbol resolution                                            */
/* ------------------------------------------------------------------ */

static void *(*g_real_malloc)(size_t);
static void  (*g_real_free)(void *);
static void *(*g_real_calloc)(size_t, size_t);
static void *(*g_real_realloc)(void *, size_t);
static int   (*g_real_posix_memalign)(void **, size_t, size_t);
static void *(*g_real_aligned_alloc)(size_t, size_t);
static void *(*g_real_memalign)(size_t, size_t);
static void *(*g_real_valloc)(size_t);
static void *(*g_real_pvalloc)(size_t);
static size_t (*g_real_malloc_usable_size)(void *);

static _Atomic int g_symbols_ready;

/* Set while this thread is inside dlsym. dlsym allocates, which re-enters our
 * hooks; the flag routes that recursion to the bootstrap arena instead of to a
 * function pointer we have not resolved yet. */
static HV_TLS int g_resolving;

static void hv_resolve_symbols(void) {
    if (atomic_load_explicit(&g_symbols_ready, memory_order_acquire)) return;
    if (g_resolving) return; /* recursive call from inside dlsym */

    g_resolving = 1;

    /* Order matters only in that malloc/calloc/free come first: dlsym's own
     * allocations are served from the arena until these are live. */
    g_real_malloc            = (void *(*)(size_t))dlsym(RTLD_NEXT, "malloc");
    g_real_free              = (void (*)(void *))dlsym(RTLD_NEXT, "free");
    g_real_calloc            = (void *(*)(size_t, size_t))dlsym(RTLD_NEXT, "calloc");
    g_real_realloc           = (void *(*)(void *, size_t))dlsym(RTLD_NEXT, "realloc");
    g_real_posix_memalign    = (int (*)(void **, size_t, size_t))dlsym(RTLD_NEXT, "posix_memalign");
    g_real_aligned_alloc     = (void *(*)(size_t, size_t))dlsym(RTLD_NEXT, "aligned_alloc");
    g_real_memalign          = (void *(*)(size_t, size_t))dlsym(RTLD_NEXT, "memalign");
    g_real_valloc            = (void *(*)(size_t))dlsym(RTLD_NEXT, "valloc");
    g_real_pvalloc           = (void *(*)(size_t))dlsym(RTLD_NEXT, "pvalloc");
    g_real_malloc_usable_size = (size_t (*)(void *))dlsym(RTLD_NEXT, "malloc_usable_size");

    g_resolving = 0;

    if (g_real_malloc == NULL || g_real_free == NULL) {
        hv_die("heapviz: could not resolve malloc/free via dlsym\n");
    }
    /* valloc and pvalloc are absent on some libcs; that is not fatal. */

    atomic_store_explicit(&g_symbols_ready, 1, memory_order_release);
}

/* ------------------------------------------------------------------ */
/* M1.7  Shared memory ring                                           */
/* ------------------------------------------------------------------ */

static HvRingHeader *g_ring;
static uint64_t g_ring_bytes;
static char g_shm_name[HV_SHM_NAME_MAX];
static int g_enabled = 1;

/* Set while telemetry is running on this thread. The pass-through to the real
 * allocator happens OUTSIDE the guard, so a guarded call still returns correct
 * memory; only the event is skipped. */
static HV_TLS int g_in_hook;

/* gettid() is a syscall. Cache it: allocation-heavy code would otherwise pay
 * for one syscall per event, which alone would blow the 50 ns budget. */
static HV_TLS uint32_t g_tid;

static uint32_t hv_tid(void) {
    if (g_tid == 0) g_tid = (uint32_t)gettid();
    return g_tid;
}

static uint64_t hv_now_ns(void) {
    struct timespec ts;
    /* vDSO on Linux: no syscall, roughly 20 ns. */
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * UINT64_C(1000000000) + (uint64_t)ts.tv_nsec;
}

static uint64_t hv_env_capacity(void) {
    const char *raw = getenv("HEAPVIZ_CAPACITY");
    uint64_t v;
    char *end;

    if (raw == NULL || *raw == '\0') return HV_DEFAULT_CAPACITY;
    v = (uint64_t)strtoull(raw, &end, 10);
    if (*end != '\0' || !hv_is_pow2(v) || v < 1024u) {
        hv_write_err("heapviz: HEAPVIZ_CAPACITY must be a power of two >= 1024; "
                     "using the default\n");
        return HV_DEFAULT_CAPACITY;
    }
    return v;
}

static void hv_read_comm(char *out, size_t outlen) {
    ssize_t n;
    size_t i;
    int fd = open("/proc/self/comm", O_RDONLY | O_CLOEXEC);

    memset(out, 0, outlen);
    if (fd < 0) return;
    n = read(fd, out, outlen - 1u);
    close(fd);
    if (n <= 0) { out[0] = '\0'; return; }
    for (i = 0; i < (size_t)n; i++)
        if (out[i] == '\n') { out[i] = '\0'; break; }
    out[outlen - 1u] = '\0';
}

static void hv_ring_setup(void) {
    const uint64_t capacity = hv_env_capacity();
    const uint64_t page = (uint64_t)sysconf(_SC_PAGESIZE);
    const int32_t pid = (int32_t)getpid();
    HvRingHeader *h;
    void *map;
    int fd;

    g_ring_bytes = hv_mapping_size(capacity, page);
    hv_shm_name(g_shm_name, sizeof(g_shm_name), pid);

    fd = shm_open(g_shm_name, O_CREAT | O_EXCL | O_RDWR, 0600);
    if (fd < 0 && errno == EEXIST) {
        /* A segment from a crashed run with the same pid. Clear it once. */
        shm_unlink(g_shm_name);
        fd = shm_open(g_shm_name, O_CREAT | O_EXCL | O_RDWR, 0600);
    }
    if (fd < 0) {
        hv_write_err("heapviz: shm_open failed; telemetry disabled\n");
        return;
    }
    if (ftruncate(fd, (off_t)g_ring_bytes) != 0) {
        hv_write_err("heapviz: ftruncate failed; telemetry disabled\n");
        close(fd);
        shm_unlink(g_shm_name);
        return;
    }

    map = mmap(NULL, (size_t)g_ring_bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd); /* the mapping outlives the descriptor */
    if (map == MAP_FAILED) {
        hv_write_err("heapviz: mmap failed; telemetry disabled\n");
        shm_unlink(g_shm_name);
        return;
    }

    h = (HvRingHeader *)map;
    memset(h, 0, sizeof(*h)); /* also clears every slot's commit bit */

    h->abi_version   = HEAPVIZ_ABI_VERSION;
    h->event_size    = (uint32_t)sizeof(HvEvent);
    h->capacity      = capacity;
    h->capacity_log2 = hv_log2_pow2(capacity);
    h->pid           = pid;
    h->flags         = 0;
    h->start_time_ns = hv_now_ns();
    hv_read_comm(h->comm, sizeof(h->comm));

    g_ring = h;

    /* Published last, with release ordering: a consumer that sees the magic is
     * guaranteed to see every field above it. */
    atomic_store_explicit(&h->magic, HEAPVIZ_ABI_MAGIC, memory_order_release);
}

/* Optional startup handshake.
 *
 * A short-lived target can run to completion and unlink its segment before any
 * consumer manages to open it, so its telemetry is simply lost. Setting
 * HEAPVIZ_WAIT_MS makes the constructor wait for a consumer to announce itself
 * first. This is what `heapviz -- ./prog` (ROADMAP.md M2.3) needs in order to
 * see a program's first allocations.
 *
 * Blocking here does not violate ground rule 2: that governs the per-allocation
 * hot path. This runs once, before main(), and only when explicitly asked for.
 */
static void hv_wait_for_consumer(HvRingHeader *h) {
    const char *raw = getenv("HEAPVIZ_WAIT_MS");
    long budget_ms;
    char *end;

    if (raw == NULL || *raw == '\0') return;
    budget_ms = strtol(raw, &end, 10);
    if (*end != '\0' || budget_ms <= 0) return;

    while (budget_ms > 0) {
        struct timespec ts;
        if (atomic_load_explicit(&h->consumer_pid, memory_order_acquire) != 0u)
            return;
        ts.tv_sec = 0;
        ts.tv_nsec = 500000L; /* 0.5 ms */
        nanosleep(&ts, NULL);
        budget_ms--; /* approximate: 2 polls per ms, close enough for a timeout */
    }
    hv_write_err("heapviz: no consumer attached within HEAPVIZ_WAIT_MS; "
                 "continuing without one\n");
}

static void hv_ring_teardown(void) {
    HvRingHeader *h = g_ring;
    if (h == NULL) return;

    /* Tell the consumer the target is gone before the mapping disappears, so it
     * can show "target exited" rather than a frozen frame. */
    atomic_store_explicit(&h->producer_exited, 1u, memory_order_release);

    g_ring = NULL;
    munmap(h, (size_t)g_ring_bytes);
    shm_unlink(g_shm_name);
}

/* After fork(), the child inherits the parent's mapping but has a different
 * pid, so it would publish into the parent's ring under the parent's name.
 * Detaching is the simplest correct answer and is what ROADMAP.md M1.7 calls
 * for; the child is simply not profiled. */
static void hv_atfork_child(void) {
    g_ring = NULL;
    g_tid = 0;
    g_in_hook = 0;
}

/* ------------------------------------------------------------------ */
/* M1.5  Event emission                                               */
/* ------------------------------------------------------------------ */

static uint32_t hv_usable(void *p) {
    size_t u;
    if (p == NULL || g_real_malloc_usable_size == NULL) return 0u;
    u = g_real_malloc_usable_size(p);
    /* Saturate rather than wrap: the TUI renders 0xFFFFFFFF as ">=4 GiB". */
    return (u > UINT32_MAX) ? UINT32_MAX : (uint32_t)u;
}

static void hv_emit(uint8_t op, const void *ptr, uint64_t size, uint32_t usable) {
    HvRingHeader *h = g_ring;
    if (h == NULL) return;
    (void)hv_ring_push(h, hv_now_ns(), (uint64_t)(uintptr_t)ptr, size, usable,
                       hv_tid(), op);
}

/* ------------------------------------------------------------------ */
/* M1.4  Interposed allocator functions                               */
/* ------------------------------------------------------------------ */

/* Each hook follows the same shape:
 *
 *   allocating call: run the real function first (we need the pointer), then
 *   emit. Nothing else can observe the pointer before we report it.
 *
 *   free: emit FIRST, then run the real function. Once the block is released,
 *   another thread can allocate the same address and emit its Malloc; reporting
 *   the Free afterwards would let the consumer see Malloc-before-Free for one
 *   address and conclude a double allocation.
 */

HV_EXPORT void *malloc(size_t size) {
    void *p;
    if (g_real_malloc == NULL) {
        hv_resolve_symbols();
        if (g_real_malloc == NULL) return hv_bootstrap_alloc(size);
    }
    p = g_real_malloc(size);
    if (p != NULL && !g_in_hook) {
        g_in_hook = 1;
        hv_emit(HV_OP_MALLOC, p, size, hv_usable(p));
        g_in_hook = 0;
    }
    return p;
}

HV_EXPORT void free(void *ptr) {
    if (ptr == NULL) return;
    /* Bootstrap blocks never came from libc; handing one to the real free is an
     * immediate abort. They are simply never reclaimed. */
    if (hv_is_bootstrap_ptr(ptr)) return;

    if (g_real_free == NULL) {
        hv_resolve_symbols();
        if (g_real_free == NULL) return;
    }
    if (!g_in_hook) {
        g_in_hook = 1;
        hv_emit(HV_OP_FREE, ptr, 0, hv_usable(ptr));
        g_in_hook = 0;
    }
    g_real_free(ptr);
}

HV_EXPORT void *calloc(size_t nmemb, size_t size) {
    size_t total;
    void *p;

    if (g_real_calloc == NULL) {
        hv_resolve_symbols();
        if (g_real_calloc == NULL) {
            /* This is the path dlsym itself takes. */
            if (__builtin_mul_overflow(nmemb, size, &total)) return NULL;
            p = hv_bootstrap_alloc(total);
            if (p != NULL) memset(p, 0, total);
            return p;
        }
    }
    p = g_real_calloc(nmemb, size);
    if (p != NULL && !g_in_hook) {
        g_in_hook = 1;
        if (__builtin_mul_overflow(nmemb, size, &total)) total = 0;
        hv_emit(HV_OP_CALLOC, p, total, hv_usable(p));
        g_in_hook = 0;
    }
    return p;
}

HV_EXPORT void *realloc(void *ptr, size_t size) {
    void *p;

    if (g_real_realloc == NULL) {
        hv_resolve_symbols();
        if (g_real_realloc == NULL) return NULL;
    }
    if (ptr == NULL) return malloc(size);

    if (hv_is_bootstrap_ptr(ptr)) {
        /* Migrate out of the arena into the real heap. The old block stays
         * where it is; the arena is never reclaimed. */
        const size_t old = hv_bootstrap_size(ptr);
        p = malloc(size);
        if (p != NULL) memcpy(p, ptr, (old < size) ? old : size);
        return p;
    }

    /* Report the free before the block can be reused, then report whatever
     * realloc hands back. ROADMAP.md decision D1: realloc is two events, so the
     * packet stays 32 bytes. If realloc fails, the original block is still
     * live, so re-report it rather than leaving the consumer thinking it died. */
    if (!g_in_hook) {
        g_in_hook = 1;
        hv_emit(HV_OP_FREE, ptr, 0, hv_usable(ptr));
        g_in_hook = 0;
    }
    p = g_real_realloc(ptr, size);
    if (!g_in_hook) {
        g_in_hook = 1;
        if (p != NULL) hv_emit(HV_OP_REALLOC, p, size, hv_usable(p));
        else           hv_emit(HV_OP_REALLOC, ptr, size, hv_usable(ptr));
        g_in_hook = 0;
    }
    return p;
}

HV_EXPORT int posix_memalign(void **memptr, size_t alignment, size_t size) {
    int rc;
    if (g_real_posix_memalign == NULL) {
        hv_resolve_symbols();
        if (g_real_posix_memalign == NULL) return ENOMEM;
    }
    rc = g_real_posix_memalign(memptr, alignment, size);
    if (rc == 0 && *memptr != NULL && !g_in_hook) {
        g_in_hook = 1;
        hv_emit(HV_OP_MEMALIGN, *memptr, size, hv_usable(*memptr));
        g_in_hook = 0;
    }
    return rc;
}

/* memalign, aligned_alloc, valloc, and pvalloc all differ only in how they
 * compute alignment, so they share one reporting path. */
#define HV_ALIGNED_HOOK(fn, realfn, ...)                                   \
    do {                                                                   \
        void *p_;                                                          \
        if ((realfn) == NULL) {                                            \
            hv_resolve_symbols();                                          \
            if ((realfn) == NULL) { errno = ENOMEM; return NULL; }         \
        }                                                                  \
        p_ = (realfn)(__VA_ARGS__);                                        \
        if (p_ != NULL && !g_in_hook) {                                    \
            g_in_hook = 1;                                                 \
            hv_emit(HV_OP_MEMALIGN, p_, size, hv_usable(p_));              \
            g_in_hook = 0;                                                 \
        }                                                                  \
        return p_;                                                         \
    } while (0)

HV_EXPORT void *aligned_alloc(size_t alignment, size_t size) {
    HV_ALIGNED_HOOK(aligned_alloc, g_real_aligned_alloc, alignment, size);
}

HV_EXPORT void *memalign(size_t alignment, size_t size) {
    HV_ALIGNED_HOOK(memalign, g_real_memalign, alignment, size);
}

HV_EXPORT void *valloc(size_t size) {
    HV_ALIGNED_HOOK(valloc, g_real_valloc, size);
}

HV_EXPORT void *pvalloc(size_t size) {
    HV_ALIGNED_HOOK(pvalloc, g_real_pvalloc, size);
}

/* ------------------------------------------------------------------ */
/* Lifecycle                                                          */
/* ------------------------------------------------------------------ */

/* Version probe, so the TUI can confirm which build a target loaded. */
HV_EXPORT uint32_t heapviz_preload_abi_version(void) {
    return HEAPVIZ_ABI_VERSION;
}

__attribute__((constructor(101)))
static void hv_preload_init(void) {
    const char *disable = getenv("HEAPVIZ_DISABLE");
    if (disable != NULL && disable[0] == '1') {
        g_enabled = 0;
        return;
    }
    hv_resolve_symbols();
    hv_ring_setup();
    if (g_ring != NULL) hv_wait_for_consumer(g_ring);
    pthread_atfork(NULL, NULL, hv_atfork_child);
}

__attribute__((destructor))
static void hv_preload_fini(void) {
    hv_ring_teardown();
}
