/* heapviz - external fragmentation over the live set (M5.4).
 *
 * Copyright (C) 2026 Parth Sinha
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "tui/fragmentation.h"

#include "tui/chunk_reader.h"

#include <algorithm>

namespace hv {

bool FragAnalyzer::due(std::uint32_t now_ms) const noexcept {
    /* The first call always runs. Waiting a quarter second before the panel says
     * anything would be indistinguishable, from the user's side, from the
     * analysis being broken. */
    if (!ran_) return true;
    return static_cast<std::uint32_t>(now_ms - last_ms_) >= interval_ms_;
}

bool FragAnalyzer::analyze(const ChunkTable &table, const RegionMap &regions,
                           std::uint32_t now_ms) {
    if (!due(now_ms)) return false;
    last_ms_ = now_ms;
    ran_     = true;

    const FragReport prev = report_;
    report_ = FragReport{};

    const std::vector<Span> &spans = regions.spans();
    const std::size_t nr = spans.size();

    /* Assigned rather than resized-and-cleared so a region appearing or
     * vanishing cannot leave one arena's totals attached to another's index.
     * Reallocates only when the region count grows, which happens a handful of
     * times in a session and never in its steady state. */
    acc_.assign(nr, RegionAcc{});

    /* Decided before the walk from a bound the table already knows, rather than
     * discovered partway through it: a pass that started collecting and gave up
     * halfway would have a list covering an arbitrary prefix of the heap, and
     * the largest hole in a prefix is not the largest hole. `size()` counts
     * freed records too, so this errs toward not collecting, which is the safe
     * direction for a cost bound. */
    const bool collect = nr != 0 && table.size() <= max_sorted_;
    if (collect) extents_.clear();

    const Chunk *slots = table.slots();
    const std::size_t n = table.slot_count();

    for (std::size_t i = 0; i < n; ++i) {
        const Chunk &c = slots[i];
        if (c.state != kChunkLive || c.key == 0) continue;

        const Span *s = regions.span_at_addr(c.key);
        if (s == nullptr) {
            /* Ordinary rather than an error. The region scan runs at 2 Hz and
             * the target allocates continuously, so between one scan and the
             * next there are always chunks in memory nothing has mapped yet.
             * Counting them would need a span that does not exist. */
            ++report_.outside;
            continue;
        }
        const auto ri = static_cast<std::size_t>(s - spans.data());

        /* See the header: the word below the pointer is what makes two adjacent
         * ptmalloc chunks adjacent, and leaving it out puts an eight-byte hole
         * in front of every allocation in the heap. */
        const std::uint64_t start = c.key;
        std::uint64_t end = start + c.usable + kChunkMinOverheadBytes;

        /* A chunk at the top of a heap that grew since the last scan runs past
         * the region's recorded end. Clamped rather than dropped: the chunk is
         * real and its bytes are used, and letting the span run past the mapping
         * would credit the heap with address space the target does not have. */
        if (end > s->end) end = s->end;
        if (end <= start) continue;

        RegionAcc &a = acc_[ri];
        if (a.chunks == 0) {
            a.lo = start;
            a.hi = end;
        } else {
            if (start < a.lo) a.lo = start;
            if (end > a.hi)   a.hi = end;
        }
        a.used += end - start;
        ++a.chunks;
        ++report_.chunks;

        if (collect) extents_.push_back(Extent{start, end});
    }

    for (const RegionAcc &a : acc_) {
        if (a.chunks == 0) continue;
        ++report_.regions;
        report_.span_bytes += a.hi - a.lo;
        report_.used_bytes += a.used;
    }

    /* Floored rather than trusted. The footprints are modelled, not read, so an
     * mmapped chunk (whose usable excludes a word this adds back) or an address
     * the allocator recycled before the free reached us can make two extents
     * overlap. A negative total is arithmetic noise; presenting it as an
     * enormous positive one after the unsigned subtraction would not be. */
    report_.gap_bytes = report_.span_bytes > report_.used_bytes
                            ? report_.span_bytes - report_.used_bytes
                            : 0;

    /* Truncated, not rounded. The badge boundaries are 15 and 40, so no rounding
     * rule can move a heap between bands; the only thing a rule would change is
     * whether a heap with a few hundred stranded bytes reads as 0% or 1%, and 0%
     * is the true one. */
    if (report_.span_bytes != 0)
        report_.percent =
            static_cast<int>(report_.gap_bytes * 100 / report_.span_bytes);

    /* The largest single hole, which is the figure that answers "can I still
     * allocate 1 MB". Unlike the total it cannot be had from sums, so this is
     * the only part of the pass that pays for order. */
    if (collect && !extents_.empty()) {
        std::sort(extents_.begin(), extents_.end(),
                  [](const Extent &a, const Extent &b) {
                      return a.start < b.start;
                  });

        /* Regions are ascending and disjoint and the extents are now sorted, so
         * one cursor walks both. A pair straddling a boundary is skipped: the
         * distance from the top of one arena to the bottom of the next is
         * megabytes of address space that was never part of either heap, and
         * reporting it as a hole would answer "yes, easily" to a request that
         * has nowhere to go. */
        std::size_t ri = 0;
        for (std::size_t i = 1; i < extents_.size(); ++i) {
            const Extent &a = extents_[i - 1];
            const Extent &b = extents_[i];

            while (ri < nr && spans[ri].end <= a.start) ++ri;
            if (ri >= nr) break;
            if (b.start >= spans[ri].end) continue;

            if (b.start > a.end) {
                const std::uint64_t hole = b.start - a.end;
                if (hole > report_.largest_gap) report_.largest_gap = hole;
            }
        }
        report_.largest_gap_known = true;
    }

    return report_.percent != prev.percent ||
           report_.largest_gap != prev.largest_gap ||
           report_.largest_gap_known != prev.largest_gap_known;
}

} // namespace hv
