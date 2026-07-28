/* heapviz - a synthetic heap to drive the map with (M3.1 / M4.6).
 *
 * Copyright (C) 2026 Parth Sinha
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * M2.3 is what connects the map to a real target; until then there is no way to
 * look at M3 on a real terminal at all, and "the gutter labels are aligned" is
 * not a thing a unit test can tell you. This churns a 4 MiB address space
 * through the same Grid, HeatMap and MapView the real thing will use, so what is
 * on screen is the shipped code path with a fake event source rather than a
 * drawing of what it might look like.
 *
 * It also gives M4.6 its subject: heavy churn through a full-screen map is the
 * workload the 1 ms frame budget is supposed to survive. That is why this lives
 * in heapviz_core rather than inside main.cpp -- the number the roadmap records
 * has to come from the same object `--term-check` runs, or it is a measurement
 * of a copy.
 *
 * The one property worth preserving when editing: churning must not allocate.
 * `live_` is reserved once in the constructor and never grows past kMaxLive, so
 * a frame's worth of traffic is arithmetic and two vector writes. A benchmark
 * whose harness allocates is measuring its harness.
 */

#ifndef HEAPVIZ_TUI_DEMO_HEAP_H
#define HEAPVIZ_TUI_DEMO_HEAP_H

#include "tui/heatmap.h"
#include "tui/map_view.h"

#include <cstdint>
#include <vector>

namespace hv {

class DemoHeap {
public:
    static constexpr std::uint64_t kBase    = 0x55A0000000ull;
    static constexpr std::uint64_t kSpan    = 4u << 20;
    static constexpr std::size_t   kMaxLive = 4096;

    DemoHeap();

    /* Re-bucket for a new drawing area and replay what is live, so a resize
     * shows the heap at the new granularity rather than an empty map. */
    void fit(Rect area);

    /* One frame's worth of allocator traffic. Returns true if anything moved. */
    bool churn(std::uint32_t now_ms, unsigned ops);

    void seed(unsigned n) { churn(0, n); }

    const HeatMap &map() const noexcept { return map_; }
    const Grid    &grid() const noexcept { return grid_; }
    std::size_t    live_count() const noexcept { return live_.size(); }

private:
    std::uint64_t next() noexcept;

    struct Live { std::uint64_t addr; std::uint32_t size; std::uint32_t usable; };

    Grid              grid_;
    HeatMap           map_;
    std::vector<Live> live_;
    std::uint64_t     rng_    = 0x243F6A8885A308D3ull;
    std::uint32_t     now_ms_ = 0;
};

} // namespace hv

#endif /* HEAPVIZ_TUI_DEMO_HEAP_H */
