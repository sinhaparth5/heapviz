/* heapviz - snapshot marks and leak candidates (M5.5).
 *
 * Copyright (C) 2026 Parth Sinha
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Mark an instant, keep working, and ask what has accumulated since. Everything
 * still live that was allocated after the mark is a leak candidate -- not a
 * leak, because a program is entitled to allocate and hold, but the set a person
 * hunting one has to look through.
 *
 * WHY THE SNAPSHOT IS A TIMESTAMP AND NOT A SET OF POINTERS
 * --------------------------------------------------------
 * The obvious implementation records every live pointer at the mark and then
 * asks, per chunk, whether it was in that set. `ChunkTable` already answers the
 * same question for free: `Chunk::alloc_ms` is when the record was created, so
 * `alloc_ms >= taken_ms` *is* "allocated after the mark", exactly, for one
 * comparison and no memory.
 *
 * It is also the answer that survives address recycling, which is the case a
 * pointer set gets wrong. An address that was live at the mark, was freed, and
 * was handed back out afterwards is a leak candidate -- the allocation being
 * held now is not the one that was held then. It is also in the recorded set, so
 * a set-membership test excludes it, and the excluded ones are precisely the
 * addresses the allocator reuses most. `insert_live` overwrites the record and
 * its `alloc_ms` with it, so the timestamp test gets that case right without
 * knowing it exists.
 *
 * The consequence worth stating plainly: a snapshot costs a `uint32_t`. There is
 * no memory bound to enforce, nothing to refuse, and a target with fifty million
 * live chunks marks as cheaply as one with three.
 *
 * WHY THE PASS IS ON A TIMER AND THE OVERLAY IS PER-CELL
 * -----------------------------------------------------
 * Deciding which chunks qualify is one comparison, but finding them is a linear
 * walk of the chunk table, which is `FragAnalyzer`'s cost and gets
 * `FragAnalyzer`'s treatment: 4 Hz, not per frame. Leak candidates accumulate
 * over the seconds a person spends watching for them, not between two frames.
 *
 * What the walk produces is a count per *cell*, not a list of pointers, so the
 * overlay is bounded by the grid -- ten thousand cells on a large terminal --
 * rather than by the live set. That is what lets the map be repainted from it
 * every frame while the analysis runs at a fifteenth of the rate.
 *
 * WHEN NO PASS HAS RUN
 * --------------------
 * Off is the default and costs nothing: with diff mode disabled `analyze`
 * returns on a bool and never touches the table. Turning it on forces a pass
 * immediately rather than waiting for the tick, for the reason M5.2's inspector
 * refreshes on a keypress -- the display is the answer to the key that was just
 * pressed, and a quarter-second of blank reads as the tool being slow.
 */

#ifndef HEAPVIZ_TUI_SNAPSHOT_H
#define HEAPVIZ_TUI_SNAPSHOT_H

#include "tui/chunk_table.h"
#include "tui/heatmap.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace hv {

/* The same 250 ms the enrichment pass, the inspector and the fragmentation
 * analysis use. Four things now share this cadence and none of them should have
 * its own idea of "the slowest tick that still reads as live". */
constexpr std::uint32_t kLeakIntervalMs = 250;

/* Room for `HH:MM:SS` and its terminator, which is all the mark needs to be
 * identifiable: a session runs for minutes, and a date on the banner would cost
 * columns to say what the user already knows. */
constexpr std::size_t kMarkTimeMax = 16;

/* What one pass found. `chunks` and `bytes` are the whole live set's answer;
 * `offmap` is the part of it the overlay cannot show, because those chunks sit
 * in a region the last scan did not know about or outside the grid entirely.
 * Reported rather than folded in, so a summary that says 900 and a map that
 * highlights 850 is explained by a number rather than by a discrepancy. */
struct LeakReport {
    std::uint64_t chunks = 0;
    std::uint64_t bytes  = 0; /* usable bytes, matching the live-set figures */
    std::uint64_t offmap = 0;
};

class SnapshotDiff {
public:
    /* Marks now. Taking a second snapshot replaces the first -- there is one
     * mark, because "leaked since" is only a sentence when there is one thing
     * it can be since. */
    void take(std::uint32_t now_ms) noexcept;

    /* Drops the mark and leaves diff mode. Leaving the mode on with nothing to
     * diff against would show a map with no highlights, which is the picture of
     * a program with no leak candidates rather than of a cleared snapshot. */
    void clear() noexcept;

    /* Turns the overlay on or off. A no-op without a mark, and the caller is
     * expected to have said so: the footer advertises `s snap` until a snapshot
     * exists and `d diff` only once one does, so the key that does nothing is
     * also the key that is not offered. Returns the resulting mode. */
    bool toggle() noexcept;

    bool has_snapshot() const noexcept { return taken_; }
    bool diff_mode()    const noexcept { return diff_; }
    std::uint32_t taken_ms() const noexcept { return taken_ms_; }

    /* `HH:MM:SS` of the wall clock when the mark was taken, for the banner.
     * Wall clock rather than the session's own milliseconds because the number
     * is there to be matched against something the user did -- a request they
     * sent, a button they clicked -- and "42318 ms into the session" is not a
     * time anybody was looking at. Empty string when there is no mark. */
    const char *taken_at() const noexcept { return taken_at_; }

    void set_interval(std::uint32_t ms) noexcept { interval_ms_ = ms; }

    /* Runs a pass if one is due, or if `force` says the model moved under the
     * last one. False on a frame where nothing ran or nothing changed, which is
     * fourteen frames in fifteen.
     *
     * `force` is what a repack or a rebuild sets: both renumber the cells, so an
     * overlay computed against the old numbering highlights a different part of
     * the heap -- and would keep doing so, plausibly, until the next tick. */
    bool analyze(const ChunkTable &table, const HeatMap &map,
                 std::uint32_t now_ms, bool force);

    const LeakReport &report() const noexcept { return report_; }

    /* How many leak candidates fall in cell `i`. Zero for every cell when diff
     * mode is off, so the overlay draw needs no separate mode check. */
    std::uint32_t cell_count(std::size_t i) const noexcept {
        return i < cells_.size() ? cells_[i] : 0;
    }
    std::size_t cell_span() const noexcept { return cells_.size(); }

    /* The indices of the cells holding at least one candidate, in no
     * particular order.
     *
     * The overlay is drawn every frame and computed four times a second, so the
     * draw is the side that has to be cheap. Iterating this makes it cost one
     * write per highlighted cell rather than one branch per cell on the map --
     * ten thousand of them on a large terminal, in a session where diff mode is
     * usually off and the answer is usually no. */
    const std::vector<std::uint32_t> &hot_cells() const noexcept { return hot_; }

private:
    void reset_cells(std::size_t n);

    LeakReport    report_{};
    std::vector<std::uint32_t> cells_;
    std::vector<std::uint32_t> hot_;

    /* A digest of the overlay, so a pass can tell "the same candidates in the
     * same cells" from "the same number of them somewhere else". The summary
     * alone cannot: one chunk freed and another of the same size allocated
     * elsewhere within a tick leaves both totals identical and the map wrong,
     * and a frame that reports no change is a frame the loop does not draw. */
    std::uint64_t cells_hash_ = 0;

    std::uint32_t taken_ms_   = 0;
    std::uint32_t last_ms_    = 0;
    std::uint32_t interval_ms_ = kLeakIntervalMs;
    bool          taken_      = false;
    bool          diff_       = false;
    bool          ran_        = false;
    char          taken_at_[kMarkTimeMax] = {0};
};

} // namespace hv

#endif /* HEAPVIZ_TUI_SNAPSHOT_H */
