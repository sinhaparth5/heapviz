/* heapviz - packing scattered regions into one displayable space (M2.4).
 *
 * Copyright (C) 2026 Parth Sinha
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * A target's heap is not one range. glibc gives every thread that allocates its
 * own arena, so a program with four working threads has a brk heap at 0x5b...
 * and four 64 MiB-aligned arenas somewhere in 0x7f..., and the interesting
 * memory is spread across all five.
 *
 * WHY NOT JUST SPAN THEM
 * ----------------------
 * Because it was tried, and measured. The union of a brk heap at 0x5b... and a
 * thread arena at 0x70... is about 23 TiB, of which perhaps 40 MiB is memory;
 * `Grid` clamps to its 1 GiB maximum cell, `covers_whole_span` goes false, and
 * the display becomes one occupied cell in a screenful of nothing. `Grid`'s own
 * comment names this as the case M2 has to fix.
 *
 * WHY NOT JUST PICK ONE
 * ---------------------
 * That is what M2.3 shipped, and it is why this exists. Choosing a region means
 * the display is right about one arena and silent about the rest, and the
 * choice cannot be made well: preferring the named `[heap]` shows the main
 * arena, which for any threaded target is the empty one.
 *
 * WHAT THIS DOES INSTEAD
 * ----------------------
 * Lays the allocatable regions end to end in address order, so the coordinate
 * the grid buckets is "how many heap bytes come before this one" rather than
 * "how far is this from the lowest address". Cell size then follows the memory
 * the target actually has, and every region is on screen at once.
 *
 * The cost is that a flat offset is not an address, and anything showing the
 * user a number has to convert back -- which is what `to_addr` is for. M3.1 is
 * emphatic about this: a gutter that labels a row with the wrong address is not
 * a cosmetic bug, it is a plausible-looking lie, and it is the one failure mode
 * a heap map cannot survive.
 */

#ifndef HEAPVIZ_TUI_REGION_MAP_H
#define HEAPVIZ_TUI_REGION_MAP_H

#include "tui/proc_maps.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace hv {

/* One region's placement. `flat` is where its first byte lands in the packed
 * coordinate space; the region occupies [flat, flat + size). */
struct Span {
    std::uint64_t start = 0; /* real address, inclusive */
    std::uint64_t end   = 0; /* real address, exclusive */
    std::uint64_t flat  = 0;
    RegionKind    kind  = RegionKind::Other;
    bool          thread_arena = false;

    std::uint64_t size() const noexcept { return end - start; }
    bool contains_addr(std::uint64_t a) const noexcept {
        return a >= start && a < end;
    }
    bool contains_flat(std::uint64_t f) const noexcept {
        return f >= flat && f < flat + size();
    }
};

class RegionMap {
public:
    /* Repacks from a scan. Keeps the allocatable regions, in the order they
     * arrive, which /proc guarantees is ascending by address.
     *
     * Returns true when the layout changed, which is the caller's signal that
     * every cached aggregate is now meaningless: a region growing shifts the
     * flat offset of every region above it, so a cell that held one arena's
     * bytes a moment ago now describes a different arena entirely. There is no
     * incremental repair for that, only `HeatMap::rebuild`.
     *
     * False when nothing moved, which is the common case: the scan runs twice a
     * second and most of the time the map is identical to last time. */
    bool rebuild(const std::vector<Region> &regions);

    void clear() noexcept;

    bool          empty()       const noexcept { return spans_.empty(); }
    std::size_t   count()       const noexcept { return spans_.size(); }
    std::uint64_t total_bytes() const noexcept { return total_; }
    const std::vector<Span> &spans() const noexcept { return spans_; }

    /* Real address -> packed offset. False when the address is in no region,
     * which is ordinary: a target allocates outside the regions heapviz knows
     * about between one scan and the next, constantly. */
    bool to_flat(std::uint64_t addr, std::uint64_t &out) const noexcept;

    /* Packed offset -> real address. False past the end. This is what the
     * gutter labels rows with, so it has to be exact rather than approximate:
     * `to_flat` and `to_addr` are inverses everywhere both are defined, and
     * `region_map_test` checks that across whole spans rather than at samples. */
    bool to_addr(std::uint64_t flat, std::uint64_t &out) const noexcept;

    const Span *span_at_addr(std::uint64_t addr) const noexcept;
    const Span *span_at_flat(std::uint64_t flat) const noexcept;

    /* True when `flat` is the first byte of a region, i.e. where the display
     * jumps from one arena to another. The map draws a boundary there, because
     * two adjacent rows whose addresses differ by 40 GiB otherwise look like
     * contiguous memory. */
    bool is_boundary(std::uint64_t flat) const noexcept;

    /* How many times the layout has been repacked. The guard against the frame
     * budget quietly becoming a rebuild loop: a heap that grows steadily
     * repacks on every scan, and each repack costs a full HeatMap rebuild. */
    std::uint64_t repacks() const noexcept { return repacks_; }

private:
    std::vector<Span> spans_;
    std::uint64_t     total_   = 0;
    std::uint64_t     repacks_ = 0;
};

} // namespace hv

#endif /* HEAPVIZ_TUI_REGION_MAP_H */
