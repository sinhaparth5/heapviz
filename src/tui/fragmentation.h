/* heapviz - external fragmentation over the live set (M5.4).
 *
 * Copyright (C) 2026 Parth Sinha
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * How much of the memory the target is holding onto is not actually holding
 * anything: the bytes stranded between live chunks, which the allocator owns,
 * cannot return to the OS, and can only reuse for a request that happens to
 * fit.
 *
 * WHY THE PERCENTAGE NEEDS NO SORT
 * -------------------------------
 * The obvious implementation walks the live set in address order and sums the
 * distance from each chunk's end to the next one's start. That walk needs the
 * chunks sorted, which is the expensive part -- and it is not needed, because
 * the same figure falls out of an identity:
 *
 *     sum of gaps  ==  (last end - first start)  -  sum of footprints
 *
 * Both terms on the right are a min, a max and a sum, all of which one linear
 * pass over the chunk table computes without ordering anything. So the headline
 * number is exact, costs O(n) with no allocation and no sort, and is available
 * on every heap regardless of size. The sorted walk survives only for the
 * largest hole below, which genuinely cannot be had any other way.
 *
 * WHY EACH REGION IS MEASURED SEPARATELY
 * -------------------------------------
 * `RegionMap`'s comment has the numbers: a threaded target's brk heap and its
 * thread arenas span about 23 TiB between them, of which perhaps 40 MiB is
 * memory. One span across all of them would report 99.99% fragmented forever,
 * on every target with more than one arena, which is most of them. So the span
 * and the gaps are accumulated per region and summed at the end, and a gap
 * running from the top of one arena to the bottom of the next is not a gap --
 * it is the distance between two separate heaps.
 *
 * WHY FREE SPACE AT THE ENDS IS NOT COUNTED
 * ----------------------------------------
 * The span runs from the first live chunk to the last, not from the region's
 * first byte to its last. Memory above the topmost allocation is headroom: the
 * heap grows into it, and it is one contiguous block that any request can use.
 * Calling that fragmentation would make a program that just reserved a large
 * arena and has not filled it yet look pathological, which is backwards. What
 * this measures is memory trapped *between* things that are still alive, which
 * is the only kind that a well-behaved program cannot get back.
 *
 * WHY A CHUNK'S FOOTPRINT IS `usable + kChunkMinOverheadBytes`
 * -----------------------------------------------------------
 * Because ptmalloc packs its chunks with no space between them, and the one
 * word an in-use chunk costs (see `chunk_reader.h`) sits below the pointer the
 * interceptor reported. Modelling a chunk as `[ptr, ptr + usable)` therefore
 * leaves an eight-byte hole in front of every single one of them, and a heap of
 * perfectly packed 32-byte allocations reads as 25% fragmented -- straight into
 * the `[Med]` badge with nothing wrong. Adding the word back closes the seam,
 * so a densely packed heap measures zero, which is what it is.
 *
 * Note this needs no help from M2.2's enrichment pass: `size + overhead_exact`
 * is the same figure by a longer road, so fragmentation does not drift upward
 * as the enrichment cursor sweeps the table.
 *
 * WHY IT RUNS ON A TIMER
 * ---------------------
 * The pass is linear in the live set, and the live set can be a million chunks.
 * At 4 Hz a pass that costs a millisecond is a fifteenth of one frame in
 * fifteen, which the paced loop absorbs without missing a deadline; per frame it
 * would be the frame budget. Fragmentation is also not a per-frame quantity --
 * it moves over seconds, and a figure that flickered between two values sixty
 * times a second would be unreadable even if it were free.
 */

#ifndef HEAPVIZ_TUI_FRAGMENTATION_H
#define HEAPVIZ_TUI_FRAGMENTATION_H

#include "tui/chunk_table.h"
#include "tui/region_map.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace hv {

/* How often the analysis runs. The same 250 ms M2.2's enrichment and M5.2's
 * inspector refresh use, and for the same reason: it is the slowest tick a
 * person still reads as live. */
constexpr std::uint32_t kFragIntervalMs = 250;

/* Above this many records the largest-hole figure is not computed.
 *
 * It is the one part of the analysis that needs the chunks in address order,
 * and sorting a quarter of a million of them is a few milliseconds -- affordable
 * once a second, not affordable at ten times that size. The percentage is
 * unaffected and keeps being reported, so what a very large heap loses is the
 * secondary figure rather than the headline one. `largest_gap_known` says which
 * case the panel is in, so it can leave the field off rather than print a zero
 * that would read as "no holes". */
constexpr std::size_t kFragMaxSorted = std::size_t{1} << 18;

/* What one pass concluded. `percent` below zero means no pass has produced a
 * figure -- an empty table, no regions yet, or a heap whose live chunks all sit
 * outside the regions the last scan knew about. That is a different statement
 * from zero percent, and the panel draws it differently. */
struct FragReport {
    std::uint64_t span_bytes  = 0; /* summed per region: last end - first start */
    std::uint64_t used_bytes  = 0; /* summed chunk footprints                   */
    std::uint64_t gap_bytes   = 0; /* span - used, floored at zero              */
    std::uint64_t largest_gap = 0; /* biggest single hole, within one region    */
    std::uint64_t chunks      = 0; /* live chunks the pass measured             */
    std::uint64_t outside     = 0; /* live chunks in no known region, skipped   */
    std::size_t   regions     = 0; /* regions holding at least one live chunk   */
    bool          largest_gap_known = false;
    int           percent     = -1;
};

class FragAnalyzer {
public:
    /* Tests drive the clock rather than waiting on it, and a future `[r]` that
     * wanted an immediate recount would set this to zero. */
    void set_interval(std::uint32_t ms) noexcept { interval_ms_ = ms; }
    void set_max_sorted(std::size_t n) noexcept { max_sorted_ = n; }

    bool due(std::uint32_t now_ms) const noexcept;

    /* Runs a pass if one is due, and returns true when the figures the panel
     * shows came out different from last time. False on a frame where nothing
     * ran, which is fifty-nine frames in sixty -- so this costs a comparison to
     * call from `update` and nothing else. */
    bool analyze(const ChunkTable &table, const RegionMap &regions,
                 std::uint32_t now_ms);

    const FragReport &report() const noexcept { return report_; }
    int percent() const noexcept { return report_.percent; }
    std::uint64_t largest_gap() const noexcept { return report_.largest_gap; }
    bool largest_gap_known() const noexcept { return report_.largest_gap_known; }

private:
    /* One region's running totals. `lo`/`hi` are only meaningful once `chunks`
     * is non-zero, which is what stops an empty arena contributing a span. */
    struct RegionAcc {
        std::uint64_t lo     = 0;
        std::uint64_t hi     = 0;
        std::uint64_t used   = 0;
        std::uint64_t chunks = 0;
    };

    /* A live chunk reduced to the only two things the ordered walk needs. Kept
     * to sixteen bytes so a quarter-million of them is 4 MiB rather than 6. */
    struct Extent {
        std::uint64_t start;
        std::uint64_t end;
    };

    FragReport    report_{};
    std::uint32_t interval_ms_ = kFragIntervalMs;
    std::uint32_t last_ms_     = 0;
    bool          ran_         = false;
    std::size_t   max_sorted_  = kFragMaxSorted;

    /* Both reused across passes, so a steady-state session allocates here once
     * and then never again. */
    std::vector<RegionAcc> acc_;
    std::vector<Extent>    extents_;
};

} // namespace hv

#endif /* HEAPVIZ_TUI_FRAGMENTATION_H */
