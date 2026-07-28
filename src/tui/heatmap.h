/* heapviz - per-cell aggregation (M3.3).
 *
 * Copyright (C) 2026 Parth Sinha
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * A cell covers many bytes and therefore many chunks, but it gets exactly one
 * glyph and one colour. This is what collapses "everything that happened in
 * this 4 KB of address space" into that one answer.
 *
 * INCREMENTAL, NOT REBUILT
 * ------------------------
 * The aggregates are folded forward as events arrive, never recomputed per
 * frame. A full rebuild is O(table), and at 60 fps against a million live
 * chunks that is the entire frame budget spent re-deriving something that
 * changed in a handful of cells. `rebuild` exists for the one case where the
 * incremental path cannot work -- the granularity changed, so every address
 * maps somewhere new -- and `rebuilds()` is exposed so a test can prove it is
 * not being called per frame.
 *
 * That makes the incremental path the one that has to be right, and the two
 * paths agreeing is not obvious: they compute the same numbers by completely
 * different routes. `heatmap_test` therefore replays a long event stream
 * incrementally and requires the result to be identical to a rebuild from the
 * chunk table.
 *
 * NO STORED STATE, NO TIMERS
 * --------------------------
 * A cell's state is a pure function of its aggregates and the current time,
 * evaluated at render time. Nothing here decays, expires, or is swept: a cell
 * that flashed red 300 ms ago stops being red because the clock moved, not
 * because something updated it. That is what M3.4's colour aging is built on,
 * and it is why there is no per-cell animation state machine to keep in sync.
 */

#ifndef HEAPVIZ_TUI_HEATMAP_H
#define HEAPVIZ_TUI_HEATMAP_H

#include "tui/chunk_table.h"
#include "tui/grid.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace hv {

/* Timestamps are milliseconds since the session began, and zero is a perfectly
 * ordinary value there -- it is the first millisecond. "Never happened" needs
 * its own value rather than borrowing zero. */
constexpr std::uint32_t kNoTime = 0xFFFFFFFFu;

/* How long an event keeps a cell in its flash state. M3.4 owns the colour ramps
 * these drive; the durations live here because the precedence below is what
 * first needs them, and one definition is better than two that drift.
 *
 * Free is louder and lasts longer than malloc on purpose: an allocation landing
 * is expected and constant, whereas memory going away is the thing a person is
 * usually watching for. */
struct HeatTimings {
    std::uint32_t malloc_pulse_ms = 200;
    std::uint32_t free_flash_ms   = 300;
};

constexpr HeatTimings kDefaultTimings{};

/* Order matters: this is the precedence, highest first. ROADMAP M3.3 fixes it
 * as recent free > recent malloc > live > overhead > empty. */
enum class CellState : std::uint8_t {
    Empty = 0,
    Overhead,     /* only chunk-header bytes reach this cell, no payload */
    Live,
    RecentMalloc,
    RecentFree,
};

const char *cell_state_str(CellState s) noexcept;


/* 24 bytes. Everything a cell's colour needs and nothing that can go stale.
 *
 * `live_bytes` is usable bytes rather than requested, because it answers the
 * question the display is actually asking -- how much of this cell is spoken
 * for -- and the gap between the two is `overhead_bytes`. */
struct CellAggregate {
    std::uint64_t live_bytes     = 0;
    std::uint32_t n_live         = 0;
    std::uint32_t overhead_bytes = 0;
    std::uint32_t last_alloc_ms  = kNoTime;
    std::uint32_t last_free_ms   = kNoTime;
};

/* The precedence rule itself, as a free function over an aggregate.
 *
 * Separated from the map so the whole table can be tested directly. One level
 * cannot otherwise be reached through the event path: `Overhead` needs a cell
 * holding chunk-header bytes and no payload, which only arises once M2.2
 * decodes real ptmalloc headers -- until then overhead always arrives attached
 * to a live allocation, which outranks it. */
CellState cell_state(const CellAggregate &a, const HeatTimings &t,
                     std::uint32_t now_ms) noexcept;

class HeatMap {
public:
    /* Points the map at a grid. Resizes and clears only when the geometry
     * actually changed, so a caller may hand this the same grid every frame
     * without paying for it -- which is exactly what the event loop does after
     * a SIGWINCH it turns out did not change anything. Returns true if a
     * rebuild is now needed. */
    bool configure(const Grid &g);

    /* Recomputes every cell from the chunk table. The only correct response to
     * a granularity change, and the only place the whole grid is touched.
     *
     * Exact only for chunks the table still holds: a freed record evicted under
     * the bounded-memory policy takes its `last_free_ms` with it, so a rebuild
     * after an eviction legitimately differs from the incremental state. That is
     * the cost of the memory cap, not a bug in either path. */
    void rebuild(const ChunkTable &table);

    /* Fold one event in. Both are no-ops for an address outside the grid, which
     * is ordinary: the target allocates outside the displayed region constantly.
     *
     * `usable` and `size` are the allocator's figure and the caller's request;
     * the difference is the overhead this cell carries. Until M2.2 decodes real
     * ptmalloc chunk headers, that difference is the best available proxy for
     * them. */
    void on_alloc(std::uint64_t ptr, std::uint32_t size, std::uint32_t usable,
                  std::uint32_t now_ms) noexcept;
    void on_free(std::uint64_t ptr, std::uint32_t size, std::uint32_t usable,
                 std::uint32_t now_ms) noexcept;

    /* The precedence rule. Pure in (aggregate, now), so the same cell yields a
     * different answer as the clock moves with nothing having updated it. */
    CellState state_at(std::size_t index, std::uint32_t now_ms) const noexcept;

    const CellAggregate &at(std::size_t index) const noexcept;
    std::size_t cell_count() const noexcept { return cells_.size(); }
    const Grid &grid() const noexcept { return grid_; }

    void set_timings(const HeatTimings &t) noexcept { timings_ = t; }
    const HeatTimings &timings() const noexcept { return timings_; }

    /* Rolling totals, for the header bar and for tests that need to know the
     * whole-map answer without walking every cell. */
    std::uint64_t total_live_bytes() const noexcept { return total_live_bytes_; }
    std::uint64_t total_live_chunks() const noexcept { return total_live_; }

    /* How many full rebuilds have happened. The guard against the frame budget
     * quietly becoming O(table): this must not climb once the geometry has
     * settled. */
    std::uint64_t rebuilds() const noexcept { return rebuilds_; }

private:
    CellAggregate *cell_for(std::uint64_t ptr) noexcept;

    Grid                       grid_;
    std::vector<CellAggregate> cells_;
    HeatTimings                timings_ = kDefaultTimings;

    std::uint64_t total_live_bytes_ = 0;
    std::uint64_t total_live_       = 0;
    std::uint64_t rebuilds_         = 0;
};

} // namespace hv

#endif /* HEAPVIZ_TUI_HEATMAP_H */
