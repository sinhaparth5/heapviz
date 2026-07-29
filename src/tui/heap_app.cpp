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
      scanner_(session.pid()) {
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

        /* A rolling window of where allocations are landing, which is what
         * decides the displayed region. One store per event, no branch. */
        recent_[recent_at_] = e.ptr;
        recent_at_ = (recent_at_ + 1) & (kRecentAddrs - 1);

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
        const AddrRange b = choose_view();
        if (!b.empty() && (b.base != bounds_.base || b.end != bounds_.end)) {
            bounds_ = b;
            geometry_dirty_ = true;
            changed = true;
        }
    }

    if (geometry_dirty_ && !bounds_.empty() && width_ > 0) {
        refit(width_, height_);
        changed = true;
    }

    return changed;
}

/* Which region the map is pointed at.
 *
 * Not the union of them, which is what the header bar reports and what the
 * first version of this used. A brk heap at 0x5b... and a thread arena at
 * 0x70... span 23 TiB between them and are perhaps 40 MiB of actual memory: the
 * grid clamps to 1 GiB cells, `covers_whole_span` goes false, and every
 * allocation past the first gigabyte falls off a map that is drawing one cell
 * of heap and several thousand cells of hole. `Grid`'s own comment names this
 * as the case M2 has to fix.
 *
 * So, per D3, v0.1 displays the main arena and says how many regions it is not
 * showing. Compacting every allocatable region into one contiguous view is the
 * right long-term answer and is ROADMAP M2.4; it is a change to what an address
 * on screen means, which is more than the attach lifecycle should be deciding.
 *
 * The fallback for a target with no [heap] -- one allocating purely from thread
 * arenas, or from something that is not glibc -- is the largest allocatable
 * region, on the grounds that it is where most of the memory is. */
AddrRange HeapApp::choose_view() noexcept {
    const Region *best = nullptr;
    unsigned best_score = 0;
    unsigned current_score = 0;
    int count = 0;

    for (const Region &r : scanner_.regions()) {
        if (!r.allocatable() || r.size() == 0) continue;
        ++count;

        /* Score each region by how much of the recent traffic landed in it.
         * The obvious rule -- show `[heap]` -- is wrong for most real targets:
         * glibc gives every thread that allocates its own arena, so a program
         * with a single worker thread has a main arena holding almost nothing
         * and a thread arena holding everything. Preferring the named region
         * would reliably display the empty one.
         *
         * A fixed window of recent addresses rather than a walk of the chunk
         * table, because this runs on the scan timer and the table can hold a
         * million records: sixty-four addresses against thirty regions is a
         * rounding error, and "where are the allocations going right now" does
         * not need a census to answer. */
        unsigned score = 0;
        for (std::size_t i = 0; i < kRecentAddrs; ++i)
            if (recent_[i] != 0 && r.contains(recent_[i])) ++score;

        if (r.start == bounds_.base && r.end == bounds_.end) current_score = score;
        if (best == nullptr || score > best_score ||
            (score == best_score && r.size() > best->size())) {
            best = &r;
            best_score = score;
        }
    }

    /* Exactly one region is ever drawn, so everything else is hidden. Counted
     * and reported, so the map never implies it is all of the heap. */
    hidden_regions_ = count > 0 ? count - 1 : 0;

    if (best == nullptr) return AddrRange{};

    /* Hysteresis. Two arenas taking turns would otherwise re-bucket the whole
     * address space twice a second, and a map that re-scales while being read
     * is worse than one showing the second-busiest region. */
    if (!bounds_.empty() && current_score * 2 >= best_score) return bounds_;

    return AddrRange{best->start, best->end};
}

void HeapApp::refit(int w, int h) {
    grid_.set_bounds(bounds_.base, bounds_.end);
    if (!fit_grid(grid_, map_area(w, h))) return;

    /* A granularity change invalidates every aggregate, and rebuilding from the
     * chunk table is the only correct response. `configure` says whether that
     * actually happened, so a resize that changed nothing costs nothing. */
    if (map_.configure(grid_)) map_.rebuild(table_);
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
        format_byte_size(span, sizeof span, bounds_.span());
        /* The range shown is the range drawn, and the regions left out are
         * named as left out. A header reporting the union while the map draws
         * one region of it would be a number that is true of nothing on
         * screen. */
        std::snprintf(line, sizeof line,
                      " Heap: 0x%012llx - 0x%012llx (%s)   Arena: %s   "
                      "1 cell = %s",
                      static_cast<unsigned long long>(bounds_.base),
                      static_cast<unsigned long long>(bounds_.end), span, arena,
                      cell);
    }
    fb.text(0, 1, line, kInk, kPanel);

    if (hidden_regions_ > 0) {
        std::snprintf(line, sizeof line, " +%d region%s not shown ",
                      hidden_regions_, hidden_regions_ == 1 ? "" : "s");
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

    /* Dropped events are reported the moment they appear rather than folded
     * into a health percentage. Ground rule: a profiler that quietly lies about
     * what it missed is worse than no profiler, and after one overflow the
     * chunk table has a phantom leak in it that nothing can resync (D5). */
    const std::uint64_t dropped = session_.dropped() - dropped_at_attach_;
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
