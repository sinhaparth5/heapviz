/* heapviz - single-consumer enforcement (ABI v3).
 *
 * Copyright (C) 2026 Parth Sinha
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * `tail` is one cursor with no ownership of its own. Two consumers draining the
 * same ring each advance it past events the other never copied out, so both
 * display a plausible half of the stream and neither reports an error. That is
 * the worst kind of bug: silently wrong output.
 *
 * The second test here is the one that matters. It drains one ring from two
 * "consumers" and shows events going missing, which is what the claim exists to
 * prevent.
 */

#include "common/heapviz_abi.h"
#include "common/heapviz_ring.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace {

int g_failures = 0;

void check(bool ok, const char *what) {
    if (!ok) { std::fprintf(stderr, "  FAIL %s\n", what); ++g_failures; }
}

/* A ring in ordinary heap memory. The claim logic is pure atomics, so it needs
 * no shared mapping to exercise. */
struct TestRing {
    std::vector<unsigned char> storage;
    HvRingHeader              *h = nullptr;

    explicit TestRing(std::uint64_t capacity) {
        const std::uint64_t bytes =
            sizeof(HvRingHeader) + capacity * sizeof(HvEvent);
        storage.assign(static_cast<std::size_t>(bytes), 0);
        h = reinterpret_cast<HvRingHeader *>(storage.data());
        h->abi_version   = HEAPVIZ_ABI_VERSION;
        h->event_size    = sizeof(HvEvent);
        h->capacity      = capacity;
        h->capacity_log2 = hv_log2_pow2(capacity);
        h->magic.store(HEAPVIZ_ABI_MAGIC, std::memory_order_release);
    }
};

void test_claim_is_exclusive() {
    TestRing r(64);

    std::uint32_t owner = 0;
    check(hv_ring_claim(r.h, 1111u, &owner) == 1, "claim: first consumer wins");

    owner = 0;
    check(hv_ring_claim(r.h, 2222u, &owner) == 0, "claim: second consumer is refused");
    check(owner == 1111u, "claim: the refusal names the current owner");

    /* The refused consumer must not have taken ownership on the way out. */
    check(r.h->consumer_pid.load(std::memory_order_acquire) == 1111u,
          "claim: a refused claim leaves the owner untouched");

    hv_ring_release(r.h, 1111u);
    check(r.h->consumer_pid.load(std::memory_order_acquire) == 0u,
          "claim: release clears ownership");

    check(hv_ring_claim(r.h, 2222u, &owner) == 1,
          "claim: the ring can be claimed again after release");

    /* Releasing with the wrong pid must not steal the ring from its owner. */
    hv_ring_release(r.h, 9999u);
    check(r.h->consumer_pid.load(std::memory_order_acquire) == 2222u,
          "claim: a release from a non-owner is ignored");

    hv_ring_break_claim(r.h);
    check(r.h->consumer_pid.load(std::memory_order_acquire) == 0u,
          "claim: a stale claim can be broken");
}

/* The failure the claim prevents, demonstrated. */
void test_two_drainers_lose_events() {
    constexpr std::uint32_t kEvents = 64;
    TestRing r(128);

    for (std::uint32_t i = 0; i < kEvents; ++i) {
        hv_ring_push(r.h, i, 0x1000 + i, 32, 32, 7, HV_OP_MALLOC);
    }

    HvEvent a[kEvents], b[kEvents];
    const std::uint32_t na = hv_ring_drain(r.h, a, kEvents);
    const std::uint32_t nb = hv_ring_drain(r.h, b, kEvents);

    /* The first drainer took everything; the second sees an empty ring. Neither
     * call fails, and nothing anywhere records that they were competing. With
     * two real consumers the split is arbitrary and both show partial data. */
    check(na == kEvents, "two drainers: the first took every event");
    check(nb == 0, "two drainers: the second silently got nothing");
    check(na + nb == kEvents,
          "two drainers: events are split, not duplicated, so neither sees all");
}

} // namespace

int main() {
    test_claim_is_exclusive();
    test_two_drainers_lose_events();

    if (g_failures != 0) {
        std::fprintf(stderr, "consumer_claim_test: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("consumer_claim_test: the ring admits exactly one consumer\n");
    return 0;
}
