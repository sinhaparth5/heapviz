/* heapviz - the attached session as a LoopApp (M2.3).
 *
 * Copyright (C) 2026 Parth Sinha
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Everything above this file already existed and was driven by `DemoHeap`; this
 * is the same pipeline with a real target on the front of it. Events come off
 * the ring, land in the `ChunkTable` and the `HeatMap`, and `MapsScanner` plus
 * `RegionMap` supply the packed coordinate space the grid buckets into. Nothing below is new, which
 * is the point: if this needed changes in `Grid` or `HeatMap`, one of them was
 * holding an assumption about the synthetic heap.
 *
 * THE THREE THINGS THAT ARE ONLY TRUE OF A REAL TARGET
 * ---------------------------------------------------
 * **The heap moves.** `DemoHeap` had fixed bounds decided in its constructor. A
 * real one grows, and its bounds arrive from `/proc/<pid>/maps` on a timer. A
 * change of bounds is a change of granularity, which invalidates every cell, so
 * it goes through `HeatMap::configure` + `rebuild` rather than being folded in.
 *
 * **The past is not observable.** heapviz attaches to a process that already has
 * a heap, so the first thing it sees is a `free` of a chunk it never saw
 * allocated. Every counter that could be decremented by one of those is
 * saturating rather than wrapping -- and the `ChunkTable` lookup on free exists
 * because the event carries `usable` but not the original request, and passing
 * a request of zero would subtract the whole allocation as overhead.
 *
 * **The target dies.** Usually while being watched, sometimes mid-frame. The
 * session then freezes what it has rather than clearing it: the last state of a
 * heap that has just exited is the most interesting frame of the session, and
 * quitting on the user's behalf throws it away.
 */

#ifndef HEAPVIZ_TUI_HEAP_APP_H
#define HEAPVIZ_TUI_HEAP_APP_H

#include "common/heapviz_abi.h"
#include "tui/capabilities.h"
#include "tui/chunk_reader.h"
#include "tui/chunk_table.h"
#include "tui/event_loop.h"
#include "tui/heatmap.h"
#include "tui/map_view.h"
#include "tui/proc_maps.h"
#include "tui/region_map.h"
#include "tui/session.h"

#include <cstdint>
#include <vector>

namespace hv {

/* Ceiling on how many events one frame will absorb.
 *
 * There has to be one: a consumer that drains an entire backlog in a single
 * frame at ~30 ns an event turns a full 1 Mi-slot ring into a 30 ms stall, and
 * a paced loop that misses its deadline by thirty frames is indistinguishable
 * from a hung one. The cost of the cap is that a target producing faster than
 * 60 * this per second outruns the consumer and the ring starts dropping --
 * which is visible, counted, and reported, where a stall is not. */
constexpr std::uint32_t kMaxEventsPerFrame = 32768;

/* How many chunk headers one enrichment pass will read.
 *
 * A full sweep of a large live set is a hundred syscalls, which does not belong
 * in a 1 ms frame. The pass is a round-robin cursor over the chunk table
 * instead: each one corrects a bounded slice and the sweep completes over
 * several seconds. Overhead is a slowly-changing property of chunks that mostly
 * outlive a frame, so arriving late is not the same as being wrong. */
constexpr std::size_t kEnrichPerPass = 512;

/* How often a pass runs. Deliberately slower than the map scan: this reads the
 * target's memory, and the figure it produces does not change once a chunk has
 * been allocated. */
constexpr std::uint32_t kEnrichIntervalMs = 250;

/* How the session is doing, in the order the status line reports them. */
enum class SessionState : std::uint8_t {
    Live,      /* attached, target running                              */
    Exited,    /* the target is gone; what is on screen is its last state */
    Detached,  /* never attached, or released                            */
};

class HeapApp final : public LoopApp {
public:
    HeapApp(RingSession &session, Capabilities caps);

    unsigned drain() override;
    bool     update(std::uint64_t now_ns) override;
    bool     key(char byte) override;
    bool     animating() const override;
    void     resized(int w, int h) override;
    void     draw(Framebuffer &fb, const LoopStats &stats) override;

    /* For the exit summary and for tests, which need to assert on what the
     * session saw without scraping the framebuffer. */
    SessionState state()        const noexcept { return state_; }
    std::uint64_t events_seen() const noexcept { return events_; }
    std::uint64_t frees_unknown() const noexcept { return frees_unknown_; }
    std::uint64_t live_chunks() const noexcept { return live_; }
    std::uint64_t live_bytes()  const noexcept { return live_bytes_; }
    int hidden_regions() const noexcept { return hidden_regions_; }
    AddrRange view_range() const noexcept { return bounds_; }
    const RegionMap &region_map() const noexcept { return regions_; }
    const HeatMap    &map()     const noexcept { return map_; }
    const ChunkTable &table()   const noexcept { return table_; }
    const MapsScanner &maps()   const noexcept { return scanner_; }
    const ChunkReader &reader() const noexcept { return reader_; }
    std::uint64_t refined_chunks() const noexcept { return refined_; }
    std::uint64_t exact_overhead() const noexcept { return exact_overhead_; }

private:
    void apply(const HvEvent *events, std::uint32_t n) noexcept;
    std::uint32_t event_ms(std::uint64_t timestamp_ns) const noexcept;
    /* Repacks the regions and reports whether the layout moved. Replaces
     * M2.3's choose-one-region, which showed a threaded target's main arena --
     * reliably the empty one. */
    bool repack() noexcept;

    /* Reads exact chunk overhead for a bounded slice of the live set and folds
     * it in, replacing the interceptor's `usable - size` approximation. Returns
     * true if anything changed. */
    bool enrich(std::uint32_t now_ms) noexcept;
    void refit(int w, int h);
    Rect map_area(int w, int h) const noexcept;

    RingSession &session_;
    Capabilities caps_;
    MapView      view_;
    ChunkTable   table_;
    HeatMap      map_;
    Grid         grid_;
    MapsScanner  scanner_;
    RegionMap    regions_;
    ChunkReader  reader_;

    std::vector<HvEvent> batch_;

    /* Reused by the enrichment pass, so correcting overhead allocates nothing
     * once the session is running. */
    std::vector<std::uint64_t> enrich_ptrs_;
    std::vector<std::uint64_t> enrich_want_;
    std::vector<std::uint32_t> enrich_approx_;
    std::vector<ChunkInfo>     enrich_out_;
    std::size_t   enrich_cursor_ = 0;
    std::uint32_t enrich_last_ms_ = 0;
    std::uint64_t refined_ = 0;
    std::uint64_t exact_overhead_ = 0;

    std::uint64_t origin_ns_ = 0;
    std::uint32_t now_ms_    = 0;
    std::uint64_t events_    = 0;
    std::uint64_t frees_unknown_ = 0;
    std::uint64_t dropped_at_attach_ = 0;

    /* The whole model's live set, not the displayed region's. The HeatMap knows
     * only about the cells it has, which is one region; these are what the
     * target is actually holding, as far as heapviz has seen. */
    std::uint64_t live_       = 0;
    std::uint64_t live_bytes_ = 0;

    /* The packed coordinate range the grid buckets: always [0, total_bytes).
     * Not addresses -- `RegionMap` converts, and the gutter is the only thing
     * that needs to. */
    AddrRange    bounds_{};
    int          hidden_regions_ = 0; /* always 0 now; every region is shown */
    SessionState state_ = SessionState::Detached;
    int          width_ = 0;
    int          height_ = 0;
    bool         geometry_dirty_ = true;
};

} // namespace hv

#endif /* HEAPVIZ_TUI_HEAP_APP_H */
