/* heapviz - a synthetic heap to drive the map with (M3.1 / M4.6).
 *
 * Copyright (C) 2026 Parth Sinha
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "tui/demo_heap.h"

namespace hv {

DemoHeap::DemoHeap() {
    live_.reserve(kMaxLive); /* once, so churning allocates nothing */
    grid_.set_bounds(kBase, kBase + kSpan);
}

void DemoHeap::fit(Rect area, bool half_block) {
    fit_grid(grid_, area, half_block);
    map_.configure(grid_);
    /* A granularity change invalidates every aggregate, so replay what is
     * live rather than showing an empty map until the next allocation. */
    for (const Live &c : live_)
        map_.on_alloc(c.addr, c.size, c.usable, now_ms_);
}

bool DemoHeap::churn(std::uint32_t now_ms, unsigned ops) {
    now_ms_ = now_ms;
    for (unsigned i = 0; i < ops; ++i) {
        const bool freeing = !live_.empty() &&
                             (live_.size() >= kMaxLive || (next() & 3u) == 0);
        if (freeing) {
            const std::size_t k = next() % live_.size();
            const Live c = live_[k];
            map_.on_free(c.addr, c.size, c.usable, now_ms);
            live_[k] = live_.back();
            live_.pop_back();
        } else {
            const auto size = static_cast<std::uint32_t>(32 + (next() % 2000));
            const std::uint32_t usable = (size + 31u) & ~31u;
            const std::uint64_t addr =
                kBase + ((next() % (kSpan - usable)) & ~std::uint64_t{15});
            map_.on_alloc(addr, size, usable, now_ms);
            live_.push_back(Live{addr, size, usable});
        }
    }
    return ops != 0;
}

std::uint64_t DemoHeap::next() noexcept {
    rng_ += 0x9E3779B97F4A7C15ull;
    std::uint64_t z = rng_;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

} // namespace hv
