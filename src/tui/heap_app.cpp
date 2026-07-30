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

/* The chrome, until M6.1's theme owns these. */
constexpr Rgb kInk   = 0x00D8D8D8;
constexpr Rgb kDim   = 0x007A7A7A;
constexpr Rgb kWarn  = 0x00F5A623;
constexpr Rgb kBad   = 0x00E05252;
constexpr Rgb kGood  = 0x0058C7F3;
constexpr Rgb kPanel = 0x000C0C0C;
/* M5.5's diff mode, and the same value as `MapStyle::leak` on purpose: the
 * banner and the cells it describes have to be recognisably one thing. */
constexpr Rgb kLeak  = 0x00E040FB;

/* Three header rows and one status row, both fixed rather than sized to their
 * contents. A map whose top edge moved when a banner appeared would re-bucket
 * the whole address space at the moment the user most wants to read it. */
constexpr int kHeaderRows = 3;
constexpr int kFooterRows = 1;

/* The two bottom panels share their rows, so the row gate can be one gate. */
static_assert(kMetricsRows == kInspectorRows,
              "the bottom block is one band of rows, not two");

std::uint32_t clamp_u32(std::uint64_t v) noexcept {
    return v > UINT32_MAX ? UINT32_MAX : static_cast<std::uint32_t>(v);
}

} // namespace

HeapApp::HeapApp(RingSession &session, Capabilities caps)
    : session_(session), caps_(caps), view_(caps),
      scanner_(session.pid()), reader_(session.pid()) {
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
        /* Two independent signals, and both are needed. `producer_exited` is
         * set by the interceptor's destructor, which SIGKILL skips; the pid
         * check catches that, but not a target that has run its destructor and
         * is still winding down. Whichever arrives first ends the session. */
        if (session_.producer_exited() || !process_alive(session_.pid())) {
            state_ = SessionState::Exited;
            reap_if_exited(session_.pid());
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

void HeapApp::refit(int w, int h) {
    map_.set_regions(&regions_);
    grid_.set_bounds(bounds_.base, bounds_.end);
    if (!fit_grid(grid_, map_area(w, h))) return;

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
    return !paused_ && MapView::animating(map_, now_ms_);
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
    geometry_dirty_ = true;
    if (!bounds_.empty()) refit(w, h);
}

bool HeapApp::inspector_fits(int h) const noexcept {
    return h - kHeaderRows - kFooterRows - kInspectorRows >= kInspectorMinMapRows;
}

Rect HeapApp::map_area(int w, int h) const noexcept {
    const int top = kHeaderRows;
    const int panel = inspector_fits(h) ? kInspectorRows : 0;
    return Rect{0, top, w, h - top - kFooterRows - panel};
}

int HeapApp::metrics_cols(int w) const noexcept {
    return metrics_split(w, kInspectorMinCols);
}

Rect HeapApp::inspector_area(int w, int h) const noexcept {
    if (!inspector_fits(h)) return Rect{};
    return Rect{0, h - kFooterRows - kInspectorRows, w - metrics_cols(w),
                kInspectorRows};
}

Rect HeapApp::metrics_area(int w, int h) const noexcept {
    /* Tied to the inspector's row gate rather than having its own: the two
     * occupy the same rows, so a terminal that cannot spare them for one cannot
     * spare them for the other. M6.2's solved layout is where a short-and-wide
     * terminal gets to keep the metrics and lose the inspector. */
    if (!inspector_fits(h)) return Rect{};
    const int mw = metrics_cols(w);
    if (mw == 0) return Rect{};
    return Rect{w - mw, h - kFooterRows - kMetricsRows, mw, kMetricsRows};
}

void HeapApp::draw_help(Framebuffer &fb) const noexcept {
    const int box_w = std::min(68, std::max(4, fb.width() - 4));
    const int box_h = std::min(17, std::max(3, fb.height() - 4));
    const Rect box{(fb.width() - box_w) / 2, (fb.height() - box_h) / 2,
                   box_w, box_h};

    fb.fill(box, Cell{U' ', kInk, kPanel, 0});
    fb.box(box, BoxStyle::Rounded, kGood, kPanel);

    int row = 1;
    const auto line = [&](const char *text, Rgb colour = kInk,
                          std::uint8_t attrs = kAttrNone) {
        panel_text(fb, box, 2, row, text, colour, kPanel, attrs);
        ++row;
    };

    line("KEYBOARD HELP", kGood, kAttrBold);
    line("");
    line("q          quit");
    line("?          close this help");
    line("Space      pause / resume (live target)");
    line("r          reset counters, peaks, drops, and frame stats");
    line("");
    line("h j k l    move cursor       H J K L    move x10");
    line("g / G      first / last cell");
    line("n / N      next / previous occupied cell");
    line("Tab        cycle chunks in the selected cell");
    line("");
    line("s          take snapshot     d          toggle diff");
    line("S          clear snapshot");
    if (paused_) line("Display paused; ring events are still being drained.", kWarn);
}

void HeapApp::draw(Framebuffer &fb, const LoopStats &stats) {
    const int w = fb.width();
    const int h = fb.height();

    fb.clear(Cell{U' ', kInk, kPanel, 0});

    char line[256];

    /* Row 0: what is being watched. */
    const char *comm = session_.comm();
    std::snprintf(line, sizeof line, " heapviz 0.1.0-dev   [pid %d%s%s]",
                  session_.pid(), comm[0] != '\0' ? " - " : "",
                  comm[0] != '\0' ? comm : "");
    fb.text(0, 0, line, kGood, kPanel, kAttrBold);

    std::snprintf(line, sizeof line, "%.0f fps ", stats.fps);
    const auto rlen = static_cast<int>(std::strlen(line));
    if (w > rlen) fb.text(w - rlen, 0, line, kDim, kPanel);

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
        if (w > rlen + blen) fb.text(w - rlen - blen, 0, line, kLeak, kPanel,
                                     kAttrBold);
    }

    /* Row 1: where the heap is, and which arenas it is coming from. */
    char arena[kArenaLabelMax];
    scanner_.arena_label(arena, sizeof arena);
    if (bounds_.empty()) {
        std::snprintf(line, sizeof line, " Heap: (not yet known)   Arena: %s",
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
                      " Heap: 0x%012llx - 0x%012llx (%s mapped)   Arena: %s   "
                      "1 cell = %s",
                      static_cast<unsigned long long>(lo),
                      static_cast<unsigned long long>(hi), span, arena, cell);
    }
    fb.text(0, 1, line, kInk, kPanel);

    if (regions_.count() > 1) {
        /* Named rather than silent: rows in the gutter restart at each region,
         * and a reader has to know that a seam is a seam and not a gap in the
         * heap. */
        std::snprintf(line, sizeof line, " %zu regions packed ",
                      regions_.count());
        const auto len = static_cast<int>(std::strlen(line));
        if (w > len) fb.text(w - len, 1, line, kWarn, kPanel);
    }

    /* Row 2: the model, and then whatever is wrong with it. */
    std::snprintf(line, sizeof line,
                  " Live: %llu chunks / %llu KiB   Events: %llu   On map: %llu",
                  static_cast<unsigned long long>(live_),
                  static_cast<unsigned long long>(live_bytes_ / 1024),
                  static_cast<unsigned long long>(events_),
                  static_cast<unsigned long long>(map_.total_live_chunks()));
    fb.text(0, 2, line, kInk, kPanel);

    const std::uint64_t dropped = session_.dropped() - dropped_at_attach_;

    /* Overhead, and how much of it is measured rather than inferred. The count
     * matters: the figure is exact only for the chunks the enrichment pass has
     * reached, and presenting a partly-corrected total as though it were the
     * whole truth is the kind of quiet lie this tool exists not to tell. */
    const char *why = reader_.hint();
    if (why != nullptr) {
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
        if (w > len && dropped == 0) fb.text(w - len, 2, line, kDim, kPanel);
    }

    /* Dropped events are reported the moment they appear rather than folded
     * into a health percentage. Ground rule: a profiler that quietly lies about
     * what it missed is worse than no profiler, and after one overflow the
     * chunk table has a phantom leak in it that nothing can resync (D5). */
    if (dropped != 0) {
        std::snprintf(line, sizeof line, " %llu events DROPPED - display is incomplete ",
                      static_cast<unsigned long long>(dropped));
        const auto len = static_cast<int>(std::strlen(line));
        if (w > len) fb.text(w - len, 2, line, kBad, kPanel, kAttrBold);
    }

    const Rect area = map_area(w, h);
    view_.draw(fb, area, map_, now_ms_);
    /* Before the cursor, so a candidate cell the cursor is sitting on still
     * shows where the cursor is. */
    view_.draw_leaks(fb, area, map_, snap_);
    view_.draw_cursor(fb, area, map_, cursor_);

    /* M5.2's panel, which is where the cursor's address now lives. It prints
     * the real address rather than the packed coordinate: the coordinate exists
     * nowhere in the target, so it is not a number anyone could look up in a
     * debugger. */
    const Rect panel = inspector_area(w, h);
    if (panel.h > 0)
        inspect_.draw(fb, panel, grid_, cursor_, &regions_, now_ms_);

    /* M5.3's panel, sharing the block. The two never overlap because both come
     * out of `metrics_cols`, which is the only thing that knows where the
     * boundary is. */
    const Rect stats_panel = metrics_area(w, h);
    if (stats_panel.w > 0) metrics_.draw(fb, stats_panel);

    /* The footer: the session's state, in the words the user needs. */
    /* Initialised rather than left to the switch below. It covers every
     * enumerator, but the compiler will not take an enum's declared range as a
     * promise about its value -- and at -O3 that becomes a -Werror. */
    const char *note = " q quit";
    Rgb colour = kDim;
    if (help_visible_) {
        if (paused_)
            note = " HELP   q quit   ? close   r reset   Space resume";
        else if (state_ == SessionState::Live)
            note = " HELP   q quit   ? close   r reset   Space pause";
        else if (state_ == SessionState::Exited)
            note = " HELP   q quit   ? close   r reset";
        else
            note = " HELP   q quit   ? close";
        colour = kGood;
    } else if (paused_) {
        std::snprintf(line, sizeof line,
                      " PAUSED   q quit   ? help   Space resume   r reset"
                      "   %zu events staged",
                      staged_events());
        note = line;
        colour = kWarn;
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
            colour = snap_.diff_mode() ? kLeak : kDim;
            break;
        case SessionState::Exited:
            note = " TARGET EXITED   q quit  ? help  r reset"
                   "  hjkl/HJKL move  g/G ends  n/N chunk";
            colour = kWarn;
            break;
        case SessionState::Detached:
            note = " NOT ATTACHED   q quit  ? help";
            colour = kDim;
            break;
        }
    }
    fb.text(0, h - 1, note, colour, kPanel);

    /* Last, so the modal wins over every model layer while the footer remains
     * visible and truthful about the keys that can close it. */
    if (help_visible_) draw_help(fb);
}

} // namespace hv
