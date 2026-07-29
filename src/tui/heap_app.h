/* heapviz - the attached session as a LoopApp (M2.3).
 *
 * Copyright (C) 2026 Parth Sinha
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Everything above this file already existed and was driven by `DemoHeap`; this
 * is the same pipeline with a real target on the front of it. Events come off
 * the ring, land in the `ChunkTable` and the `HeatMap`, and `MapsScanner`
 * supplies the address bounds the grid buckets into. Nothing below is new, which
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
#include "tui/chunk_table.h"
#include "tui/event_loop.h"
#include "tui/heatmap.h"
#include "tui/map_view.h"
#include "tui/proc_maps.h"
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

/* Size of the window of recent allocation addresses used to decide which region
 * the map displays. A power of two so the write is a mask, and small enough that
 * scoring every region against all of it costs nothing at the twice-a-second
 * rate it happens. See `HeapApp::choose_view`. */
constexpr std::size_t kRecentAddrs = 64;

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
    const HeatMap    &map()     const noexcept { return map_; }
    const ChunkTable &table()   const noexcept { return table_; }
    const MapsScanner &maps()   const noexcept { return scanner_; }

private:
    void apply(const HvEvent *events, std::uint32_t n) noexcept;
    std::uint32_t event_ms(std::uint64_t timestamp_ns) const noexcept;
    AddrRange choose_view() noexcept;
    void refit(int w, int h);
    Rect map_area(int w, int h) const noexcept;

    RingSession &session_;
    Capabilities caps_;
    MapView      view_;
    ChunkTable   table_;
    HeatMap      map_;
    Grid         grid_;
    MapsScanner  scanner_;

    std::vector<HvEvent> batch_;

    /* Where the last kRecentAddrs allocations went. Zero means unused. */
    std::uint64_t recent_[kRecentAddrs] = {};
    std::size_t   recent_at_ = 0;

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

    AddrRange    bounds_{};        /* the region being drawn, not the union */
    int          hidden_regions_ = 0;
    SessionState state_ = SessionState::Detached;
    int          width_ = 0;
    int          height_ = 0;
    bool         geometry_dirty_ = true;
};

} // namespace hv

#endif /* HEAPVIZ_TUI_HEAP_APP_H */
