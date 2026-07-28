/* heapviz - per-cell aggregation (M3.3).
 *
 * Copyright (C) 2026 Parth Sinha
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "tui/heatmap.h"

namespace hv {

namespace {

const CellAggregate kEmptyAggregate{};

/* kNoTime is 0xFFFFFFFF, which is larger than every real timestamp, so a plain
 * max() would keep the sentinel forever. */
void bump(std::uint32_t &slot, std::uint32_t value) noexcept {
    if (slot == kNoTime || value > slot) slot = value;
}

std::uint32_t overhead_of(std::uint32_t size, std::uint32_t usable) noexcept {
    return usable > size ? usable - size : 0u;
}

/* A stamp from the future needs no guard of its own: the subtraction is
 * unsigned, so `now - stamp` wraps to something near 2^32, which fails the
 * window comparison for any window a person would configure. An explicit
 * `now < stamp` check was written here first and removed after no mutation of
 * it could be made to fail -- it was a second spelling of what the arithmetic
 * already does. `heatmap_test` pins the behaviour so this stays true. */
bool within(std::uint32_t stamp, std::uint32_t now, std::uint32_t window) noexcept {
    if (stamp == kNoTime) return false;
    return (now - stamp) < window;
}

} // namespace

const char *cell_state_str(CellState s) noexcept {
    switch (s) {
    case CellState::Empty:        return "empty";
    case CellState::Overhead:     return "overhead";
    case CellState::Live:         return "live";
    case CellState::RecentMalloc: return "recent malloc";
    case CellState::RecentFree:   return "recent free";
    }
    return "unknown";
}

bool HeatMap::configure(const Grid &g) {
    /* Only the things that change where an address lands count as a geometry
     * change. Being handed an equal grid every frame is the normal case -- the
     * loop re-measures on every SIGWINCH, most of which do not alter the
     * granularity -- and it has to be free. */
    const bool same = grid_.valid() == g.valid() &&
                      grid_.base() == g.base() &&
                      grid_.cell_bytes() == g.cell_bytes() &&
                      grid_.cols() == g.cols() &&
                      grid_.rows() == g.rows();
    if (same && cells_.size() == g.cell_count()) return false;

    grid_ = g;
    cells_.assign(g.cell_count(), kEmptyAggregate);
    total_live_bytes_ = 0;
    total_live_       = 0;
    return true;
}

void HeatMap::rebuild(const ChunkTable &table) {
    ++rebuilds_;

    for (CellAggregate &c : cells_) c = kEmptyAggregate;
    total_live_bytes_ = 0;
    total_live_       = 0;

    if (!grid_.valid()) return;

    const Chunk *slots = table.slots();
    const std::size_t n = table.slot_count();

    for (std::size_t i = 0; i < n; ++i) {
        const Chunk &ch = slots[i];
        if (ch.state == kChunkEmpty) continue;

        std::size_t index = 0;
        if (!grid_.index_of(ch.key, index)) continue;
        CellAggregate &cell = cells_[index];

        /* Every record, live or freed, was allocated at some point, and the
         * incremental path recorded that at the time. A rebuild that only
         * looked at live records would lose the alloc stamp of anything since
         * freed and disagree with it. */
        bump(cell.last_alloc_ms, ch.alloc_ms);

        if (ch.state == kChunkLive) {
            ++cell.n_live;
            cell.live_bytes += ch.usable;
            cell.overhead_bytes += overhead_of(ch.size, ch.usable);
            ++total_live_;
            total_live_bytes_ += ch.usable;
        } else {
            bump(cell.last_free_ms, ch.free_ms);
        }
    }
}

CellAggregate *HeatMap::cell_for(std::uint64_t ptr) noexcept {
    std::size_t index = 0;
    if (!grid_.index_of(ptr, index) || index >= cells_.size()) return nullptr;
    return &cells_[index];
}

void HeatMap::on_alloc(std::uint64_t ptr, std::uint32_t size,
                       std::uint32_t usable, std::uint32_t now_ms) noexcept {
    CellAggregate *cell = cell_for(ptr);
    if (cell == nullptr) return;

    ++cell->n_live;
    cell->live_bytes += usable;
    cell->overhead_bytes += overhead_of(size, usable);
    bump(cell->last_alloc_ms, now_ms);

    ++total_live_;
    total_live_bytes_ += usable;
}

void HeatMap::on_free(std::uint64_t ptr, std::uint32_t size,
                      std::uint32_t usable, std::uint32_t now_ms) noexcept {
    CellAggregate *cell = cell_for(ptr);
    if (cell == nullptr) return;

    /* Guarded rather than assumed. heapviz attaches to a process that already
     * has a heap, so frees of chunks it never saw allocated are routine, and
     * an unguarded decrement would wrap the counters into enormous values that
     * would then be drawn. */
    if (cell->n_live > 0) {
        --cell->n_live;
        cell->live_bytes -= (cell->live_bytes >= usable) ? usable : cell->live_bytes;

        const std::uint32_t over = overhead_of(size, usable);
        cell->overhead_bytes -= (cell->overhead_bytes >= over)
                                    ? over : cell->overhead_bytes;

        if (total_live_ > 0) --total_live_;
        total_live_bytes_ -= (total_live_bytes_ >= usable) ? usable
                                                          : total_live_bytes_;
    }

    /* The flash is recorded whether or not the chunk was known, because the
     * free happened in this cell either way and that is what the user is being
     * shown. */
    bump(cell->last_free_ms, now_ms);
}

CellState cell_state(const CellAggregate &a, const HeatTimings &t,
                     std::uint32_t now_ms) noexcept {
    /* ROADMAP M3.3 fixes this order. Note that a free always outranks a malloc
     * inside the flash windows, even when the malloc is the more recent of the
     * two: memory going away is the event people are watching for, and an
     * allocation landing is constant background. */
    if (within(a.last_free_ms, now_ms, t.free_flash_ms)) {
        return CellState::RecentFree;
    }
    if (within(a.last_alloc_ms, now_ms, t.malloc_pulse_ms)) {
        return CellState::RecentMalloc;
    }
    if (a.n_live > 0) return CellState::Live;

    /* Only reachable where a cell holds chunk-header bytes and no payload,
     * which happens at fine granularity when a chunk's header and its data land
     * in different cells. Where a cell has both, the payload wins by the
     * precedence above. */
    if (a.overhead_bytes > 0) return CellState::Overhead;

    return CellState::Empty;
}

CellState HeatMap::state_at(std::size_t index,
                            std::uint32_t now_ms) const noexcept {
    if (index >= cells_.size()) return CellState::Empty;
    return cell_state(cells_[index], timings_, now_ms);
}

const CellAggregate &HeatMap::at(std::size_t index) const noexcept {
    if (index >= cells_.size()) return kEmptyAggregate;
    return cells_[index];
}

} // namespace hv
