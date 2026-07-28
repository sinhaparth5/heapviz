/* heapviz - per-cell aggregation checks (M3.3).
 *
 * Copyright (C) 2026 Parth Sinha
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The central check is differential. Aggregates are folded forward event by
 * event because rebuilding per frame would cost the whole frame budget, but
 * that makes the incremental path the one carrying the truth, and an
 * accumulator that drifts is invisible: every number stays plausible, no
 * assertion trips, and the display is simply wrong. Replaying a long stream
 * incrementally and requiring the result to equal a rebuild from the chunk
 * table is what makes the drift observable, because the two arrive at the same
 * numbers by entirely different routes.
 *
 * The precedence table is checked through `cell_state` directly rather than
 * through the map, because one of its five levels cannot be reached by any
 * sequence of events yet -- see the note on `Overhead` in heatmap.h.
 */

#include "tui/heatmap.h"

#include <cstdio>
#include <vector>

namespace {

int g_failures = 0;

void check(bool ok, const char *what) {
    if (!ok) { std::fprintf(stderr, "  FAIL %s\n", what); ++g_failures; }
}

std::uint64_t next_rand(std::uint64_t &s) {
    s += 0x9E3779B97F4A7C15ull;
    std::uint64_t z = s;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

constexpr std::uint64_t kBase = 0x55A0000000ull;
constexpr std::uint64_t kSpan = 4 * 1024 * 1024;

hv::Grid make_grid(int cols = 80, int rows = 24) {
    hv::Grid g;
    g.configure(kBase, kBase + kSpan, cols, rows);
    return g;
}

/* --- the precedence table ------------------------------------------------- */

void test_precedence() {
    const hv::HeatTimings t{}; /* 200 ms pulse, 300 ms flash */

    hv::CellAggregate empty;
    check(hv::cell_state(empty, t, 1000) == hv::CellState::Empty,
          "precedence: nothing recorded is empty");

    hv::CellAggregate overhead;
    overhead.overhead_bytes = 16;
    check(hv::cell_state(overhead, t, 1000) == hv::CellState::Overhead,
          "precedence: header bytes with no payload read as overhead");

    hv::CellAggregate live;
    live.n_live = 3;
    live.live_bytes = 300;
    live.overhead_bytes = 48;
    live.last_alloc_ms = 100;
    check(hv::cell_state(live, t, 1000) == hv::CellState::Live,
          "precedence: a settled allocation outranks its own overhead");

    hv::CellAggregate fresh = live;
    fresh.last_alloc_ms = 950;
    check(hv::cell_state(fresh, t, 1000) == hv::CellState::RecentMalloc,
          "precedence: a fresh malloc outranks live");

    hv::CellAggregate freed = fresh;
    freed.last_free_ms = 900;
    check(hv::cell_state(freed, t, 1000) == hv::CellState::RecentFree,
          "precedence: a recent free outranks everything, including a newer "
          "malloc");

    /* The roadmap fixes that last one deliberately, so pin it explicitly: the
     * malloc here is 50 ms newer than the free and still loses. */
    check(freed.last_alloc_ms > freed.last_free_ms,
          "precedence: (the malloc really was the more recent event)");
}

/* State changes because the clock moved, not because anything updated the
 * cell. This is what lets M3.4 age colours with no timers and no sweep. */
void test_state_is_a_function_of_time_alone() {
    const hv::HeatTimings t{};

    hv::CellAggregate a;
    a.n_live = 1;
    a.live_bytes = 64;
    a.last_alloc_ms = 1000;

    check(hv::cell_state(a, t, 1000) == hv::CellState::RecentMalloc,
          "aging: at the instant of the malloc");
    check(hv::cell_state(a, t, 1199) == hv::CellState::RecentMalloc,
          "aging: one millisecond inside the pulse window");
    check(hv::cell_state(a, t, 1200) == hv::CellState::Live,
          "aging: the window is half-open, so 200 ms later it has settled");
    check(hv::cell_state(a, t, 100000) == hv::CellState::Live,
          "aging: and stays settled, with nothing having touched it");

    a.last_free_ms = 2000;
    a.n_live = 0;
    a.live_bytes = 0;
    check(hv::cell_state(a, t, 2299) == hv::CellState::RecentFree,
          "aging: inside the free flash");
    check(hv::cell_state(a, t, 2300) == hv::CellState::Empty,
          "aging: and empty once it has passed");

    /* A timestamp in the future must not read as recent. Nothing should
     * produce one, and no explicit guard exists: the unsigned subtraction wraps
     * to ~2^32, which fails the window comparison on its own. Pinned here so
     * that stays true if the arithmetic is ever reworked. */
    check(hv::cell_state(a, t, 1999) == hv::CellState::Empty,
          "aging: a stamp from the future does not flash");
}

void test_timings_are_configurable() {
    hv::HeatMap m;
    m.configure(make_grid());
    m.on_alloc(kBase + 4096, 32, 48, 1000);

    std::size_t idx = 0;
    m.grid().index_of(kBase + 4096, idx);

    check(m.state_at(idx, 1150) == hv::CellState::RecentMalloc,
          "timings: default pulse is 200 ms");

    hv::HeatTimings fast;
    fast.malloc_pulse_ms = 100;
    m.set_timings(fast);
    check(m.state_at(idx, 1150) == hv::CellState::Live,
          "timings: shortening the pulse takes effect with no rebuild");
}

/* --- the differential ----------------------------------------------------- */

bool same_cell(const hv::CellAggregate &a, const hv::CellAggregate &b) {
    return a.live_bytes == b.live_bytes && a.n_live == b.n_live &&
           a.overhead_bytes == b.overhead_bytes &&
           a.last_alloc_ms == b.last_alloc_ms &&
           a.last_free_ms == b.last_free_ms;
}

void test_incremental_matches_a_rebuild() {
    const hv::Grid g = make_grid();

    hv::HeatMap    incremental;
    hv::ChunkTable table;
    incremental.configure(g);

    /* Each address is used at most once. Recycling is covered separately below,
     * because there the two paths genuinely disagree and the reason matters. */
    std::uint64_t rng = 99;
    std::vector<std::uint64_t> live;

    for (int step = 0; step < 40000; ++step) {
        const auto now = static_cast<std::uint32_t>(step / 10);

        if (live.empty() || (next_rand(rng) % 100) < 60) {
            const std::uint64_t ptr =
                kBase + (next_rand(rng) % (kSpan / 16)) * 16;
            if (table.find(ptr) != nullptr) continue; /* keep addresses unique */

            const auto size = static_cast<std::uint32_t>(next_rand(rng) % 2048 + 1);
            const std::uint32_t usable = size + 8;

            table.insert_live(ptr, size, usable, now, 1);
            incremental.on_alloc(ptr, size, usable, now);
            live.push_back(ptr);
        } else {
            const std::size_t which = next_rand(rng) % live.size();
            const std::uint64_t ptr = live[which];
            live[which] = live.back();
            live.pop_back();

            const hv::Chunk *c = table.find(ptr);
            if (c == nullptr) continue;
            const std::uint32_t size = c->size, usable = c->usable;

            table.mark_freed(ptr, now);
            incremental.on_free(ptr, size, usable, now);
        }
    }

    hv::HeatMap rebuilt;
    rebuilt.configure(g);
    rebuilt.rebuild(table);

    check(incremental.cell_count() == rebuilt.cell_count(),
          "differential: both maps have the same shape");

    std::size_t differing = 0;
    std::size_t non_empty = 0;
    for (std::size_t i = 0; i < incremental.cell_count(); ++i) {
        const hv::CellAggregate &a = incremental.at(i);
        const hv::CellAggregate &b = rebuilt.at(i);
        if (a.n_live != 0 || a.last_alloc_ms != hv::kNoTime) ++non_empty;
        if (!same_cell(a, b)) ++differing;
    }

    check(non_empty > 100, "differential: the workload actually filled cells");
    check(differing == 0,
          "differential: 40000 folded events equal a rebuild, cell for cell");
    check(incremental.total_live_bytes() == rebuilt.total_live_bytes(),
          "differential: and the running totals agree");
    check(incremental.total_live_chunks() == rebuilt.total_live_chunks(),
          "differential: including the live chunk count");

    check(incremental.rebuilds() == 0,
          "differential: 40000 events cost no rebuild at all");
}

/* Where the two paths legitimately disagree, stated as a property rather than
 * left as a surprise. The chunk table holds one record per address, so an
 * address that was freed and then handed back by the allocator no longer
 * records that a free ever happened there. The incremental path saw it and
 * keeps the flash; a rebuild cannot know. */
void test_recycling_diverges_and_that_is_the_design() {
    const hv::Grid g = make_grid();
    const std::uint64_t ptr = kBase + 8192;

    hv::HeatMap    incremental;
    hv::ChunkTable table;
    incremental.configure(g);

    table.insert_live(ptr, 32, 48, 10, 1);
    incremental.on_alloc(ptr, 32, 48, 10);
    table.mark_freed(ptr, 20);
    incremental.on_free(ptr, 32, 48, 20);
    table.insert_live(ptr, 64, 80, 30, 1); /* allocator hands it straight back */
    incremental.on_alloc(ptr, 64, 80, 30);

    hv::HeatMap rebuilt;
    rebuilt.configure(g);
    rebuilt.rebuild(table);

    std::size_t idx = 0;
    check(g.index_of(ptr, idx), "recycle: the address is on the grid");

    check(incremental.at(idx).last_free_ms == 20,
          "recycle: the incremental path remembers the free that happened");
    check(rebuilt.at(idx).last_free_ms == hv::kNoTime,
          "recycle: a rebuild cannot, because the record was overwritten");

    /* Everything else still agrees, so the divergence is confined to the flash
     * and does not corrupt the live accounting. */
    check(incremental.at(idx).n_live == rebuilt.at(idx).n_live &&
              incremental.at(idx).live_bytes == rebuilt.at(idx).live_bytes,
          "recycle: the live accounting is identical either way");
}

/* --- geometry ------------------------------------------------------------- */

void test_rebuild_only_on_granularity_change() {
    hv::HeatMap m;
    const hv::Grid g = make_grid();

    check(m.configure(g), "geometry: the first configure needs a rebuild");
    const std::uint64_t after_first = m.rebuilds();

    /* Handing it the same geometry repeatedly is what the loop does after every
     * SIGWINCH, most of which change nothing. It must be free. */
    for (int frame = 0; frame < 1000; ++frame) {
        check(!m.configure(g) || frame < 0,
              "geometry: an unchanged grid needs no rebuild");
    }
    check(m.rebuilds() == after_first,
          "geometry: a thousand frames triggered no rebuild");

    /* A resize that changes the granularity does. */
    hv::Grid wider = make_grid(200, 50);
    check(wider.cell_bytes() != g.cell_bytes(),
          "geometry: (the resize really did change the granularity)");
    check(m.configure(wider), "geometry: a new granularity needs a rebuild");
}

void test_geometry_change_discards_stale_aggregates() {
    hv::HeatMap m;
    m.configure(make_grid());
    m.on_alloc(kBase + 4096, 32, 48, 10);
    check(m.total_live_chunks() == 1, "geometry: the allocation was recorded");

    /* Every address maps somewhere new, so the old aggregates are meaningless
     * and must not survive into the new grid. */
    m.configure(make_grid(200, 50));
    check(m.total_live_chunks() == 0 && m.total_live_bytes() == 0,
          "geometry: a granularity change clears what it cannot remap");
}

/* --- events that do not belong to this grid ------------------------------- */

void test_out_of_range_events_are_ignored() {
    hv::HeatMap m;
    m.configure(make_grid());

    m.on_alloc(kBase - 4096, 32, 48, 10);       /* below the base */
    m.on_alloc(kBase + kSpan * 4, 32, 48, 10);  /* past the grid  */
    m.on_alloc(0, 32, 48, 10);                  /* a null pointer */
    check(m.total_live_chunks() == 0,
          "range: allocations outside the grid are ignored, not clamped");

    m.on_free(kBase - 4096, 32, 48, 20);
    check(m.total_live_bytes() == 0, "range: and so are the matching frees");
}

/* heapviz attaches to a process that already has a heap, so frees of chunks it
 * never saw are routine. An unguarded decrement would wrap the counters to
 * enormous values and then draw them. */
void test_unmatched_frees_do_not_wrap() {
    hv::HeatMap m;
    m.configure(make_grid());
    const std::uint64_t ptr = kBase + 4096;

    m.on_free(ptr, 32, 48, 10);
    m.on_free(ptr, 32, 48, 11);

    std::size_t idx = 0;
    m.grid().index_of(ptr, idx);
    check(m.at(idx).n_live == 0, "unmatched: the live count stays at zero");
    check(m.at(idx).live_bytes == 0, "unmatched: the byte count does not wrap");
    check(m.at(idx).overhead_bytes == 0, "unmatched: nor does the overhead");
    check(m.total_live_bytes() == 0, "unmatched: nor do the running totals");

    /* The flash is still recorded: the free did happen in that cell, whether or
     * not heapviz knew what was there. */
    check(m.state_at(idx, 20) == hv::CellState::RecentFree,
          "unmatched: the cell still flashes, because the free was real");
}

void test_aggregates_sum_over_many_chunks_in_one_cell() {
    hv::HeatMap m;
    hv::Grid g = make_grid();
    m.configure(g);

    /* Pack several allocations into a single cell. */
    const std::uint64_t cell_start = kBase + g.cell_bytes() * 3;
    for (int i = 0; i < 4; ++i) {
        m.on_alloc(cell_start + static_cast<std::uint64_t>(i) * 16, 8, 16,
                   static_cast<std::uint32_t>(i));
    }

    std::size_t idx = 0;
    check(g.index_of(cell_start, idx), "sum: the cell is on the grid");
    check(m.at(idx).n_live == 4, "sum: all four chunks counted in one cell");
    check(m.at(idx).live_bytes == 64, "sum: usable bytes are summed");
    check(m.at(idx).overhead_bytes == 32, "sum: so is the per-chunk overhead");
    check(m.at(idx).last_alloc_ms == 3, "sum: the newest stamp wins");
}

} // namespace

int main() {
    test_precedence();
    test_state_is_a_function_of_time_alone();
    test_timings_are_configurable();
    test_incremental_matches_a_rebuild();
    test_recycling_diverges_and_that_is_the_design();
    test_rebuild_only_on_granularity_change();
    test_geometry_change_discards_stale_aggregates();
    test_out_of_range_events_are_ignored();
    test_unmatched_frees_do_not_wrap();
    test_aggregates_sum_over_many_chunks_in_one_cell();

    if (g_failures != 0) {
        std::fprintf(stderr, "heatmap_test: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("heatmap_test: precedence, time-only aging, and 40000 folded "
                "events matching a rebuild all hold\n");
    return 0;
}
