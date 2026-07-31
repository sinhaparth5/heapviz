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
#include "tui/cursor.h"
#include "tui/event_loop.h"
#include "tui/fragmentation.h"
#include "tui/heap_walker.h"
#include "tui/heatmap.h"
#include "tui/inspector.h"
#include "tui/layout.h"
#include "tui/map_view.h"
#include "tui/metrics.h"
#include "tui/proc_maps.h"
#include "tui/region_map.h"
#include "tui/session.h"
#include "tui/snapshot.h"
#include "tui/theme.h"

#include <cstdint>
#include <optional>
#include <string>
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

/* A launched target that dies this soon never got far enough to be profiling
 * anything, and by far the commonest reason is that it wanted a terminal: heapviz
 * owns this one, so the child was given `/dev/null` on stdin. Generous rather
 * than tight, because the cost of a false positive is one extra sentence and the
 * cost of a false negative is a user staring at a frozen map with no idea why. */
constexpr std::uint32_t kInteractiveExitMs = 3000;

/* How often the snapshot mode re-walks the target's heap.
 *
 * A walk is a syscall per 256 KiB of heap and a full rebuild of the chunk table
 * and every cell, so it is far too expensive to run per frame -- and pointless
 * at that rate, since it is a *sample* rather than a stream and nothing between
 * two samples is observable anyway. 4 Hz is the same tick the fragmentation and
 * leak passes already use, and is fast enough that the display tracks a heap
 * being watched by a human. */
constexpr std::uint32_t kSnapshotIntervalMs = 250;

/* The share of the frame thread a snapshot pass is allowed to occupy.
 *
 * A pass is not incremental: it reads the heap, replaces the chunk table and
 * rebuilds every cell, and all three scale with the live set. Measured against
 * a 200k-chunk child it is ~11 ms of walk and roughly as much again of rebuild,
 * which is most of a 16 ms frame -- fine four times a second, and not fine on a
 * heap several times larger.
 *
 * So the interval is not fixed: whatever the last pass cost, the next one waits
 * at least this many times as long, which bounds the work to 1/N of the frame
 * thread however big the heap turns out to be. Large heaps lose refresh rate,
 * which is the right thing to give up -- the display is a 4 Hz sample of a
 * quantity that moves slowly, and a stuttering cursor is far more obviously
 * broken than a map that updates twice a second instead of four times. */
constexpr std::uint32_t kSnapshotDutyFactor = 8;

/* How the session is doing, in the order the status line reports them. */
enum class SessionState : std::uint8_t {
    Live,      /* attached, target running                              */
    Exited,    /* the target is gone; what is on screen is its last state */
    Detached,  /* never attached, or released                            */
};

class HeapApp final : public LoopApp {
public:
    HeapApp(RingSession &session, Capabilities caps,
            Theme theme = dark_theme(), bool animations = true);

    unsigned drain() override;
    bool     update(std::uint64_t now_ns) override;
    bool     key(char byte) override;
    bool     animating() const override;
    bool     take_stats_reset() override;
    void     resized(int w, int h) override;
    void     draw(Framebuffer &fb, const LoopStats &stats) override;

    /* What heapviz launched, when it launched it: the target's output log and
     * the command that produced it. Never set when attaching to a pid heapviz
     * did not start, because then neither the log nor the stdin decision is
     * heapviz's to explain.
     *
     * Given to the app rather than kept in `main` because the answer is needed
     * on screen the moment the target dies. Reporting it only in the exit
     * summary means the reason is behind a keypress, and a user staring at a
     * frozen map has no way to know there is anything to press. */
    void set_launch(const std::string &output_path, const std::string &argv0);

    /* Turns on snapshot mode: no ring, no events, the live set recovered by
     * reading the target's own chunk headers on a timer (M2.5).
     *
     * This is what `heapviz <pid>` does for a process that was not started
     * under the interceptor -- which is most of them, since `LD_PRELOAD` binds
     * at load time and nothing can retrofit it onto a running process. The
     * session is `observe`d rather than attached, so every ring path in here
     * stays on its already-existing "not attached" branch and the difference is
     * confined to `snapshot()`.
     *
     * What is lost is everything that needs a timeline: there are no allocation
     * timestamps in a heap, so heat is aged from when heapviz *first saw* an
     * address rather than from when the target allocated it, and the cumulative
     * and peak figures have no source at all. The UI says which mode it is in
     * for exactly that reason. */
    void enable_snapshots();

    bool snapshot_mode() const noexcept { return snapshot_mode_; }

    /* The last walk's result, for the header and for tests. Zeroed until the
     * first tick has run. */
    const WalkResult &walk() const noexcept { return walk_; }

    /* What the last snapshot pass cost, and how long the next one will wait as
     * a result. Exposed so a test can assert the pacing rather than infer it
     * from a stopwatch, which would be measuring the machine. */
    std::uint32_t walk_cost_ms() const noexcept { return walk_cost_ms_; }
    std::uint32_t walk_interval_ms() const noexcept { return walk_interval_ms_; }

    /* For the exit summary and for tests, which need to assert on what the
     * session saw without scraping the framebuffer. */
    SessionState state()        const noexcept { return state_; }

    /* Milliseconds into the session at which the target was noticed gone, and 0
     * while it is still running. The exit summary uses it to tell a target that
     * ran and finished from one that could not start: a program launched by
     * heapviz gets `/dev/null` on stdin, and one that needs a terminal dies in
     * the first second or two -- which is a diagnosis heapviz can offer rather
     * than leaving the user to read "target exited" and guess. */
    std::uint32_t exited_ms()   const noexcept { return exited_ms_; }

    /* The footer's one-line explanation of why the target is gone, or empty when
     * there is nothing to add to "TARGET EXITED". Public so a test can assert on
     * the sentence rather than scraping it back out of the framebuffer. */
    const std::string &exit_note() const noexcept { return exit_note_; }
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
    const MapCursor   &cursor() const noexcept { return cursor_; }
    const ChunkInspector &inspector() const noexcept { return inspect_; }
    const Metrics     &metrics() const noexcept { return metrics_; }
    const FragAnalyzer &fragmentation() const noexcept { return frag_; }
    const SnapshotDiff &snapshot() const noexcept { return snap_; }
    const Grid        &grid()   const noexcept { return grid_; }
    std::uint64_t refined_chunks() const noexcept { return refined_; }
    std::uint64_t exact_overhead() const noexcept { return exact_overhead_; }
    bool paused() const noexcept { return paused_; }
    bool help_visible() const noexcept { return help_visible_; }
    std::size_t staged_events() const noexcept {
        return staged_.size() - staged_head_;
    }

private:
    void apply(const HvEvent *events, std::uint32_t n) noexcept;
    void reset_stats() noexcept;
    void draw_help(Framebuffer &fb) const noexcept;
    std::uint32_t event_ms(std::uint64_t timestamp_ns) const noexcept;
    /* Repacks the regions and reports whether the layout moved. Replaces
     * M2.3's choose-one-region, which showed a threaded target's main arena --
     * reliably the empty one. */
    bool repack() noexcept;

    /* Reads exact chunk overhead for a bounded slice of the live set and folds
     * it in, replacing the interceptor's `usable - size` approximation. Returns
     * true if anything changed. */
    bool enrich(std::uint32_t now_ms) noexcept;

    /* One pass of snapshot mode: walk, then replace the live set with what the
     * walk found. Returns true if anything on screen changed. */
    bool snapshot(std::uint32_t now_ms) noexcept;
    void refit(int w, int h);

    RingSession &session_;
    Capabilities caps_;
    Theme        theme_;
    MapView      view_;
    ChunkTable   table_;
    HeatMap      map_;
    Grid         grid_;
    MapsScanner  scanner_;
    RegionMap    regions_;
    ChunkReader  reader_;
    MapCursor    cursor_;
    ChunkInspector inspect_;
    Metrics        metrics_;
    FragAnalyzer   frag_;
    SnapshotDiff   snap_;

    std::vector<HvEvent> batch_;
    /* Events removed from the ring while the display is paused. `staged_head_`
     * makes replay a cursor over contiguous storage rather than an O(n) erase
     * per frame; the vector is cleared once the cursor reaches its end. */
    std::vector<HvEvent> staged_;
    std::size_t staged_head_ = 0;

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

    /* Snapshot mode (M2.5). Absent unless `enable_snapshots` was called, which
     * is how every path above stays on its ring-driven default. */
    std::optional<HeapWalker> walker_;
    WalkResult                walk_{};
    std::vector<WalkedChunk>  walked_;
    /* When heapviz first saw each address in `walked_`, parallel to it.
     *
     * A chunk header carries no timestamp, so the honest thing to age a cell by
     * is not when the target allocated it -- unknowable -- but when this
     * process first observed it. Carrying it across walks is what stops the
     * whole map re-flashing as newly allocated four times a second. */
    std::vector<std::uint32_t> walked_first_ms_;
    std::uint32_t walk_last_ms_     = 0;
    std::uint32_t walk_cost_ms_     = 0;
    std::uint32_t walk_interval_ms_ = kSnapshotIntervalMs;
    bool          walked_once_   = false;
    bool          snapshot_mode_ = false;

    std::uint64_t origin_ns_ = 0;
    std::uint32_t now_ms_    = 0;
    void build_exit_note();

    std::uint32_t exited_ms_ = 0;
    std::string   launch_log_;   /* the target's output, when we launched it */
    std::string   launch_argv0_;
    std::string   exit_note_;    /* built once, on the transition to Exited  */
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
    bool         paused_ = false;
    bool         help_visible_ = false;
    bool         reset_loop_stats_ = false;
    bool         animations_ = true;
    AppLayout    layout_{};
};

} // namespace hv

#endif /* HEAPVIZ_TUI_HEAP_APP_H */
