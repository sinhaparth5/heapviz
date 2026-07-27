/* heapviz - shared memory ABI between libheapviz.so and the heapviz TUI.
 *
 * Copyright (C) 2026 Parth Sinha
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This header is compiled by BOTH halves of the project:
 *
 *   - libheapviz.so, in C11, inside the target process
 *   - heapviz, in C++20, in a separate process
 *
 * The two binaries are built separately and may be at different versions on a
 * user's machine, so everything below is a wire format. Changing any field
 * offset, any size, or the meaning of any value is an ABI break: bump
 * HEAPVIZ_ABI_VERSION and say so in CHANGELOG.md.
 *
 * Every layout guarantee is enforced with a static assert at the bottom of this
 * file. Both binaries compile them, so a drift is a compile error rather than
 * silent cross-process corruption.
 */

#ifndef HEAPVIZ_ABI_H
#define HEAPVIZ_ABI_H

#include <stddef.h>
#include <stdint.h>

/* ------------------------------------------------------------------ */
/* C11 / C++20 compatibility                                          */
/* ------------------------------------------------------------------ */

#ifdef __cplusplus
#  include <atomic>
#  define HV_ATOMIC(T)          std::atomic<T>
#  define HV_ALIGNAS(n)         alignas(n)
#  define HV_STATIC_ASSERT(c,m) static_assert(c, m)
#  define HV_INLINE             inline
extern "C" {
#else
#  include <stdalign.h>
#  include <stdatomic.h>
#  define HV_ATOMIC(T)          _Atomic T
#  define HV_ALIGNAS(n)         alignas(n)
#  define HV_STATIC_ASSERT(c,m) _Static_assert(c, m)
#  define HV_INLINE             static inline
#endif

/* std::atomic<uint64_t> and _Atomic uint64_t are layout-compatible on the
 * gcc/clang Linux targets we support: both are 8 bytes, 8-byte aligned, and
 * lock-free. tests/abi_layout proves it at runtime by diffing the layout dump
 * from a C11 build against the C++20 build. */

/* ------------------------------------------------------------------ */
/* Versioning                                                         */
/* ------------------------------------------------------------------ */

/* "HPZV1" - published to the ring header LAST, with a release store. Its
 * presence is what tells a consumer the region is fully initialised. */
#define HEAPVIZ_ABI_MAGIC   UINT64_C(0x48505A5631000000)
#define HEAPVIZ_ABI_VERSION 1u

#define HV_CACHELINE 64u

/* ------------------------------------------------------------------ */
/* Event packet - exactly 32 bytes                                    */
/* ------------------------------------------------------------------ */

/* Operation codes. Stored as uint8_t so the value is the wire format; do not
 * reorder or reuse a retired number. */
enum {
    HV_OP_MALLOC   = 0,
    HV_OP_FREE     = 1,
    HV_OP_CALLOC   = 2,
    HV_OP_REALLOC  = 3,
    HV_OP_MEMALIGN = 4,
    HV_OP__COUNT
};

/* One allocator event.
 *
 * `ptr` is uint64_t rather than uintptr_t on purpose: a wire format wants a
 * fixed width independent of the compiling platform's pointer size.
 *
 * `usable` is malloc_usable_size() truncated to 32 bits. Allocations above 4 GiB
 * saturate; the TUI shows them as ">=4 GiB" rather than pretending to precision
 * it does not have.
 *
 * `tid` is the low 24 bits of the thread id, stored little-endian explicitly so
 * the format does not depend on host byte order. 24 bits covers the default
 * pid_max of 4194304.
 *
 * realloc is emitted as two events (Free then Malloc) rather than widening this
 * packet to carry both pointers. See ROADMAP.md decision D1.
 */
typedef struct HvEvent {
    uint64_t timestamp; /*  0: CLOCK_MONOTONIC nanoseconds              */
    uint64_t ptr;       /*  8: user pointer returned to / passed by app */
    uint64_t size;      /* 16: requested bytes (0 for free)             */
    uint32_t usable;    /* 24: malloc_usable_size, truncated            */
    uint8_t  tid[3];    /* 28: low 24 bits of thread id, little-endian  */
    uint8_t  op;        /* 31: one of HV_OP_*                           */
} HvEvent;

HV_INLINE void hv_event_set_tid(HvEvent *e, uint32_t tid) {
    e->tid[0] = (uint8_t)(tid & 0xFFu);
    e->tid[1] = (uint8_t)((tid >> 8) & 0xFFu);
    e->tid[2] = (uint8_t)((tid >> 16) & 0xFFu);
}

HV_INLINE uint32_t hv_event_get_tid(const HvEvent *e) {
    return (uint32_t)e->tid[0]
         | ((uint32_t)e->tid[1] << 8)
         | ((uint32_t)e->tid[2] << 16);
}

/* ------------------------------------------------------------------ */
/* Ring header - 256 bytes, four cache lines                          */
/* ------------------------------------------------------------------ */

/* Four separately aligned blocks. head and tail MUST NOT share a cache line:
 * the producer stores to head on every event while the consumer stores to tail
 * on every drain, and false sharing between them is the classic way to make an
 * SPSC ring slower than a mutex.
 *
 * Block 0 is written once at setup and read-only afterwards.
 * Block 1 is producer-write, consumer-read.
 * Block 2 is consumer-write, producer-read.
 * Block 3 is producer-write, consumer-read (diagnostics only).
 */
typedef struct HvRingHeader {
    /* --- block 0: setup, immutable once magic is published --------- */
    HV_ALIGNAS(HV_CACHELINE) HV_ATOMIC(uint64_t) magic;
    uint32_t abi_version;
    uint32_t event_size;    /* sizeof(HvEvent); cross-checks the ABI  */
    uint64_t capacity;      /* slots, power of two                     */
    int32_t  pid;           /* producer pid, matches the shm name      */
    uint32_t flags;         /* reserved, must be 0                     */
    uint64_t start_time_ns; /* CLOCK_MONOTONIC at producer init        */
    char     comm[16];      /* producer comm, NUL-padded               */

    /* --- block 1: producer writes ---------------------------------- */
    HV_ALIGNAS(HV_CACHELINE) HV_ATOMIC(uint64_t) head;

    /* --- block 2: consumer writes ---------------------------------- */
    HV_ALIGNAS(HV_CACHELINE) HV_ATOMIC(uint64_t) tail;

    /* --- block 3: producer diagnostics ----------------------------- */
    HV_ALIGNAS(HV_CACHELINE) HV_ATOMIC(uint64_t) dropped;
    HV_ATOMIC(uint64_t) total_events;
    HV_ATOMIC(uint32_t) producer_exited;
} HvRingHeader;

/* ------------------------------------------------------------------ */
/* Sizing helpers                                                     */
/* ------------------------------------------------------------------ */

/* 1 Mi events x 32 B = 32 MiB of shared memory. */
#define HV_DEFAULT_CAPACITY (UINT64_C(1) << 20)

HV_INLINE int hv_is_pow2(uint64_t v) {
    return v != 0 && (v & (v - 1)) == 0;
}

/* Bytes to map for a ring of `capacity` events, rounded up to `page_size`.
 * page_size is a parameter rather than a constant because arm64 kernels are
 * built with 4K, 16K, or 64K pages; callers pass sysconf(_SC_PAGESIZE). */
HV_INLINE uint64_t hv_mapping_size(uint64_t capacity, uint64_t page_size) {
    uint64_t raw = (uint64_t)sizeof(HvRingHeader)
                 + capacity * (uint64_t)sizeof(HvEvent);
    return (raw + page_size - 1) & ~(page_size - 1);
}

/* Slot address for a monotonically increasing sequence number. Capacity is a
 * power of two, so this is a mask rather than a modulo. */
HV_INLINE HvEvent *hv_ring_slot(HvRingHeader *h, uint64_t seq) {
    HvEvent *base = (HvEvent *)((unsigned char *)h + sizeof(HvRingHeader));
    return base + (seq & (h->capacity - 1));
}

/* ------------------------------------------------------------------ */
/* Shared memory name                                                 */
/* ------------------------------------------------------------------ */

#define HV_SHM_NAME_PREFIX "/heapviz_shm_"
#define HV_SHM_NAME_MAX    32

/* Formats "/heapviz_shm_<pid>" into buf. Hand-rolled rather than snprintf
 * because this runs inside the interceptor, where ground rule #1 forbids any
 * path that might reach malloc. Returns the string length. */
HV_INLINE size_t hv_shm_name(char *buf, size_t buflen, int32_t pid) {
    static const char prefix[] = HV_SHM_NAME_PREFIX;
    const size_t plen = sizeof(prefix) - 1;
    char digits[16];
    size_t n = 0, i = 0;
    uint32_t v;

    if (buflen < HV_SHM_NAME_MAX) {
        if (buflen > 0) buf[0] = '\0';
        return 0;
    }
    for (i = 0; i < plen; i++) buf[i] = prefix[i];

    v = (pid < 0) ? 0u : (uint32_t)pid;
    do { digits[n++] = (char)('0' + (v % 10u)); v /= 10u; } while (v != 0u);
    while (n > 0) buf[i++] = digits[--n];
    buf[i] = '\0';
    return i;
}

/* ------------------------------------------------------------------ */
/* Layout guarantees - the actual ABI contract                        */
/* ------------------------------------------------------------------ */

HV_STATIC_ASSERT(sizeof(HvEvent) == 32, "HvEvent must be exactly 32 bytes");
HV_STATIC_ASSERT(offsetof(HvEvent, timestamp) ==  0, "HvEvent.timestamp @ 0");
HV_STATIC_ASSERT(offsetof(HvEvent, ptr)       ==  8, "HvEvent.ptr @ 8");
HV_STATIC_ASSERT(offsetof(HvEvent, size)      == 16, "HvEvent.size @ 16");
HV_STATIC_ASSERT(offsetof(HvEvent, usable)    == 24, "HvEvent.usable @ 24");
HV_STATIC_ASSERT(offsetof(HvEvent, tid)       == 28, "HvEvent.tid @ 28");
HV_STATIC_ASSERT(offsetof(HvEvent, op)        == 31, "HvEvent.op @ 31");

HV_STATIC_ASSERT(sizeof(HvRingHeader) == 256, "HvRingHeader must be 256 bytes");
HV_STATIC_ASSERT(offsetof(HvRingHeader, magic)           ==   0, "magic @ 0");
HV_STATIC_ASSERT(offsetof(HvRingHeader, abi_version)     ==   8, "abi_version @ 8");
HV_STATIC_ASSERT(offsetof(HvRingHeader, event_size)      ==  12, "event_size @ 12");
HV_STATIC_ASSERT(offsetof(HvRingHeader, capacity)        ==  16, "capacity @ 16");
HV_STATIC_ASSERT(offsetof(HvRingHeader, pid)             ==  24, "pid @ 24");
HV_STATIC_ASSERT(offsetof(HvRingHeader, flags)           ==  28, "flags @ 28");
HV_STATIC_ASSERT(offsetof(HvRingHeader, start_time_ns)   ==  32, "start_time_ns @ 32");
HV_STATIC_ASSERT(offsetof(HvRingHeader, comm)            ==  40, "comm @ 40");
HV_STATIC_ASSERT(offsetof(HvRingHeader, head)            ==  64, "head @ 64");
HV_STATIC_ASSERT(offsetof(HvRingHeader, tail)            == 128, "tail @ 128");
HV_STATIC_ASSERT(offsetof(HvRingHeader, dropped)         == 192, "dropped @ 192");
HV_STATIC_ASSERT(offsetof(HvRingHeader, total_events)    == 200, "total_events @ 200");
HV_STATIC_ASSERT(offsetof(HvRingHeader, producer_exited) == 208, "producer_exited @ 208");

/* head and tail on separate cache lines - the whole point of the padding. */
HV_STATIC_ASSERT(offsetof(HvRingHeader, tail) - offsetof(HvRingHeader, head)
                 >= HV_CACHELINE, "head and tail must not share a cache line");

HV_STATIC_ASSERT(sizeof(HvRingHeader) % HV_CACHELINE == 0,
                 "header must be a whole number of cache lines so slot 0 is aligned");
HV_STATIC_ASSERT(HV_CACHELINE % sizeof(HvEvent) == 0,
                 "events must tile cache lines evenly");
HV_STATIC_ASSERT(sizeof(HvEvent) % 8 == 0, "HvEvent needs 8-byte alignment");

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* HEAPVIZ_ABI_H */
