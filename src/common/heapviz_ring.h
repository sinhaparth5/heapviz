/* heapviz - lock-free multi-producer / single-consumer event ring.
 *
 * Copyright (C) 2026 Parth Sinha
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Producers are every thread in the target process that calls an allocator
 * function. The consumer is the heapviz TUI in a separate process. Both compile
 * this header: the producer side from C11, the consumer side from C++20.
 *
 * WHY NOT SPSC
 * ------------
 * ROADMAP.md M1.6 specifies a single-producer ring, with `head` loaded relaxed
 * "because we are the only writer". That holds only for a single-threaded
 * target. Any threaded program has one producer per thread, and two threads
 * claiming the same head would write the same slot and lose both events. Since
 * M1.8 requires the interceptor to run clean under a multi-threaded workload,
 * the ring has to be multi-producer.
 *
 * HOW A SLOT IS PUBLISHED
 * -----------------------
 * Claiming is a CAS loop on `head`. That is lock-free but not wait-free: a
 * thread may retry, though only because another thread made progress. No thread
 * can ever block another indefinitely, and nothing here takes a lock, so ground
 * rule 2 holds.
 *
 * Publication cannot ride on `head` the way it does in an SPSC ring, because
 * with several producers in flight `head` runs ahead of the slots that are
 * actually filled. Each slot publishes itself instead, through two flag bits in
 * the `op` byte (see heapviz_abi.h):
 *
 *   - COMMIT says a producer finished writing this slot.
 *   - PARITY says which lap around the ring that write belongs to.
 *
 * The consumer at sequence s accepts a slot only when COMMIT is set and PARITY
 * equals s's own lap. Without PARITY it would accept a committed event left
 * over from the previous lap; without COMMIT it would accept the zero-filled
 * region on lap 0, where parity happens to match.
 *
 * A stalled producer holding slot s blocks the consumer at s even if later
 * slots are committed. That is head-of-line blocking, and it is the correct
 * behaviour: events must reach the model in sequence order. The window is the
 * handful of nanoseconds between claiming a slot and committing it.
 */

#ifndef HEAPVIZ_RING_H
#define HEAPVIZ_RING_H

#include "common/heapviz_abi.h"

#ifdef __cplusplus
extern "C" {
#endif

/* HvEvent.op is a plain uint8_t so the struct stays a POD wire format in both
 * languages, which rules out _Atomic / std::atomic on the field itself. The
 * GCC/Clang __atomic builtins give the ordering we need on a plain object and
 * work identically from C and C++. Both compilers are already required. */
#define HV_STORE_RELEASE_U8(p, v) __atomic_store_n((p), (v), __ATOMIC_RELEASE)
#define HV_LOAD_ACQUIRE_U8(p)     __atomic_load_n((p), __ATOMIC_ACQUIRE)

/* ------------------------------------------------------------------ */
/* Producer                                                           */
/* ------------------------------------------------------------------ */

/* Publishes one event. Returns 1 if it landed, 0 if the ring was full.
 *
 * Never blocks and never allocates. On a full ring it bumps `dropped` and
 * returns immediately: losing telemetry is always preferable to stalling the
 * program under inspection. The TUI surfaces the drop count so the loss is
 * visible rather than silent.
 */
HV_INLINE int hv_ring_push(HvRingHeader *h,
                           uint64_t timestamp,
                           uint64_t ptr,
                           uint64_t size,
                           uint32_t usable,
                           uint32_t tid,
                           uint8_t  opcode) {
    const uint64_t capacity = h->capacity;
    const uint32_t shift    = h->capacity_log2;
    uint64_t seq;
    HvEvent *slot;
    uint8_t published;

    for (;;) {
        uint64_t head = HV_LOAD(&h->head, HV_MO_RELAXED);
        uint64_t tail = HV_LOAD(&h->tail, HV_MO_ACQUIRE);

        if (head - tail >= capacity) {
            HV_FETCH_ADD(&h->dropped, 1, HV_MO_RELAXED);
            return 0;
        }
        /* Claim exactly one slot. Weak CAS is fine inside a retry loop. */
        if (HV_CAS_WEAK(&h->head, &head, head + 1,
                        HV_MO_RELAXED, HV_MO_RELAXED)) {
            seq = head;
            break;
        }
    }

    slot = hv_ring_slot(h, seq);
    slot->timestamp = timestamp;
    slot->ptr       = ptr;
    slot->size      = size;
    slot->usable    = usable;
    hv_event_set_tid(slot, tid);

    published = (uint8_t)((opcode & HV_OP_MASK) | HV_OP_COMMIT_BIT
                          | (uint8_t)(((seq >> shift) & 1u) ? HV_OP_PARITY_BIT : 0u));

    /* Release: every field above must be visible to any consumer that observes
     * this store. Pairs with HV_LOAD_ACQUIRE_U8 in hv_ring_drain. */
    HV_STORE_RELEASE_U8(&slot->op, published);

    HV_FETCH_ADD(&h->total_events, 1, HV_MO_RELAXED);
    return 1;
}

/* ------------------------------------------------------------------ */
/* Consumer                                                           */
/* ------------------------------------------------------------------ */

/* Drains up to `max` events into `out`, in sequence order. Returns the count.
 *
 * Stops early at the first slot whose producer has not committed yet, so a
 * short return does not mean the ring is empty. Batching keeps the atomic
 * traffic to one tail store per drain instead of one per event.
 */
HV_INLINE uint32_t hv_ring_drain(HvRingHeader *h, HvEvent *out, uint32_t max) {
    const uint32_t shift = h->capacity_log2;
    uint64_t tail = HV_LOAD(&h->tail, HV_MO_RELAXED);
    const uint64_t head = HV_LOAD(&h->head, HV_MO_ACQUIRE);
    uint32_t n = 0;

    while (n < max && tail != head) {
        HvEvent *slot = hv_ring_slot(h, tail);
        const uint8_t op = HV_LOAD_ACQUIRE_U8(&slot->op);
        const uint8_t want_parity =
            (uint8_t)(((tail >> shift) & 1u) ? HV_OP_PARITY_BIT : 0u);

        if ((op & HV_OP_COMMIT_BIT) == 0)               break; /* in flight   */
        if ((op & HV_OP_PARITY_BIT) != want_parity)     break; /* previous lap */

        out[n] = *slot;
        out[n].op = (uint8_t)(op & HV_OP_MASK); /* hand the model a clean op */
        n++;
        tail++;
    }

    if (n != 0) {
        /* Release: the producer's acquire load of tail must not see this
         * advance before we have finished copying the slots out. */
        HV_STORE(&h->tail, tail, HV_MO_RELEASE);
    }
    return n;
}

/* Events claimed but not yet consumed. Drives the "Telemetry Ring" metric. */
HV_INLINE uint64_t hv_ring_used(const HvRingHeader *h) {
    const uint64_t head = HV_LOAD(&h->head, HV_MO_RELAXED);
    const uint64_t tail = HV_LOAD(&h->tail, HV_MO_RELAXED);
    return head - tail;
}

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* HEAPVIZ_RING_H */
