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

/* Three header rows and one status row, both fixed rather than sized to their
 * contents. A map whose top edge moved when a banner appeared would re-bucket
 * the whole address space at the moment the user most wants to read it. */
constexpr int kHeaderRows = 3;
constexpr int kFooterRows = 1;

std::uint32_t clamp_u32(std::uint64_t v) noexcept {
    return v > UINT32_MAX ? UINT32_MAX : static_cast<std::uint32_t>(v);
}

} // namespace

HeapApp::HeapApp(RingSession &session, Capabilities caps)
    : session_(session), caps_(caps), view_(caps),
      scanner_(session.pid()), reader_(session.pid()) {
    batch_.resize(kMaxEventsPerFrame); /* once; draining allocates nothing */

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

    const std::uint32_t n = session_.drain(batch_.data(), kMaxEventsPerFrame);
    if (n != 0) apply(batch_.data(), n);
    events_ += n;
    return n;
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

        map_.on_alloc(e.ptr, size, e.usable, ms);
        table_.insert_live(e.ptr, size, e.usable, ms, hv_event_get_tid(&e));
    }
}

bool HeapApp::update(std::uint64_t now_ns) {
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
        if (!table_.mark_refined(enrich_ptrs_[i], info.overhead)) continue;
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
    geometry_dirty_ = false;
}

bool HeapApp::key(char byte) {
    if (byte == 'q') { request_quit(); return false; }
    return false;
}

bool HeapApp::animating() const {
    return MapView::animating(map_, now_ms_);
}

void HeapApp::resized(int w, int h) {
    width_  = w;
    height_ = h;
    geometry_dirty_ = true;
    if (!bounds_.empty()) refit(w, h);
}

Rect HeapApp::map_area(int w, int h) const noexcept {
    const int top = kHeaderRows;
    return Rect{0, top, w, h - top - kFooterRows};
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

    view_.draw(fb, map_area(w, h), map_, now_ms_);

    /* The footer: the session's state, in the words the user needs. */
    /* Initialised rather than left to the switch below. It covers every
     * enumerator, but the compiler will not take an enum's declared range as a
     * promise about its value -- and at -O3 that becomes a -Werror. */
    const char *note = " q quit";
    Rgb colour = kDim;
    switch (state_) {
    case SessionState::Live:
        note = " q quit    watching";
        colour = kDim;
        break;
    case SessionState::Exited:
        note = " q quit    TARGET EXITED - showing its last state";
        colour = kWarn;
        break;
    case SessionState::Detached:
        note = " q quit    not attached";
        colour = kDim;
        break;
    }
    fb.text(0, h - 1, note, colour, kPanel);
}

} // namespace hv
