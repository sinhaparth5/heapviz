/* heapviz - snapshot marks and leak candidates (M5.5).
 *
 * Copyright (C) 2026 Parth Sinha
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "tui/snapshot.h"

#include <cstdio>
#include <ctime>

namespace hv {

void SnapshotDiff::take(std::uint32_t now_ms) noexcept {
    taken_ms_ = now_ms;
    taken_    = true;

    /* Two clocks, deliberately. The comparison against `Chunk::alloc_ms` needs
     * the session's own milliseconds, and the banner needs something the user
     * can match against what they were doing. Neither is derivable from the
     * other -- the session clock has no epoch anyone knows -- so both are kept.
     *
     * `localtime_r` rather than `localtime`: the TUI is one thread today, and a
     * function that returns a pointer into shared static storage is the kind of
     * thing that is fine until it is not. */
    const std::time_t t = std::time(nullptr);
    std::tm tmv{};
    if (localtime_r(&t, &tmv) != nullptr) {
        std::snprintf(taken_at_, sizeof taken_at_, "%02d:%02d:%02d", tmv.tm_hour,
                      tmv.tm_min, tmv.tm_sec);
    } else {
        /* A clock that will not answer is not a reason to refuse the snapshot:
         * the diff works off the session clock, and only the label is lost. */
        taken_at_[0] = '\0';
    }

    /* The mark moved, so every figure computed against the old one is stale.
     * Cleared rather than left to the next tick, because `s` is usually pressed
     * *because* the user is about to do something they want measured, and a
     * quarter second of the previous mark's numbers under the new mark's
     * timestamp is a reading nobody can interpret. */
    report_ = LeakReport{};
    reset_cells(cells_.size());
    ran_ = false;
}

void SnapshotDiff::clear() noexcept {
    taken_       = false;
    diff_        = false;
    taken_ms_    = 0;
    taken_at_[0] = '\0';
    report_      = LeakReport{};
    reset_cells(cells_.size());
    ran_ = false;
}

bool SnapshotDiff::toggle() noexcept {
    if (!taken_) return false;
    diff_ = !diff_;
    if (diff_) {
        /* Force the next `analyze` rather than computing here: this is a
         * keypress handler, and the pass wants the chunk table and the map,
         * which the key path has no business reaching into. */
        ran_ = false;
    } else {
        /* Leaving the mode zeroes the overlay so the map stops highlighting,
         * and does it here rather than in the draw so that the draw can stay a
         * function of the cell counts alone. */
        report_ = LeakReport{};
        reset_cells(cells_.size());
    }
    return diff_;
}

void SnapshotDiff::reset_cells(std::size_t n) {
    /* Both containers keep their capacity across passes, so a steady-state
     * session allocates here once and then never again. */
    if (cells_.size() != n) {
        cells_.assign(n, 0);
    } else {
        for (std::uint32_t &c : cells_) c = 0;
    }
    hot_.clear();
    cells_hash_ = 0;
}

bool SnapshotDiff::analyze(const ChunkTable &table, const HeatMap &map,
                           std::uint32_t now_ms, bool force) {
    /* The whole cost of the feature when it is off. A user who never presses
     * `d` pays one branch per frame for this file. */
    if (!diff_ || !taken_) return false;

    const bool due = !ran_ || force ||
                     static_cast<std::uint32_t>(now_ms - last_ms_) >= interval_ms_;
    if (!due) return false;

    last_ms_ = now_ms;
    ran_     = true;

    /* Both read before `reset_cells`, which zeroes the digest along with the
     * cells it summarises. */
    const LeakReport   prev      = report_;
    const std::uint64_t prev_hash = cells_hash_;

    report_ = LeakReport{};
    reset_cells(map.cell_count());

    const Grid &g = map.grid();
    const Chunk *slots = table.slots();
    const std::size_t n = table.slot_count();

    for (std::size_t i = 0; i < n; ++i) {
        const Chunk &c = slots[i];
        if (c.state != kChunkLive || c.key == 0) continue;

        /* The entire membership test. Unsigned comparison against the mark
         * rather than a subtraction, because the session clock only moves
         * forward and a chunk allocated in the same millisecond as the mark
         * belongs to the "since" side: `s` is pressed to measure what happens
         * next, and the allocation that raced the keypress is what happened
         * next. */
        if (c.alloc_ms < taken_ms_) continue;

        ++report_.chunks;
        report_.bytes += c.usable;

        /* The map's own conversion, never a raw shift. Under a RegionMap the
         * grid coordinate is a packed offset, and an address bucketed as though
         * it were one lands in an unrelated cell -- which on this overlay would
         * be a magenta highlight over memory that is not a leak candidate. */
        std::uint64_t coord = 0;
        std::size_t idx = 0;
        if (!map.coordinate_of(c.key, coord) || !g.index_of(coord, idx) ||
            idx >= cells_.size()) {
            /* Counted in the total and missing from the overlay, which is why
             * the total says so. A chunk in a region the 2 Hz scan has not seen
             * yet is the ordinary case here, exactly as it is for
             * `FragReport::outside`. */
            ++report_.offmap;
            continue;
        }
        /* The first candidate in a cell is what puts it on the draw's list.
         * Later ones only deepen a highlight that is already there. */
        if (cells_[idx]++ == 0) hot_.push_back(static_cast<std::uint32_t>(idx));
    }

    /* FNV-1a over the overlay. The caller uses the return value to decide
     * whether the frame is worth drawing, so "changed" has to mean the screen
     * changed -- which the summary alone cannot say, hence the digest. Linear in
     * the cell count, which is ten thousand at 4 Hz. */
    std::uint64_t h = 1469598103934665603ull;
    for (const std::uint32_t c : cells_) {
        h = (h ^ c) * 1099511628211ull;
    }
    cells_hash_ = h;

    return h != prev_hash || report_.chunks != prev.chunks ||
           report_.bytes != prev.bytes || report_.offmap != prev.offmap;
}

} // namespace hv
