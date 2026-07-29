/* heapviz - packing scattered regions into one displayable space (M2.4).
 *
 * Copyright (C) 2026 Parth Sinha
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "tui/region_map.h"

namespace hv {

bool RegionMap::rebuild(const std::vector<Region> &regions) {
    /* Compared against the previous layout rather than rebuilt and diffed after
     * the fact, so the common case -- a scan that found exactly what the last
     * one found -- touches nothing and returns false. */
    bool changed = false;
    std::size_t i = 0;
    std::uint64_t flat = 0;

    for (const Region &r : regions) {
        if (!r.allocatable() || r.size() == 0) continue;

        const Span s{r.start, r.end, flat, r.kind, r.thread_arena};

        if (i < spans_.size()) {
            const Span &old = spans_[i];
            if (old.start != s.start || old.end != s.end || old.flat != s.flat)
                changed = true;
            spans_[i] = s;
        } else {
            spans_.push_back(s);
            changed = true;
        }

        flat += s.size();
        ++i;
    }

    /* Regions can go away: a thread exits and its arena is unmapped. Anything
     * left over from the previous layout is stale. */
    if (i < spans_.size()) {
        spans_.resize(i);
        changed = true;
    }

    total_ = flat;
    if (changed) ++repacks_;
    return changed;
}

void RegionMap::clear() noexcept {
    spans_.clear();
    total_ = 0;
}

const Span *RegionMap::span_at_addr(std::uint64_t addr) const noexcept {
    /* Ascending and disjoint, because /proc emits them that way and `rebuild`
     * preserves the order it was given. */
    std::size_t lo = 0, hi = spans_.size();
    while (lo < hi) {
        const std::size_t mid = lo + (hi - lo) / 2;
        const Span &s = spans_[mid];
        if (addr < s.start)     hi = mid;
        else if (addr >= s.end) lo = mid + 1;
        else                    return &s;
    }
    return nullptr;
}

const Span *RegionMap::span_at_flat(std::uint64_t flat) const noexcept {
    /* The packed offsets are contiguous by construction, so this is the same
     * search over a different key rather than a scan. */
    std::size_t lo = 0, hi = spans_.size();
    while (lo < hi) {
        const std::size_t mid = lo + (hi - lo) / 2;
        const Span &s = spans_[mid];
        if (flat < s.flat)                 hi = mid;
        else if (flat >= s.flat + s.size()) lo = mid + 1;
        else                                return &s;
    }
    return nullptr;
}

bool RegionMap::to_flat(std::uint64_t addr, std::uint64_t &out) const noexcept {
    const Span *s = span_at_addr(addr);
    if (s == nullptr) return false;
    out = s->flat + (addr - s->start);
    return true;
}

bool RegionMap::to_addr(std::uint64_t flat, std::uint64_t &out) const noexcept {
    const Span *s = span_at_flat(flat);
    if (s == nullptr) return false;
    out = s->start + (flat - s->flat);
    return true;
}

bool RegionMap::is_boundary(std::uint64_t flat) const noexcept {
    /* Offset zero is the start of the display, not a seam between two regions:
     * marking it would draw a boundary above the first row, which is a line
     * between the map and the legend rather than between two arenas. */
    if (flat == 0) return false;
    for (const Span &s : spans_)
        if (s.flat == flat) return true;
    return false;
}

} // namespace hv
