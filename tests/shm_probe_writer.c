/* heapviz - shared memory round-trip, producer half (C11).
 *
 * Copyright (C) 2026 Parth Sinha
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Creates a ring segment, fills the header, writes a deterministic run of
 * events, then publishes the magic with a release store and exits WITHOUT
 * unlinking. shm_probe_reader (built as C++20) validates and unlinks.
 *
 * This is the M0 definition-of-done check: both binaries map the same region
 * and agree on every offset. The real producer lands in M1.7.
 */

#include "common/heapviz_abi.h"

#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define PROBE_CAPACITY 1024u
#define PROBE_EVENTS   64u

/* Deterministic, so the reader can assert exact values. Shared with the reader
 * by construction: if these drift, the test fails loudly. */
static void fill_event(HvEvent *e, uint32_t i) {
    e->timestamp = UINT64_C(1000000) + i;
    e->ptr       = UINT64_C(0x0000dead00000000) + (uint64_t)i * 16u;
    e->size      = (uint64_t)(i + 1u) * 64u;
    e->usable    = (i + 1u) * 64u + 8u;
    e->op        = (uint8_t)(i % (uint32_t)HV_OP__COUNT);
    hv_event_set_tid(e, 0x100u + i);
}

int main(void) {
    char name[HV_SHM_NAME_MAX];
    const int32_t pid = (int32_t)getpid();
    const uint64_t page = (uint64_t)sysconf(_SC_PAGESIZE);
    const uint64_t bytes = hv_mapping_size(PROBE_CAPACITY, page);
    struct timespec ts;
    HvRingHeader *h;
    void *map;
    int fd;
    uint32_t i;

    hv_shm_name(name, sizeof(name), pid);

    fd = shm_open(name, O_CREAT | O_EXCL | O_RDWR, 0600);
    if (fd < 0) {
        /* A stale segment from a crashed run. Clear it and retry once. */
        shm_unlink(name);
        fd = shm_open(name, O_CREAT | O_EXCL | O_RDWR, 0600);
    }
    if (fd < 0) {
        perror("writer: shm_open");
        return 1;
    }
    if (ftruncate(fd, (off_t)bytes) != 0) {
        perror("writer: ftruncate");
        shm_unlink(name);
        return 1;
    }

    map = mmap(NULL, (size_t)bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd); /* the mapping outlives the descriptor */
    if (map == MAP_FAILED) {
        perror("writer: mmap");
        shm_unlink(name);
        return 1;
    }

    h = (HvRingHeader *)map;
    memset(h, 0, sizeof(*h));

    h->abi_version = HEAPVIZ_ABI_VERSION;
    h->event_size  = (uint32_t)sizeof(HvEvent);
    h->capacity    = PROBE_CAPACITY;
    h->pid         = pid;
    h->flags       = 0;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    h->start_time_ns = (uint64_t)ts.tv_sec * UINT64_C(1000000000)
                     + (uint64_t)ts.tv_nsec;
    memcpy(h->comm, "shm_probe", sizeof("shm_probe"));

    for (i = 0; i < PROBE_EVENTS; i++)
        fill_event(hv_ring_slot(h, i), i);

    atomic_store_explicit(&h->head, PROBE_EVENTS, memory_order_relaxed);
    atomic_store_explicit(&h->tail, 0, memory_order_relaxed);
    atomic_store_explicit(&h->dropped, 7, memory_order_relaxed);
    atomic_store_explicit(&h->total_events, PROBE_EVENTS + 7, memory_order_relaxed);
    atomic_store_explicit(&h->producer_exited, 0, memory_order_relaxed);

    /* Publish last, with release ordering: everything above must be visible to
     * any consumer that observes the magic. */
    atomic_store_explicit(&h->magic, HEAPVIZ_ABI_MAGIC, memory_order_release);

    munmap(map, (size_t)bytes);

    /* The reader needs the pid to reconstruct the segment name. */
    printf("%d\n", (int)pid);
    return 0;
}
