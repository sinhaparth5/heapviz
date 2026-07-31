/* heapviz - the attached session as a LoopApp (M2.3).
 *
 * Copyright (C) 2026 Parth Sinha
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "tui/heap_app.h"

#include "tui/grid.h"
#include "tui/terminal.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace hv {

namespace {

std::uint32_t clamp_u32(std::uint64_t v) noexcept {
    return v > UINT32_MAX ? UINT32_MAX : static_cast<std::uint32_t>(v);
}

} // namespace

HeapApp::HeapApp(RingSession &session, Capabilities caps, Theme theme,
                 bool animations)
    : session_(session), caps_(caps), theme_(theme),
      view_(caps, heat_palette(theme)),
      scanner_(session.pid()), reader_(session.pid()) {
    animations_ = animations;
    view_.set_animations(animations);
    view_.set_half_block(caps.unicode);
    view_.set_style(MapStyle{theme.text, theme.dim, theme.accent, theme.bg,
                             theme.cursor, theme.leak});
    inspect_.set_style(InspectorStyle{
        theme.text, theme.dim, theme.accent, theme.frame, theme.active,
        theme.danger_text, theme.bg});
    metrics_.set_style(MetricsStyle{
        theme.text, theme.dim, theme.accent, theme.frame, theme.title,
        theme.accent, theme.danger_text, theme.bg});
    batch_.resize(kMaxEventsPerFrame); /* once; draining allocates nothing */
    staged_.reserve(kMaxEventsPerFrame);

    /* The clock origin is attach time, not the producer's start time. Anchoring
     * to the producer would be more faithful, and would overflow the model's
     * 32-bit millisecond timestamps for any target that had been running for
     * more than 49 days before heapviz arrived. */
    origin_ns_ = monotonic_ns();

    if (session_.attached()) {
        state_ = SessionState::Live;
        /* Events dropped before we arrived are the target's business, not a
         * gap in what we are showing. D5: the number that matters is the one
         * that grows while somebody is watching. */
        dropped_at_attach_ = session_.dropped();
    }

    table_.reserve(1 << 16);

    enrich_ptrs_.reserve(kEnrichPerPass);
    enrich_want_.reserve(kEnrichPerPass);
    enrich_approx_.reserve(kEnrichPerPass);
    enrich_out_.resize(kEnrichPerPass);
}

std::uint32_t HeapApp::event_ms(std::uint64_t timestamp_ns) const noexcept {
    if (timestamp_ns <= origin_ns_) return 0; /* emitted before we attached */
    return clamp_u32((timestamp_ns - origin_ns_) / 1000000ull);
}

unsigned HeapApp::drain() {
    if (!session_.attached()) return 0;

    if (paused_) {
        const std::uint32_t n = session_.drain(batch_.data(), kMaxEventsPerFrame);
        if (n != 0)
            staged_.insert(staged_.end(), batch_.begin(),
                           batch_.begin() + static_cast<std::ptrdiff_t>(n));
        /* The event loop treats a non-zero return as a visible model change.
         * The ring was drained, but the frozen model was not changed. */
        return 0;
    }

    std::uint32_t applied = 0;

    /* Replay before reading newer ring entries: an allocation and its later
     * free must reach the model in the order the producer published them. */
    const std::size_t waiting = staged_events();
    if (waiting != 0) {
        const std::size_t take =
            std::min<std::size_t>(waiting, kMaxEventsPerFrame);
        apply(staged_.data() + staged_head_, static_cast<std::uint32_t>(take));
        staged_head_ += take;
        applied = static_cast<std::uint32_t>(take);
        if (staged_head_ == staged_.size()) {
            staged_.clear();
            staged_head_ = 0;
        }
    }

    if (applied < kMaxEventsPerFrame && staged_.empty()) {
        const std::uint32_t room = kMaxEventsPerFrame - applied;
        const std::uint32_t n = session_.drain(batch_.data(), room);
        if (n != 0) apply(batch_.data(), n);
        applied += n;
    }

    events_ += applied;
    return applied;
}

void HeapApp::apply(const HvEvent *events, std::uint32_t n) noexcept {
    for (std::uint32_t i = 0; i < n; ++i) {
        const HvEvent &e = events[i];
        const std::uint32_t ms = event_ms(e.timestamp);

        /* The scanner decides for itself whether this means anything; it is two
         * compares against bounds already in cache. Doing it per event rather
         * than per frame is what catches a heap that grew mid-frame. */
        scanner_.note_address(e.ptr);

        if ((e.op & HV_OP_MASK) == HV_OP_FREE) {
            /* A free event carries `usable` but not the original request, so
             * the overhead this chunk contributed can only come from the record
             * of its allocation. Passing zero would tell the map the whole
             * allocation was overhead and subtract it from the wrong counter.
             *
             * A miss here is ordinary rather than an error: every chunk the
             * target allocated before heapviz attached frees exactly once, and
             * is unknown when it does. */
            const Chunk *c = table_.find(e.ptr);
            if (c != nullptr) {
                /* Undo any exact-overhead correction before the free removes
                 * the approximate one. The cell holds a sum it cannot
                 * attribute, so a correction applied on alloc and not reversed
                 * on free leaves its difference behind permanently -- which
                 * shows up as overhead per chunk drifting upward all session. */
                if ((c->flags & kChunkFlagRefined) != 0)
                    map_.refine_overhead(e.ptr, c->overhead_exact,
                                         overhead_of(c->size, c->usable));
                if (c->state == kChunkLive) {
                    if (live_ != 0) --live_;
                    live_bytes_ -= std::min<std::uint64_t>(live_bytes_, c->usable);
                }
                map_.on_free(e.ptr, c->size, c->usable, ms);
                table_.mark_freed(e.ptr, ms);
            } else {
                ++frees_unknown_;
                /* Still recorded: the free happened in that cell whether or not
                 * we saw the malloc, and the flash is what the user is being
                 * shown. `on_free` guards its own decrements. */
                map_.on_free(e.ptr, e.usable, e.usable, ms);
            }
            continue;
        }

        const std::uint32_t size = clamp_u32(e.size);

        /* Counted here rather than read off the HeatMap, which only knows about
         * the one region being displayed. A target allocating mostly from
         * thread arenas would otherwise report a handful of live chunks while
         * holding tens of thousands, and the number that looks like the truth
         * would be the wrong one. */
        const Chunk *prev = table_.find(e.ptr);
        if (prev == nullptr || prev->state != kChunkLive) {
            ++live_;
        } else {
            /* The allocator recycled an address we still think is live: this is
             * a malloc for a free we never saw, not a second live chunk. */
            live_bytes_ -= std::min<std::uint64_t>(live_bytes_, prev->usable);
        }
        live_bytes_ += e.usable;

        /* The one figure nothing else keeps. Everything else on the metrics
         * panel is derived from the live set, which the lines above have just
         * got right; a cumulative total cannot be derived from a live set at
         * all, so it is counted where the events are. */
        metrics_.on_alloc(e.usable);

        map_.on_alloc(e.ptr, size, e.usable, ms);
        table_.insert_live(e.ptr, size, e.usable, ms, hv_event_get_tid(&e));
    }
}

bool HeapApp::update(std::uint64_t now_ns) {
    /* The ring still drains into `staged_`, but every clock-driven and model
     * operation below is frozen. In particular now_ms_ does not advance, so
     * allocation/free heat cannot visibly decay while paused. */
    if (paused_) return false;

    now_ms_ = clamp_u32((now_ns - origin_ns_) / 1000000ull);

    bool changed = false;

    if (state_ == SessionState::Live) {
        /* Reaped before the liveness test, not after it, and the order is the
         * whole correctness of this block. A target heapviz launched is its
         * child, and an unreaped child is a zombie -- for which `kill(pid, 0)`
         * *succeeds*, so `process_alive` says "running" for a process that has
         * already exited. Reaping first is what makes the next line true; doing
         * it afterwards, as this did until the launch path started being used,
         * left the session permanently Live against a dead target and the
         * display saying so only because `ChunkReader` had noticed
         * independently. It is a `waitpid(WNOHANG)` per frame, which returns
         * ECHILD immediately for a target attached to by pid rather than
         * launched, and is the cheapest signal available for one we own.
         *
         * Two independent signals, and both are needed. `producer_exited` is
         * set by the interceptor's destructor, which SIGKILL skips; the pid
         * check catches that, but not a target that has run its destructor and
         * is still winding down. Whichever arrives first ends the session. */
        reap_if_exited(session_.pid());
        if (session_.producer_exited() || !process_alive(session_.pid())) {
            state_ = SessionState::Exited;
            exited_ms_ = now_ms_;
            build_exit_note();
            changed = true;
        }
    }

    if (scanner_.due(now_ms_)) {
        scanner_.scan(now_ms_);
        if (repack()) {
            /* The layout moved, so every cell now describes memory that is no
             * longer at that offset. Only a full rebuild fixes that. */
            geometry_dirty_ = true;
            changed = true;
        }
    }

    /* After the scan and the repack, because the walk is driven by the region
     * list and a stale one would have it reading an arena that has since been
     * unmapped; before everything below, because they all measure the table
     * this replaces. Inert unless snapshot mode is on. */
    const bool walked = snapshot(now_ms_);
    if (walked) changed = true;

    if (geometry_dirty_ && !bounds_.empty() && width_ > 0) {
        refit(width_, height_);
        changed = true;
    }

    if (enrich(now_ms_)) changed = true;

    /* Fragmentation, on its own 4 Hz tick. After `repack`, because it measures
     * chunks against the regions they sit in and a stale region list would put
     * a whole arena's worth of chunks in the "outside" bucket; before the panel
     * is told anything, because the panel only reports what it is handed. */
    if (frag_.analyze(table_, regions_, now_ms_)) {
        metrics_.set_fragmentation(frag_.percent());
        metrics_.set_largest_gap(frag_.largest_gap(), frag_.largest_gap_known());
        changed = true;
    }

    /* M5.5's leak candidates, on the same 4 Hz tick and after the repack for
     * the same reason. `changed` is passed as the force flag: a repack or a
     * rebuild renumbers the cells, and an overlay indexed by the old numbering
     * highlights a different part of the heap -- plausibly, and until the next
     * tick. */
    if (snap_.analyze(table_, map_, now_ms_, changed)) changed = true;

    /* After everything that could have moved the model, and told about it:
     * a repack or a rebuild renumbers the cells, so a selection cached against
     * the old numbering describes a different part of the heap. */
    if (inspect_.refresh(table_, map_, cursor_, now_ms_, changed)) changed = true;

    /* Sampled per frame rather than folded per event, because a peak is a
     * property of a frame: memory the target allocated and freed between two
     * frames was never on screen, and counting it would report a high-water
     * mark for bytes the process may never have held at one time. */
    MetricsSample ms{};
    ms.live_chunks   = live_;
    ms.live_bytes    = live_bytes_;
    ms.ring_queued   = session_.queued();
    ms.ring_capacity = session_.capacity();
    ms.dropped       = session_.dropped() - dropped_at_attach_;
    ms.now_ms        = now_ms_;
    if (metrics_.sample(ms)) changed = true;

    return changed;
}

/* Repacks the regions into one contiguous display space.
 *
 * This replaced M2.3's choose-one-region, and the reason is worth keeping: the
 * union of a brk heap at 0x5b... and a thread arena at 0x70... is 23 TiB of
 * which perhaps 40 MiB is memory, so `Grid` clamped to 1 GiB cells and drew one
 * occupied cell in a screenful of hole. Picking a single region instead avoided
 * that but showed a threaded target's main arena, which is reliably the empty
 * one -- glibc gives every allocating thread its own.
 *
 * Packing them end to end means the grid measures heap bytes, and every arena
 * is on screen at once. The cost is that a flat offset is not an address, which
 * is why `MapView`'s gutter converts back through the same map.
 */
bool HeapApp::repack() noexcept {
    const bool moved = regions_.rebuild(scanner_.regions());

    /* Nothing is hidden any more: every allocatable region is on the map. The
     * count is kept so the header can say how many arenas are in view, which is
     * the thing a user of a threaded program actually wants to know. */
    hidden_regions_ = 0;
    bounds_ = regions_.empty()
                  ? AddrRange{}
                  : AddrRange{0, regions_.total_bytes()};
    return moved;
}

/* Corrects a slice of the live set's overhead from the target's own chunk
 * headers (M2.2).
 *
 * The interceptor reports `usable`, which is what the caller may safely write.
 * The chunk is bigger than that: a header, plus whatever the allocator rounded
 * up to. `usable - size` is therefore an underestimate of what a program is
 * really costing, and the gap is exactly what the overhead display is for.
 *
 * Bounded and resumable rather than a full sweep, because the live set can be
 * hundreds of thousands of chunks and a header read is a syscall per thousand.
 * The cursor walks the table's slots and picks up where it left off.
 */
bool HeapApp::enrich(std::uint32_t now_ms) noexcept {
    /* Never in snapshot mode. The walk has already read every header this would
     * read, and the figure it produces would be nonsense besides: overhead is
     * `usable - request`, and snapshot mode has no request to subtract from --
     * it reports the two as equal precisely because it does not know. */
    if (snapshot_mode_) return false;
    if (!reader_.available() || state_ != SessionState::Live) return false;
    if (static_cast<std::uint32_t>(now_ms - enrich_last_ms_) < kEnrichIntervalMs)
        return false;
    enrich_last_ms_ = now_ms;

    const Chunk *slots = table_.slots();
    const std::size_t n = table_.slot_count();
    if (n == 0) return false;

    enrich_ptrs_.clear();
    enrich_want_.clear();
    enrich_approx_.clear();

    /* One lap at most: without the bound an entirely refined table would spin
     * the cursor over every slot looking for work that is not there. */
    for (std::size_t seen = 0;
         seen < n && enrich_ptrs_.size() < kEnrichPerPass; ++seen) {
        const Chunk &c = slots[enrich_cursor_];
        enrich_cursor_ = (enrich_cursor_ + 1) % n;

        if (c.state != kChunkLive) continue;
        if ((c.flags & kChunkFlagRefined) != 0) continue;
        if (c.key == 0 || c.size == 0) continue;

        enrich_ptrs_.push_back(c.key);
        enrich_want_.push_back(c.size);
        enrich_approx_.push_back(overhead_of(c.size, c.usable));
    }

    if (enrich_ptrs_.empty()) return false;

    const ReadStatus st = reader_.read(enrich_ptrs_.data(), enrich_want_.data(),
                                       enrich_ptrs_.size(), enrich_out_.data());
    if (st == ReadStatus::Denied || st == ReadStatus::TargetGone) return true;

    bool changed = false;
    for (std::size_t i = 0; i < enrich_ptrs_.size(); ++i) {
        const ChunkInfo &info = enrich_out_[i];
        if (!info.valid) continue;

        /* Marked before the fold rather than after, and marked even when the
         * cell is out of view: a chunk in a region the map is not showing must
         * not be re-read on every pass for the rest of the session. */
        /* Recorded before the fold, and only folded if it was recorded: a
         * correction the table cannot remember is one that can never be undone
         * when the chunk is freed. */
        /* The header bits ride along with the overhead: the same read decoded
         * both, and M5.2's `MMAPPED` status has no other source. Reading them
         * off the address instead would be guessing at glibc's arena layout,
         * which is wrong for any target using a different allocator. */
        const auto header = static_cast<std::uint8_t>(
            (info.mmapped ? kChunkFlagMmapped : 0) |
            (info.non_main_arena ? kChunkFlagNonMain : 0));
        if (!table_.mark_refined(enrich_ptrs_[i], info.overhead, header))
            continue;
        ++refined_;
        exact_overhead_ += info.overhead;

        if (map_.refine_overhead(enrich_ptrs_[i], enrich_approx_[i],
                                 info.overhead))
            changed = true;
    }
    return changed;
}

void HeapApp::enable_snapshots() {
    snapshot_mode_ = true;
    walker_.emplace(session_.pid());

    /* Live from the start. There is no ring to claim and therefore no attach to
     * succeed, so the only thing that can end this session is the target
     * exiting -- which `update` already checks for by pid. */
    state_ = SessionState::Live;

    /* Nothing here can produce a cumulative figure, and the panel has to say so
     * rather than show a zero. Same for the inspector's lifetime, which in this
     * mode counts from heapviz's first sighting rather than from the
     * allocation -- a floor, and labelled as one. */
    metrics_.set_cumulative_known(false);
    inspect_.set_ages_from_first_sighting(true);
}

/* One snapshot pass: read the target's heap and replace the model with it.
 *
 * The shape is the opposite of the event path. `apply` folds a delta into a
 * model it trusts; this discards the model and rebuilds it, because a walk
 * carries no history and cannot say which of the differences between two walks
 * was an allocation and which was a free. The live set is the only thing a
 * chunk header can prove, so the live set is all this claims.
 *
 * Cost is one pass over the table to carry timestamps forward, then a clear and
 * a re-insert -- O(live set) at 4 Hz, which is around 2.5 ms for 100k chunks.
 * That is affordable at this tick and would not be at frame rate, which is part
 * of why the tick exists.
 */
bool HeapApp::snapshot(std::uint32_t now_ms) noexcept {
    if (!snapshot_mode_ || !walker_.has_value()) return false;
    if (state_ != SessionState::Live || paused_) return false;
    if (walked_once_ &&
        static_cast<std::uint32_t>(now_ms - walk_last_ms_) < walk_interval_ms_)
        return false;
    walk_last_ms_ = now_ms;
    walked_once_  = true;

    /* Timed rather than estimated from the chunk count, because the two halves
     * of the cost scale with different things -- the walk with bytes of heap,
     * the rebuild with number of chunks -- and a target with one huge arena and
     * a target with a million tiny chunks are nothing alike. */
    const std::uint64_t began_ns = monotonic_ns();

    const WalkResult r = walker_->walk(scanner_.regions(), walked_);
    walk_ = r;

    /* Neither of these can be fixed by walking again, and both leave `walked_`
     * empty -- so replacing the model with it would clear a display that is
     * still the last true thing heapviz knows. The header reports the status;
     * what is on screen stays. */
    if (r.status == WalkStatus::Denied || r.status == WalkStatus::TargetGone) {
        walk_cost_ms_     = 0;
        walk_interval_ms_ = kSnapshotIntervalMs;
        return true;
    }

    /* First-seen times, read out of the outgoing table before it is cleared,
     * because it is the only record of them. A chunk header has no timestamp,
     * so "when heapviz first saw this address" is the only clock available --
     * and without carrying it, every cell would re-flash as newly allocated
     * four times a second and the heat would mean nothing at all. */
    walked_first_ms_.clear();
    walked_first_ms_.reserve(walked_.size());
    for (const WalkedChunk &c : walked_) {
        const Chunk *prev = table_.find(c.user_ptr);
        walked_first_ms_.push_back(
            prev != nullptr && prev->state == kChunkLive ? prev->alloc_ms
                                                         : now_ms);
    }

    table_.clear();
    live_       = 0;
    live_bytes_ = 0;

    for (std::size_t i = 0; i < walked_.size(); ++i) {
        const WalkedChunk &c = walked_[i];
        const std::uint32_t usable = clamp_u32(c.usable);

        /* Request and usable are the same number here, and deliberately. The
         * request is what the program asked for, which the allocator rounded
         * away before writing the header -- so it is not recoverable from
         * outside and heapviz must not invent it. Passing usable for both makes
         * the overhead figure read zero, which is the truthful answer: heapviz
         * did not see the call. */
        table_.insert_live(c.user_ptr, usable, usable, walked_first_ms_[i], 0);
        ++live_;
        live_bytes_ += c.usable;
    }

    /* The incremental path is not available here -- a walk is a new live set,
     * not a delta -- so the rebuild has to be asked for outright. Inside the
     * timed region deliberately: it is fully half of what a pass costs, and
     * pacing against the walk alone would be pacing against half the truth.
     * A no-op until the grid has been configured, which `refit` does. */
    map_.rebuild(table_);

    walk_cost_ms_ = clamp_u32((monotonic_ns() - began_ns) / 1000000ull);
    walk_interval_ms_ =
        std::max(kSnapshotIntervalMs, walk_cost_ms_ * kSnapshotDutyFactor);
    return true;
}

void HeapApp::refit(int, int) {
    map_.set_regions(&regions_);
    grid_.set_bounds(bounds_.base, bounds_.end);
    if (!fit_grid(grid_, layout_.map, caps_.unicode, layout_.legend)) return;

    /* A granularity change invalidates every aggregate, and rebuilding from the
     * chunk table is the only correct response. `configure` says whether that
     * actually happened, so a resize that changed nothing costs nothing. */
    if (map_.configure(grid_)) {
        map_.rebuild(table_);
        /* `rebuild` recomputes every cell from `overhead_of`, which undoes every
         * exact figure the enrichment pass folded in. Records still marked
         * refined would then never be corrected again, so the marks go with the
         * corrections. */
        table_.clear_refined();
        refined_ = 0;
        exact_overhead_ = 0;
    }

    /* After the grid, never before: the cursor holds a coordinate and resolves
     * it against whatever geometry is current, so re-clamping it against the
     * old grid would pin it to a cell that is about to stop existing. */
    cursor_.refit(grid_);
    geometry_dirty_ = false;
}

void HeapApp::set_launch(const std::string &output_path,
                         const std::string &argv0) {
    launch_log_   = output_path;
    launch_argv0_ = argv0;
}

/* Built once, on the transition to Exited, and never in `draw`: this opens a
 * file, and the frame path may not.
 *
 * Two sentences are possible and only one is shown, because the footer is one
 * line. The `--instrument` hint wins when the target died in the first seconds,
 * since then it is both the likeliest explanation and the only one the user can
 * act on; otherwise the target's own last line is quoted, which for a program
 * that ran and finished is the more honest thing to say. A target heapviz did
 * not launch gets neither: its stdin was never heapviz's doing, and there is no
 * log to quote. */
void HeapApp::build_exit_note() {
    exit_note_.clear();
    if (launch_log_.empty()) return;

    if (exited_ms_ != 0 && exited_ms_ < kInteractiveExitMs) {
        exit_note_ = "needs a terminal? run: heapviz --instrument ";
        exit_note_ += launch_argv0_;
        return;
    }
    exit_note_ = last_output_line(launch_log_);
}

bool HeapApp::key(char byte) {
    /* `q` first, and unconditionally. The cursor bindings are vim's, and vim's
     * do not include `q` -- but a future mode that binds it would otherwise take
     * the one key the loop has no opinion about, and a heapviz that cannot be
     * quit is a terminal that has to be killed. */
    if (byte == 'q') { request_quit(); return false; }

    if (byte == '?') {
        help_visible_ = !help_visible_;
        return true;
    }

    if (byte == ' ' && (state_ == SessionState::Live || paused_)) {
        paused_ = !paused_;
        help_visible_ = false;
        return true;
    }

    if (byte == 'r' && state_ != SessionState::Detached) {
        reset_stats();
        return true;
    }

    /* A modal overlay and a frozen display have deliberately small, explicit
     * key sets. If a key is not in their footer it cannot mutate state behind
     * the mode and surprise the user when it closes. */
    if (help_visible_ || paused_) return false;

    CursorMove m{};
    if (cursor_move_for_key(byte, m)) {
        if (!cursor_.move(map_, m)) return false;
        /* Immediately rather than on the next 4 Hz tick: the panel is the
         * answer to the keypress, and a quarter-second lag between moving the
         * cursor and the details catching up reads as the tool being slow. */
        inspect_.refresh(table_, map_, cursor_, now_ms_, true);
        return true;
    }

    /* Tab cycles the chunks sharing the cursor's cell. A cell is a span of
     * addresses and usually holds several, so without this the panel could only
     * ever name the largest. */
    if (byte == '\t') return inspect_.cycle();

    /* M5.5. `s` marks now, `S` drops the mark, `d` shows what has accumulated
     * since it. All three only redraw; the pass itself runs from `update`,
     * which is where the model lives.
     *
     * `d` before `s` is deliberately inert rather than implicitly marking:
     * a display toggle that silently moves the reference point would answer
     * "what has leaked" with "nothing, so far", which is true of every heap one
     * millisecond after you start looking at it and is not what the key was
     * pressed to find out. The footer offers `d` only once there is a mark. */
    if (state_ == SessionState::Live) {
        if (byte == 's') { snap_.take(now_ms_); return true; }
        if (byte == 'S') { snap_.clear(); return true; }
        if (byte == 'd') { snap_.toggle(); return snap_.has_snapshot(); }
    }
    return false;
}

bool HeapApp::animating() const {
    return animations_ && !paused_ && MapView::animating(map_, now_ms_);
}

bool HeapApp::take_stats_reset() {
    const bool requested = reset_loop_stats_;
    reset_loop_stats_ = false;
    return requested;
}

void HeapApp::reset_stats() noexcept {
    events_ = 0;
    frees_unknown_ = 0;
    dropped_at_attach_ = session_.dropped();

    MetricsSample baseline{};
    baseline.live_chunks   = live_;
    baseline.live_bytes    = live_bytes_;
    baseline.ring_queued   = session_.queued();
    baseline.ring_capacity = session_.capacity();
    baseline.dropped       = 0;
    baseline.now_ms        = now_ms_;
    metrics_.reset(baseline);
    reset_loop_stats_ = true;
}

void HeapApp::resized(int w, int h) {
    width_  = w;
    height_ = h;
    layout_ = solve_layout(w, h);
    view_.set_show_legend(layout_.legend);
    geometry_dirty_ = true;
    if (!bounds_.empty()) refit(w, h);
}

void HeapApp::draw_help(Framebuffer &fb) const noexcept {
    const int box_w = std::min(68, std::max(4, fb.width() - 4));
    const int box_h = std::min(17, std::max(3, fb.height() - 4));
    const Rect box{(fb.width() - box_w) / 2, (fb.height() - box_h) / 2,
                   box_w, box_h};

    fb.fill(box, Cell{U' ', theme_.text, theme_.bg, 0});
    fb.box(box, caps_.unicode ? BoxStyle::Rounded : BoxStyle::Ascii,
           theme_.title, theme_.bg);

    int row = 1;
    const auto line = [&](const char *text, Rgb colour,
                          std::uint8_t attrs = kAttrNone) {
        panel_text(fb, box, 2, row, text, colour, theme_.bg, attrs);
        ++row;
    };

    line("KEYBOARD HELP", theme_.title, kAttrBold);
    line("", theme_.text);
    line("q          quit", theme_.text);
    line("?          close this help", theme_.text);
    line("Space      pause / resume (live target)", theme_.text);
    line("r          reset counters, peaks, drops, and frame stats", theme_.text);
    line("", theme_.text);
    line("h j k l    move cursor       H J K L    move x10", theme_.text);
    line("g / G      first / last cell", theme_.text);
    line("n / N      next / previous occupied cell", theme_.text);
    line("Tab        cycle chunks in the selected cell", theme_.text);
    line("", theme_.text);
    line("s          take snapshot     d          toggle diff", theme_.text);
    line("S          clear snapshot", theme_.text);
    if (paused_)
        line("Display paused; ring events are still being drained.", theme_.accent);
}

void HeapApp::draw(Framebuffer &fb, const LoopStats &stats) {
    const int w = fb.width();
    const int h = fb.height();

    fb.clear(Cell{U' ', theme_.text, theme_.bg, 0});

    char line[256];

    /* Row 0: what is being watched. */
    const char *comm = session_.comm();
    std::snprintf(line, sizeof line, " heapviz v0.1   [PID: %d%s%s]%s",
                  session_.pid(), comm[0] != '\0' ? " - " : "",
                  comm[0] != '\0' ? comm : "",
                  /* Named on the title row rather than tucked into a status
                   * line, because every figure below it means something
                   * different in this mode: no events, no history, and a live
                   * set that is a sample rather than a running total. A user who
                   * does not know which mode they are in will read the numbers
                   * as the other one's. */
                  snapshot_mode_ ? "   SNAPSHOT - sampled, no events" : "");
    fb.text(0, 0, line, theme_.title, theme_.bg, kAttrBold);

    std::snprintf(line, sizeof line, "%.0f fps ", stats.fps);
    const auto rlen = static_cast<int>(std::strlen(line));
    if (w > rlen) fb.text(w - rlen, 0, line, theme_.dim, theme_.bg);

    /* M5.5's banner, on the title row rather than on a row of its own.
     *
     * A fourth header row would move the map down, and the map's geometry
     * decides how many bytes a cell covers: toggling diff mode would re-bucket
     * the whole address space, so every cell would change meaning on a keypress
     * and the thing the mode exists to point at would move while you looked at
     * it. The banner therefore has to fit in the rows that are already there.
     *
     * The figures are the whole live set's, not the map's -- `offmap` is
     * reported separately below rather than being quietly dropped, because a
     * summary saying 900 over a map highlighting 850 needs a number to explain
     * it, not a discrepancy for the user to find. */
    if (snap_.diff_mode()) {
        const LeakReport &lr = snap_.report();
        char count[24], bytes[32];
        format_count(count, sizeof count, lr.chunks);
        format_byte_size(bytes, sizeof bytes, lr.bytes);
        if (lr.offmap != 0) {
            char off[24];
            format_count(off, sizeof off, lr.offmap);
            std::snprintf(line, sizeof line,
                          " [DIFF] %s chunks / %s leaked since %s (%s off map) ",
                          count, bytes, snap_.taken_at(), off);
        } else {
            std::snprintf(line, sizeof line,
                          " [DIFF] %s chunks / %s leaked since %s ", count, bytes,
                          snap_.taken_at());
        }
        const auto blen = static_cast<int>(std::strlen(line));
        if (w > rlen + blen) fb.text(w - rlen - blen, 0, line, theme_.leak, theme_.bg,
                                     kAttrBold);
    }

    /* Row 1: where the heap is, and which arenas it is coming from. */
    char arena[kArenaLabelMax];
    scanner_.arena_label(arena, sizeof arena);
    if (bounds_.empty()) {
        std::snprintf(line, sizeof line,
                      " Heap Address Range: (not yet known) | Active Arena: %s",
                      arena);
    } else {
        char cell[32], span[32];
        format_byte_size(cell, sizeof cell, grid_.cell_bytes());
        format_byte_size(span, sizeof span, regions_.total_bytes());
        /* Real addresses, from the first and last regions, with the packed
         * total beside them. `bounds_` is a packed range and printing it as an
         * address would be printing a coordinate that exists nowhere in the
         * target. */
        const std::uint64_t lo = regions_.spans().front().start;
        const std::uint64_t hi = regions_.spans().back().end;
        std::snprintf(line, sizeof line,
                      " Heap Address Range: 0x%012llx - 0x%012llx"
                      " | Active Arena: %s | %s mapped | 1 cell = %s",
                      static_cast<unsigned long long>(lo),
                      static_cast<unsigned long long>(hi), arena, span, cell);
    }
    fb.text(0, 1, line, theme_.text, theme_.bg);

    if (regions_.count() > 1) {
        /* Named rather than silent: rows in the gutter restart at each region,
         * and a reader has to know that a seam is a seam and not a gap in the
         * heap. */
        std::snprintf(line, sizeof line, " %zu regions packed ",
                      regions_.count());
        const auto len = static_cast<int>(std::strlen(line));
        if (w > len) fb.text(w - len, 1, line, theme_.accent, theme_.bg);
    }

    /* Row 2: the model, and then whatever is wrong with it. */
    constexpr const char *section = " SPATIAL HEAP MAP ";
    const int section_len = static_cast<int>(std::strlen(section));
    if (w >= section_len) {
        fb.hline(0, 2, w, caps_.unicode ? U'─' : U'-', theme_.frame, theme_.bg);
        fb.text((w - section_len) / 2, 2, section, theme_.accent, theme_.bg,
                kAttrBold);
    }

    const std::uint64_t dropped = session_.dropped() - dropped_at_attach_;

    /* Overhead, and how much of it is measured rather than inferred. The count
     * matters: the figure is exact only for the chunks the enrichment pass has
     * reached, and presenting a partly-corrected total as though it were the
     * whole truth is the kind of quiet lie this tool exists not to tell. */
    const char *why = reader_.hint();
    if (snapshot_mode_) {
        /* Snapshot mode reports what it could not see, not what it measured.
         * The number that matters here is `opaque_bytes`: a runtime that mmaps
         * its own heap -- V8, Bun, the JVM -- has no chunk headers to follow, so
         * "4 MB live" against a 600 MB process is a correct answer to a question
         * the user did not ask. Saying how much is unreadable is what stops the
         * live figure being read as the whole truth. */
        const char *wh = walker_.has_value() ? walker_->hint() : nullptr;
        if (wh != nullptr) {
            std::snprintf(line, sizeof line, " snapshot: %s ", wh);
        } else {
            char human[32];
            format_byte_size(human, sizeof human, walk_.opaque_bytes);
            std::snprintf(line, sizeof line, " snapshot: %s not readable%s ",
                          human, walk_.truncated ? ", truncated" : "");
        }
    } else if (why != nullptr) {
        std::snprintf(line, sizeof line, " overhead: %s ", why);
    } else if (refined_ != 0) {
        std::snprintf(line, sizeof line, " overhead: %llu B exact over %llu chunks ",
                      static_cast<unsigned long long>(exact_overhead_),
                      static_cast<unsigned long long>(refined_));
    } else {
        line[0] = '\0';
    }
    if (line[0] != '\0') {
        const auto len = static_cast<int>(std::strlen(line));
        if (w > len && dropped == 0)
            fb.text(w - len, 2, line, theme_.dim, theme_.bg);
    }

    /* Dropped events are reported the moment they appear rather than folded
     * into a health percentage. Ground rule: a profiler that quietly lies about
     * what it missed is worse than no profiler, and after one overflow the
     * chunk table has a phantom leak in it that nothing can resync (D5). */
    if (dropped != 0) {
        std::snprintf(line, sizeof line, " %llu events DROPPED - display is incomplete ",
                      static_cast<unsigned long long>(dropped));
        const auto len = static_cast<int>(std::strlen(line));
        if (w > len)
            fb.text(w - len, 2, line, theme_.danger_text, theme_.bg, kAttrBold);
    }

    const Rect area = layout_.map;
    view_.draw(fb, area, map_, now_ms_);
    /* Before the cursor, so a candidate cell the cursor is sitting on still
     * shows where the cursor is. */
    view_.draw_leaks(fb, area, map_, snap_);
    view_.draw_cursor(fb, area, map_, cursor_);

    /* M5.2's panel, which is where the cursor's address now lives. It prints
     * the real address rather than the packed coordinate: the coordinate exists
     * nowhere in the target, so it is not a number anyone could look up in a
     * debugger. */
    const Rect panel = layout_.inspector;
    if (panel.h > 0)
        inspect_.draw(fb, panel, grid_, cursor_, &regions_, now_ms_);

    /* M5.3's panel, sharing the block. The two never overlap because both come
     * out of `metrics_cols`, which is the only thing that knows where the
     * boundary is. */
    const Rect stats_panel = layout_.metrics;
    if (stats_panel.w > 0) metrics_.draw(fb, stats_panel);

    /* The footer: the session's state, in the words the user needs. */
    /* Initialised rather than left to the switch below. It covers every
     * enumerator, but the compiler will not take an enum's declared range as a
     * promise about its value -- and at -O3 that becomes a -Werror. */
    const char *note = " q quit";
    Rgb colour = theme_.dim;
    if (help_visible_) {
        if (paused_)
            note = " HELP   q quit   ? close   r reset   Space resume";
        else if (state_ == SessionState::Live)
            note = " HELP   q quit   ? close   r reset   Space pause";
        else if (state_ == SessionState::Exited)
            note = " HELP   q quit   ? close   r reset";
        else
            note = " HELP   q quit   ? close";
        colour = theme_.title;
    } else if (paused_) {
        std::snprintf(line, sizeof line,
                      " PAUSED   q quit   ? help   Space resume   r reset"
                      "   %zu events staged",
                      staged_events());
        note = line;
        colour = theme_.accent;
    } else {
        switch (state_) {
        case SessionState::Live:
            /* Conditional bindings only appear when they can act. In
             * particular `d`/`S` require a mark and pause only exists while a
             * producer is live. */
            std::snprintf(line, sizeof line,
                          " %s q quit  ? help  Space pause  r reset"
                          "  hjkl/HJKL move  g/G ends  n/N chunk  s snap%s",
                          snap_.diff_mode() ? "DIFF " : "",
                          snap_.has_snapshot() ? "  d diff  S drop" : "");
            note = line;
            colour = snap_.diff_mode() ? theme_.leak : theme_.dim;
            break;
        case SessionState::Exited:
            /* The reason displaces the movement keys rather than sharing the
             * line with them. A user reading this needs to know why the display
             * stopped changing, and the cursor still works whether or not it is
             * advertised -- but a reason that scrolled off the right edge would
             * not be there at all. */
            if (!exit_note_.empty())
                std::snprintf(line, sizeof line, " TARGET EXITED: %s   q quit",
                              exit_note_.c_str());
            else
                std::snprintf(line, sizeof line,
                              " TARGET EXITED   q quit  ? help  r reset"
                              "  hjkl/HJKL move  g/G ends  n/N chunk");
            note = line;
            colour = theme_.accent;
            break;
        case SessionState::Detached:
            note = " NOT ATTACHED   q quit  ? help";
            colour = theme_.dim;
            break;
        }
    }
    fb.text(0, h - 1, note, colour, theme_.bg);

    /* Last, so the modal wins over every model layer while the footer remains
     * visible and truthful about the keys that can close it. */
    if (help_visible_) draw_help(fb);
}

} // namespace hv
