/* heapviz - the chunk inspector panel (M5.2).
 *
 * Copyright (C) 2026 Parth Sinha
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * M5.1's cursor says which cell the user is pointing at. This says what is in
 * it: the address, the size the program asked for, the size the allocator
 * actually spent, the chunk's status and how long it has been alive.
 *
 * THE LOOKUP RUNS THE WRONG WAY
 * -----------------------------
 * `ChunkTable` is keyed by pointer, because every drained event is a lookup by
 * pointer and that is the hot path of the whole consumer. Nothing indexes it by
 * cell, so "what is in this cell" is a scan of the table -- 65,536 slots at a
 * default reservation, which is not a per-frame cost.
 *
 * Three things keep it off the frame budget, in the order they fire:
 *
 *   1. **The map already knows when the answer is nothing.** A cell whose
 *      aggregate holds no live bytes, no overhead and no fading free cannot
 *      contain a chunk, and that is one struct read. On a real heap most cells
 *      are dark, so most cursor positions never reach the scan at all.
 *   2. **It only runs when the answer could have changed** -- the cursor moved,
 *      the geometry was rebuilt, or a refresh interval elapsed. Between those,
 *      the selection is a cached vector and drawing it is free.
 *   3. **The refresh is 4 Hz, not 60.** A chunk that appeared in the cell 200 ms
 *      ago is not a stale display, it is a display that is 200 ms old; the panel
 *      says so implicitly by being a snapshot of a cell rather than a live feed
 *      of one. M5.4's fragmentation tick makes the same trade for the same
 *      reason, and ROADMAP M5.4 states it outright.
 *
 * SEVERAL CHUNKS TO A CELL IS THE NORMAL CASE
 * -------------------------------------------
 * A 16 KiB cell on a real heap holds hundreds of allocations, so "the chunk in
 * this cell" is usually a lie. The panel shows the largest -- the one most
 * likely to be what a person is looking for -- says how many others there are,
 * and cycles with `Tab`. The candidate list is bounded, and it is a bounded
 * *top-K by size* rather than the first K found: taking the first K would make
 * the headline chunk depend on hash-table probe order, so the same cell would
 * name a different chunk after an unrelated rehash.
 */

#ifndef HEAPVIZ_TUI_INSPECTOR_H
#define HEAPVIZ_TUI_INSPECTOR_H

#include "tui/chunk_table.h"
#include "tui/cursor.h"
#include "tui/framebuffer.h"
#include "tui/heatmap.h"
#include "tui/region_map.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace hv {

/* How many chunks in one cell `Tab` will cycle through. Past this the panel
 * still reports the true total, so the number on screen is never a lie about
 * how crowded the cell is -- only about how many of them can be reached. */
constexpr std::size_t kInspectorCandidates = 64;

/* How often the selection is re-derived when nothing else has invalidated it.
 * See the header comment: 4 Hz, deliberately, because the alternative is a
 * table scan inside a 1 ms frame. */
constexpr std::uint32_t kInspectRefreshMs = 250;

/* Rows the panel occupies, including its rule. Fixed rather than sized to its
 * contents for the reason `HeapApp` fixes its header: a map whose bottom edge
 * moved when a chunk came into view would re-bucket the address space at the
 * moment the user was reading it. M6.2 replaces this with a solved layout. */
constexpr int kInspectorRows = 6;

/* Below this the panel costs more rows than the map can spare, and a map
 * squeezed to four rows is not a heap map. It is dropped whole. */
constexpr int kInspectorMinMapRows = 6;

enum class ChunkStatus : std::uint8_t {
    Unallocated, /* nothing in this cell                          */
    Active,      /* live, ordinary ptmalloc chunk                 */
    Mmapped,     /* live, and its header says IS_MMAPPED          */
    Freed,       /* freed, still held for the fade                */
};

const char *chunk_status_str(ChunkStatus s) noexcept;

/* One row of the panel's model. A copy rather than a pointer into the table:
 * the table rehashes and backward-shifts under drained events, so a pointer
 * taken this frame is not a pointer next frame. */
struct ChunkDetail {
    std::uint64_t addr     = 0;
    std::uint32_t size     = 0; /* what the caller asked for       */
    std::uint32_t usable   = 0; /* what the allocator made usable  */
    std::uint32_t overhead = 0; /* what the chunk cost beyond `size` */
    std::uint32_t alloc_ms = 0;
    std::uint32_t free_ms  = 0;
    std::uint32_t tid      = 0;
    ChunkStatus   status   = ChunkStatus::Unallocated;

    /* True when `overhead` came out of the target's own header rather than from
     * `usable - size`. The panel marks the difference, because an inferred
     * figure is always an underestimate and presenting it as measured is
     * exactly the quiet lie this tool exists not to tell. */
    bool exact = false;

    /* Real size: what the allocator spent in total. */
    std::uint64_t chunk_bytes() const noexcept {
        return static_cast<std::uint64_t>(size) + overhead;
    }
};

/* The chrome colours the panel needs. Same shape and the same defaults as
 * `MapStyle`, so M6.1 can hand both the theme's tokens. */
struct InspectorStyle {
    Rgb ink    = 0x00D8D8D8;
    Rgb dim    = 0x007A7A7A;
    Rgb accent = 0x00F5A623;
    Rgb frame  = 0x00C87828;
    Rgb live   = 0x003584E4;
    Rgb freed  = 0x00E01B24;
    Rgb bg     = 0x000C0C0C;
};

class ChunkInspector {
public:
    void set_style(const InspectorStyle &s) noexcept { style_ = s; }
    const InspectorStyle &style() const noexcept { return style_; }

    /* Re-derives the selection for the cell the cursor is on, if anything could
     * have changed it. Returns true when what is on screen would differ.
     *
     * `dirty` is the caller's "the model moved under me" signal -- a rebuild, a
     * repack, a resize. It forces the scan regardless of the interval, because
     * after a regrid the cached selection describes a cell that no longer
     * exists at that index. */
    bool refresh(const ChunkTable &table, const HeatMap &map,
                 const MapCursor &cur, std::uint32_t now_ms, bool dirty);

    /* `Tab`: the next chunk in the cell, wrapping. False when there is nothing
     * to cycle, so the caller does not repaint for a keypress that did nothing. */
    bool cycle() noexcept;

    /* How many chunks are in the cell in total, which is not the same as how
     * many are reachable -- see kInspectorCandidates. */
    std::size_t total() const noexcept { return total_; }
    std::size_t reachable() const noexcept { return found_.size(); }
    std::size_t position() const noexcept { return pos_; }

    /* The chunk being shown. Its status is `Unallocated` when the cell is
     * empty, which is a state the panel draws rather than a failure. */
    const ChunkDetail &current() const noexcept;

    /* How many table scans have happened. Exposed for the same reason
     * `HeatMap::rebuilds()` is: the whole design of this class rests on the
     * scan being rare, and a test is the only thing that will notice when a
     * change quietly turns it into a per-frame cost. */
    std::uint64_t scans() const noexcept { return scans_; }

    /* Draws the panel into `area`. `regions` converts the packed coordinate
     * back to a real address for the header line, and may be null. */
    void draw(Framebuffer &fb, Rect area, const Grid &g, const MapCursor &cur,
              const RegionMap *regions, std::uint32_t now_ms) const noexcept;

private:
    void rescan(const ChunkTable &table, const HeatMap &map, std::size_t cell);

    InspectorStyle           style_{};
    std::vector<ChunkDetail> found_;
    std::size_t              total_    = 0;
    std::size_t              pos_      = 0;
    std::size_t              cell_     = SIZE_MAX; /* which cell `found_` is for */
    std::uint32_t            last_ms_  = 0;
    std::uint64_t            selected_ = 0; /* address, so Tab survives a rescan */
    std::uint64_t            scans_    = 0;
};

/* "1,024" -- a decimal count with thousands separators, which is how the
 * mockup's size fields read. Returns the length written. Exposed because
 * M5.3's metrics panel needs the identical formatting and two implementations
 * of a separator rule end up disagreeing about where the commas go. */
std::size_t format_count(char *buf, std::size_t n, std::uint64_t v) noexcept;

} // namespace hv

#endif /* HEAPVIZ_TUI_INSPECTOR_H */
