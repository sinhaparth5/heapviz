/* heapviz - ABI layout dump, compiled twice.
 *
 * Copyright (C) 2026 Parth Sinha
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This header is included by abi_layout_c.c (built as C11) and
 * abi_layout_cxx.cpp (built as C++20). Both binaries must print byte-identical
 * output. The test driver diffs them.
 *
 * The static asserts in heapviz_abi.h already catch drift at compile time. This
 * exists to catch the one thing a static assert cannot: C11 _Atomic and C++
 * std::atomic disagreeing about size or alignment, which would silently shift
 * every field after `magic` in one of the two binaries.
 */

#ifndef HEAPVIZ_TEST_ABI_LAYOUT_DUMP_H
#define HEAPVIZ_TEST_ABI_LAYOUT_DUMP_H

#include "common/heapviz_abi.h"

#include <stdio.h>

#ifdef __cplusplus
#  define HV_ALIGNOF(T) alignof(T)
#else
#  define HV_ALIGNOF(T) _Alignof(T)
#endif

#define HV_DUMP_FIELD(strct, field) \
    printf("%s.%s offset=%zu size=%zu\n", #strct, #field, \
           offsetof(strct, field), sizeof(((strct *)0)->field))

static void hv_dump_layout(void) {
    char name[HV_SHM_NAME_MAX];

    printf("abi_version %u\n", HEAPVIZ_ABI_VERSION);
    printf("abi_magic %llu\n", (unsigned long long)HEAPVIZ_ABI_MAGIC);
    printf("cacheline %u\n", HV_CACHELINE);

    printf("atomic_u64 size=%zu align=%zu\n",
           sizeof(HV_ATOMIC(uint64_t)), HV_ALIGNOF(HV_ATOMIC(uint64_t)));
    printf("atomic_u32 size=%zu align=%zu\n",
           sizeof(HV_ATOMIC(uint32_t)), HV_ALIGNOF(HV_ATOMIC(uint32_t)));

    printf("HvEvent size=%zu align=%zu\n",
           sizeof(HvEvent), HV_ALIGNOF(HvEvent));
    HV_DUMP_FIELD(HvEvent, timestamp);
    HV_DUMP_FIELD(HvEvent, ptr);
    HV_DUMP_FIELD(HvEvent, size);
    HV_DUMP_FIELD(HvEvent, usable);
    HV_DUMP_FIELD(HvEvent, tid);
    HV_DUMP_FIELD(HvEvent, op);

    printf("HvRingHeader size=%zu align=%zu\n",
           sizeof(HvRingHeader), HV_ALIGNOF(HvRingHeader));
    HV_DUMP_FIELD(HvRingHeader, magic);
    HV_DUMP_FIELD(HvRingHeader, abi_version);
    HV_DUMP_FIELD(HvRingHeader, event_size);
    HV_DUMP_FIELD(HvRingHeader, capacity);
    HV_DUMP_FIELD(HvRingHeader, pid);
    HV_DUMP_FIELD(HvRingHeader, flags);
    HV_DUMP_FIELD(HvRingHeader, start_time_ns);
    HV_DUMP_FIELD(HvRingHeader, comm);
    HV_DUMP_FIELD(HvRingHeader, head);
    HV_DUMP_FIELD(HvRingHeader, tail);
    HV_DUMP_FIELD(HvRingHeader, dropped);
    HV_DUMP_FIELD(HvRingHeader, total_events);
    HV_DUMP_FIELD(HvRingHeader, producer_exited);

    /* Sizing helpers must agree across languages too. */
    printf("mapping_size(1024,4096) %llu\n",
           (unsigned long long)hv_mapping_size(1024, 4096));
    printf("mapping_size(default,4096) %llu\n",
           (unsigned long long)hv_mapping_size(HV_DEFAULT_CAPACITY, 4096));
    printf("mapping_size(1024,65536) %llu\n",
           (unsigned long long)hv_mapping_size(1024, 65536));

    hv_shm_name(name, sizeof(name), 41820);
    printf("shm_name %s\n", name);
    hv_shm_name(name, sizeof(name), 1);
    printf("shm_name %s\n", name);
    hv_shm_name(name, sizeof(name), 4194304);
    printf("shm_name %s\n", name);

    /* tid packing is explicit little-endian; prove both halves agree. */
    {
        HvEvent e;
        hv_event_set_tid(&e, 0x123456u);
        printf("tid_roundtrip %u bytes=%02x%02x%02x\n",
               hv_event_get_tid(&e), e.tid[0], e.tid[1], e.tid[2]);
    }
}

#endif /* HEAPVIZ_TEST_ABI_LAYOUT_DUMP_H */
