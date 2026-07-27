/* heapviz - multi-producer ring stress test.
 *
 * Copyright (C) 2026 Parth Sinha
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * ROADMAP.md M1.8 asks for two threads and 10M events with zero loss. Since the
 * ring became multi-producer (see heapviz_ring.h), this runs N producer threads
 * against one consumer and checks three things:
 *
 *   1. Zero loss. Every event pushed is drained exactly once.
 *   2. No duplication or corruption. Each producer stamps its own id and a
 *      per-producer counter; the consumer checks every event it sees and
 *      confirms each producer's counters arrive complete and in order.
 *   3. Sequence integrity. Events drain in global claim order, so a producer's
 *      own events must arrive monotonically even when interleaved with others.
 *
 * Deliberately sized so the ring wraps many times: a capacity far below the
 * event count is what exercises the lap-parity bit. With a ring big enough to
 * hold every event, a broken parity check would still pass.
 *
 * Run under TSan (the asan preset builds this with -fsanitize=thread off, so
 * use a dedicated tsan build) to check the memory ordering itself.
 */

#include "common/heapviz_abi.h"
#include "common/heapviz_ring.h"

#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define RING_CAPACITY   4096u    /* small on purpose: forces many wraps */
#define MAX_PRODUCERS   8u
#define DRAIN_BATCH     256u

static HvRingHeader *g_ring;
static uint32_t g_producers;
static uint64_t g_per_producer;

/* Each producer stamps ptr = producer id, size = its own counter. */
static void *producer_main(void *arg) {
    const uint32_t id = (uint32_t)(uintptr_t)arg;
    uint64_t sent = 0;

    while (sent < g_per_producer) {
        /* Retry on a full ring: this test asserts ZERO loss, so the producer
         * must not give up. The interceptor does the opposite by design, since
         * it may never stall the target. */
        if (hv_ring_push(g_ring, sent, id, sent, 0u, id, HV_OP_MALLOC)) {
            sent++;
        } else {
            sched_yield();
        }
    }
    return NULL;
}

int main(int argc, char **argv) {
    uint64_t total_events = 10u * 1000u * 1000u;
    uint32_t producers = 4;
    pthread_t threads[MAX_PRODUCERS];
    HvEvent *batch;
    uint64_t *expected;   /* next counter value expected per producer */
    uint64_t *received;   /* events seen per producer */
    uint64_t drained = 0;
    uint64_t page, bytes;
    void *region;
    uint32_t i;
    int failures = 0;

    if (argc > 1) producers = (uint32_t)strtoul(argv[1], NULL, 10);
    if (argc > 2) total_events = strtoull(argv[2], NULL, 10);
    if (producers < 1u) producers = 1u;
    if (producers > MAX_PRODUCERS) producers = MAX_PRODUCERS;

    g_producers = producers;
    g_per_producer = total_events / producers;
    total_events = g_per_producer * producers;

    page = (uint64_t)sysconf(_SC_PAGESIZE);
    bytes = hv_mapping_size(RING_CAPACITY, page);

    /* Plain anonymous memory: this exercises the ring, not the shm plumbing. */
    region = aligned_alloc(64, (size_t)bytes);
    if (region == NULL) { perror("aligned_alloc"); return 1; }
    memset(region, 0, (size_t)bytes);

    g_ring = (HvRingHeader *)region;
    g_ring->capacity      = RING_CAPACITY;
    g_ring->capacity_log2 = hv_log2_pow2(RING_CAPACITY);
    g_ring->event_size    = (uint32_t)sizeof(HvEvent);
    g_ring->abi_version   = HEAPVIZ_ABI_VERSION;

    batch    = (HvEvent *)calloc(DRAIN_BATCH, sizeof(HvEvent));
    expected = (uint64_t *)calloc(producers, sizeof(uint64_t));
    received = (uint64_t *)calloc(producers, sizeof(uint64_t));
    if (!batch || !expected || !received) { perror("calloc"); return 1; }

    printf("ring_test: %u producers x %llu events (capacity %u, ~%llu wraps)\n",
           producers, (unsigned long long)g_per_producer, RING_CAPACITY,
           (unsigned long long)(total_events / RING_CAPACITY));

    for (i = 0; i < producers; i++) {
        if (pthread_create(&threads[i], NULL, producer_main,
                           (void *)(uintptr_t)i) != 0) {
            perror("pthread_create");
            return 1;
        }
    }

    /* Single consumer, draining until every event has been accounted for. */
    while (drained < total_events) {
        const uint32_t n = hv_ring_drain(g_ring, batch, DRAIN_BATCH);
        uint32_t k;

        if (n == 0) { sched_yield(); continue; }

        for (k = 0; k < n; k++) {
            const uint32_t id = (uint32_t)batch[k].ptr;

            if (id >= producers) {
                fprintf(stderr, "corrupt producer id %u at event %llu\n",
                        id, (unsigned long long)drained);
                if (++failures > 10) goto done;
                continue;
            }
            if (batch[k].size != expected[id]) {
                fprintf(stderr,
                        "producer %u: out-of-order or lost event: got %llu, "
                        "expected %llu\n", id,
                        (unsigned long long)batch[k].size,
                        (unsigned long long)expected[id]);
                if (++failures > 10) goto done;
            }
            if (hv_event_get_tid(&batch[k]) != id) {
                fprintf(stderr, "producer %u: tid mismatch (%u)\n", id,
                        hv_event_get_tid(&batch[k]));
                if (++failures > 10) goto done;
            }
            if ((batch[k].op & HV_OP_MASK) != HV_OP_MALLOC) {
                fprintf(stderr, "producer %u: op mismatch (%u)\n", id,
                        batch[k].op);
                if (++failures > 10) goto done;
            }
            expected[id]++;
            received[id]++;
        }
        drained += n;
    }

done:
    for (i = 0; i < producers; i++) pthread_join(threads[i], NULL);

    for (i = 0; i < producers; i++) {
        if (received[i] != g_per_producer) {
            fprintf(stderr, "producer %u: got %llu events, expected %llu\n", i,
                    (unsigned long long)received[i],
                    (unsigned long long)g_per_producer);
            failures++;
        }
    }
    if (HV_LOAD(&g_ring->dropped, HV_MO_RELAXED) != 0) {
        /* Producers retry, so nothing should be lost even though the counter
         * increments on each full-ring rejection. */
        printf("ring_test: %llu full-ring retries (expected under contention)\n",
               (unsigned long long)HV_LOAD(&g_ring->dropped, HV_MO_RELAXED));
    }

    free(batch); free(expected); free(received); free(region);

    if (failures != 0) {
        fprintf(stderr, "ring_test: FAILED with %d error(s)\n", failures);
        return 1;
    }
    printf("ring_test: %llu events, zero loss, order preserved\n",
           (unsigned long long)total_events);
    return 0;
}
