/* heapviz - fragmentation analysis checks (M5.4).
 *
 * Copyright (C) 2026 Parth Sinha
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Five properties, and every one of them is a way to produce a number that
 * looks like a measurement and is not one.
 *
 * **A packed heap measures zero.** ptmalloc leaves no space between its chunks,
 * and the word an in-use chunk costs sits below the pointer the interceptor
 * reported. Model a chunk as `[ptr, ptr + usable)` and every allocation in the
 * heap acquires an eight-byte hole in front of it -- 25% on a heap of 32-byte
 * requests, which is the `[Med]` badge on a heap with nothing wrong with it.
 *
 * **Arenas are measured apart.** A threaded target's regions span terabytes
 * between them. One span across the lot reports 99.99% fragmented forever, and
 * one hole from the top of one arena to the bottom of the next answers "can I
 * allocate 1 MB" with a confident yes about address space that is not a heap.
 *
 * **The headline number survives a heap too large to sort.** The percentage
 * comes from sums and the largest hole comes from an ordered walk, so only the
 * second is capped -- and when it is dropped the panel is told it is unknown
 * rather than handed a zero, which would read as "no holes at all".
 *
 * **The ends are not fragmentation.** Free space above the topmost live chunk
 * is headroom the heap grows into, in one piece any request can use. A measure
 * that counted it would call a freshly reserved arena pathological.
 *
 * **It runs on its own clock.** The pass is linear in the live set and the live
 * set can be a million chunks; per frame that is the frame budget rather than a
 * fraction of it.
 */

#include "tui/fragmentation.h"

#include "tui/metrics.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

int g_failures = 0;

void check(bool ok, const char *what) {
    if (!ok) { std::fprintf(stderr, "  FAIL %s\n", what); ++g_failures; }
}

/* The two arenas the interesting cases need: one low, one far away. Sizes are
 * generous so a test can leave headroom above its chunks without the clamp in
 * `analyze` quietly trimming them. */
constexpr std::uint64_t kArenaA = 0x5b0000000000ull;
constexpr std::uint64_t kArenaB = 0x7f0000000000ull;
constexpr std::uint64_t kArenaBytes = 1u << 20;

hv::Region reg(std::uint64_t start, std::uint64_t end) {
    hv::Region r;
    r.start = start;
    r.end   = end;
    r.kind  = hv::RegionKind::Heap;
    r.perms = hv::kPermRead | hv::kPermWrite | hv::kPermPrivate;
    return r;
}

hv::RegionMap one_arena() {
    hv::RegionMap m;
    m.rebuild({reg(kArenaA, kArenaA + kArenaBytes)});
    return m;
}

hv::RegionMap two_arenas() {
    hv::RegionMap m;
    m.rebuild({reg(kArenaA, kArenaA + kArenaBytes),
               reg(kArenaB, kArenaB + kArenaBytes)});
    return m;
}

/* The word `analyze` adds back, spelled out here so the expectations below read
 * as arithmetic rather than as a restatement of the implementation. */
constexpr std::uint64_t kWord = 8;

void put(hv::ChunkTable &t, std::uint64_t addr, std::uint32_t usable) {
    /* `size` is the request and `usable` is what the allocator gave; only the
     * second is part of the footprint, so the first is deliberately unequal to
     * it in every case here. */
    t.insert_live(addr, usable > 16 ? usable - 16 : 1, usable, 100, 1);
}

/* One pass, from a clock position that is always due. */
const hv::FragReport &run(hv::FragAnalyzer &f, const hv::ChunkTable &t,
                          const hv::RegionMap &r) {
    f.analyze(t, r, 0);
    return f.report();
}

void test_nothing_to_measure() {
    hv::ChunkTable t;
    hv::FragAnalyzer f;

    const hv::RegionMap none;
    check(run(f, t, none).percent < 0, "empty: no regions is not zero percent");

    hv::FragAnalyzer f2;
    const hv::RegionMap one = one_arena();
    const hv::FragReport &r = run(f2, t, one);
    check(r.percent < 0, "empty: an empty table is not zero percent");
    check(r.chunks == 0, "empty: and it counted nothing");

    /* The distinction the panel depends on: `frag_badge` turns "not computed"
     * into the dash and any real figure into a badge. */
    check(hv::frag_badge(r.percent) == hv::FragBadge::Unknown,
          "empty: which the badge reports as unknown");
}

void test_a_packed_heap_measures_zero() {
    hv::ChunkTable t;
    /* Three 32-byte-usable chunks laid out exactly as ptmalloc would: each one
     * starts where the previous one's chunk ends, which is 40 bytes on. */
    put(t, kArenaA + 0,  32);
    put(t, kArenaA + 40, 32);
    put(t, kArenaA + 80, 32);

    hv::FragAnalyzer f;
    const hv::RegionMap m = one_arena();
    const hv::FragReport &r = run(f, t, m);

    check(r.chunks == 3, "packed: all three were measured");
    check(r.span_bytes == 120, "packed: spanning 120 bytes");
    check(r.used_bytes == 120, "packed: all of which is used");
    check(r.gap_bytes == 0, "packed: with nothing stranded");
    check(r.percent == 0, "packed: a densely packed heap is 0% fragmented");
    check(r.largest_gap_known && r.largest_gap == 0,
          "packed: and its largest hole is nothing");
}

void test_a_hole_is_measured_exactly() {
    hv::ChunkTable t;
    /* Two 1 KiB chunks 4 KiB apart. Footprints are 1016 + 8; the hole between
     * them is 4096 - 1024 = 3072, and the span is 4096 + 1024 = 5120. */
    put(t, kArenaA + 0,    1016);
    put(t, kArenaA + 4096, 1016);

    hv::FragAnalyzer f;
    const hv::RegionMap m = one_arena();
    const hv::FragReport &r = run(f, t, m);

    check(r.span_bytes == 5120, "hole: the span runs first start to last end");
    check(r.used_bytes == 2048, "hole: two chunks of 1024");
    check(r.gap_bytes == 3072, "hole: leaving 3072 stranded");
    check(r.percent == 60, "hole: which is 60% of the span");
    check(r.largest_gap == 3072, "hole: and the whole of it is one hole");

    /* The identity the pass is built on, restated as an assertion so a change
     * that broke it here would not have to be inferred from a percentage. */
    check(r.gap_bytes == r.span_bytes - r.used_bytes,
          "hole: gaps are span minus footprints, exactly");
}

void test_the_ends_are_headroom_not_fragmentation() {
    hv::ChunkTable t;
    /* One chunk near the bottom of a megabyte arena. Almost all of the region
     * is free, and none of it is fragmentation: it is where the heap grows. */
    put(t, kArenaA + 4096, 1016);

    hv::FragAnalyzer f;
    const hv::RegionMap m = one_arena();
    const hv::FragReport &r = run(f, t, m);

    check(r.span_bytes == 1024,
          "ends: the span starts at the first chunk, not the region");
    check(r.percent == 0,
          "ends: an arena with room above it is not a fragmented one");
}

void test_freed_chunks_are_free_space() {
    hv::ChunkTable t;
    put(t, kArenaA + 0,    1016);
    put(t, kArenaA + 4096, 1016);
    put(t, kArenaA + 8192, 1016);

    hv::FragAnalyzer f;
    const hv::RegionMap m = one_arena();
    check(run(f, t, m).chunks == 3, "freed: three to start with");

    /* The table keeps a freed record so the fade has something to fade. It is
     * not memory the program is holding, and counting it would report a heap
     * that had just released its middle third as being no better off. */
    t.mark_freed(kArenaA + 4096, 200);

    hv::FragAnalyzer f2;
    const hv::FragReport &r = run(f2, t, m);
    check(r.chunks == 2, "freed: a freed chunk is not a live one");
    check(r.span_bytes == 9216, "freed: the span still runs to the last live");
    check(r.used_bytes == 2048, "freed: holding two chunks");
    check(r.largest_gap == 8192 - 1024,
          "freed: and the hole it left is the largest one");
}

void test_chunks_outside_the_known_regions() {
    hv::ChunkTable t;
    put(t, kArenaA + 0, 1016);
    /* Between one region scan and the next the target allocates in memory
     * nothing has mapped yet. There is no span to measure it against. */
    put(t, kArenaB + 0, 1016);

    hv::FragAnalyzer f;
    const hv::RegionMap m = one_arena();
    const hv::FragReport &r = run(f, t, m);

    check(r.chunks == 1, "outside: only the one in a known region counts");
    check(r.outside == 1, "outside: and the other is counted as skipped");
    check(r.percent == 0, "outside: without dragging the span across a terabyte");
}

void test_arenas_are_measured_apart() {
    hv::ChunkTable t;
    /* Two chunks in each arena, with a 4 KiB hole in the first and an 8 KiB
     * hole in the second. The distance between the arenas is 36 TiB. */
    put(t, kArenaA + 0,     1016);
    put(t, kArenaA + 5120,  1016);
    put(t, kArenaB + 0,     1016);
    put(t, kArenaB + 9216,  1016);

    hv::FragAnalyzer f;
    const hv::RegionMap m = two_arenas();
    const hv::FragReport &r = run(f, t, m);

    check(r.regions == 2, "arenas: both held live chunks");
    check(r.span_bytes == (5120 + 1024) + (9216 + 1024),
          "arenas: the spans are summed, not unioned");
    check(r.used_bytes == 4096, "arenas: four chunks of 1024");
    check(r.gap_bytes == (5120 - 1024) + (9216 - 1024),
          "arenas: and so are the gaps");

    /* The one that matters: without the region cursor in the ordered walk, the
     * largest hole would be the 36 TiB between the two arenas. */
    check(r.largest_gap == 9216 - 1024,
          "arenas: the largest hole is inside an arena, not between two");
    check(r.largest_gap < (kArenaB - kArenaA),
          "arenas: nowhere near the distance separating them");
}

void test_a_chunk_running_past_a_stale_region_end() {
    hv::ChunkTable t;
    /* A heap that grew since the last scan: the chunk is real, and half of it
     * is above the mapping /proc last reported. */
    put(t, kArenaA + kArenaBytes - 1024, 4096);

    hv::FragAnalyzer f;
    const hv::RegionMap m = one_arena();
    const hv::FragReport &r = run(f, t, m);

    check(r.chunks == 1, "stale: the chunk is still counted");
    check(r.span_bytes == 1024,
          "stale: clamped to the region rather than running past it");
    check(r.gap_bytes == 0, "stale: and it does not manufacture a gap");
}

void test_the_percentage_survives_a_heap_too_large_to_sort() {
    hv::ChunkTable t;
    put(t, kArenaA + 0,    1016);
    put(t, kArenaA + 4096, 1016);
    put(t, kArenaA + 8192, 1016);
    put(t, kArenaA + 16384, 1016);

    hv::FragAnalyzer f;
    f.set_max_sorted(2); /* stand in for a million-chunk live set */
    const hv::RegionMap m = one_arena();
    const hv::FragReport &r = run(f, t, m);

    check(r.chunks == 4, "large: every chunk was still walked");
    check(r.percent >= 0, "large: and the percentage is still exact");
    check(r.span_bytes == 17408 && r.used_bytes == 4096,
          "large: from the same sums as any other heap");
    check(!r.largest_gap_known,
          "large: only the largest hole is dropped");
    check(r.largest_gap == 0,
          "large: which is reported as unknown rather than as no holes");

    /* And the same heap under a cap that admits it gets the figure back. */
    hv::FragAnalyzer f2;
    const hv::FragReport &r2 = run(f2, t, m);
    check(r2.largest_gap_known && r2.largest_gap == 16384 - 9216,
          "large: the same heap under a workable cap finds the hole");
}

void test_it_runs_on_its_own_clock() {
    hv::ChunkTable t;
    put(t, kArenaA + 0,    1016);
    put(t, kArenaA + 4096, 1016);
    const hv::RegionMap m = one_arena();

    hv::FragAnalyzer f;
    check(f.due(0), "clock: the first pass is always due");
    check(f.analyze(t, m, 0), "clock: and it produces a figure");
    check(f.percent() == 60, "clock: the one the layout implies");

    /* A frame later. The heap changed, and the panel keeps last quarter
     * second's answer -- which is a snapshot, not a stale display. */
    put(t, kArenaA + 8192, 1016);
    check(!f.due(16), "clock: a frame later it is not due");
    check(!f.analyze(t, m, 16), "clock: so the pass does not run");
    check(f.percent() == 60, "clock: and the figure is unchanged");

    check(f.due(hv::kFragIntervalMs), "clock: due again at the interval");
    check(f.analyze(t, m, hv::kFragIntervalMs), "clock: and the figure moves");
    check(f.percent() != 60, "clock: to the one the new layout implies");
}

void test_an_unchanged_heap_reports_no_change() {
    hv::ChunkTable t;
    put(t, kArenaA + 0,    1016);
    put(t, kArenaA + 4096, 1016);
    const hv::RegionMap m = one_arena();

    hv::FragAnalyzer f;
    f.set_interval(0); /* every call is due; the question is what it returns */

    check(f.analyze(t, m, 0), "idle: the first pass is a change");
    check(!f.analyze(t, m, 1), "idle: an identical heap is not");
    check(!f.analyze(t, m, 2), "idle: still not, on the third pass");

    /* Ground rule #4 is the application's, not this class's -- but this is what
     * feeds it. A pass that returned true every 250 ms would repaint an idle
     * session four times a second forever. */
    put(t, kArenaA + 32768, 1016);
    check(f.analyze(t, m, 3), "idle: a heap that moved is a change");
}

/* The panel's half: what the analyzer computes has to survive being drawn. */

std::string row_text(const hv::Framebuffer &fb, int y) {
    std::string s;
    for (int x = 0; x < fb.width(); ++x) {
        const char32_t g = fb.at_back(x, y).glyph;
        s.push_back(g < 128 ? static_cast<char>(g) : '?');
    }
    return s;
}

void test_the_panel_draws_what_the_pass_found() {
    hv::Framebuffer fb;
    fb.resize(60, 8);
    fb.clear(hv::Cell{U' ', 0x00FFFFFF, 0, 0});
    const hv::Rect area{0, 0, 60, hv::kMetricsRows};

    hv::Metrics m;

    /* Before any pass. */
    m.draw(fb, area);
    check(row_text(fb, 4).find("Fragmented") != std::string::npos,
          "panel: the row is there before the analysis runs");
    check(row_text(fb, 4).find("--") != std::string::npos,
          "panel: saying the figure is not known");
    check(row_text(fb, 4).find("max hole") == std::string::npos,
          "panel: with no hole size beside it");

    /* After one. */
    fb.clear(hv::Cell{U' ', 0x00FFFFFF, 0, 0});
    m.set_fragmentation(22);
    m.set_largest_gap(1048576, true);
    m.draw(fb, area);
    const std::string after = row_text(fb, 4);
    check(after.find("22%") != std::string::npos,
          "panel: the percentage the pass produced");
    check(after.find("[Med]") != std::string::npos,
          "panel: with the badge its thresholds imply");
    check(after.find("max hole 1 MB") != std::string::npos,
          "panel: and the largest hole, right-aligned beside it");

    /* And on a heap too large to sort: the percentage stays, the hole goes. */
    fb.clear(hv::Cell{U' ', 0x00FFFFFF, 0, 0});
    m.set_largest_gap(0, false);
    m.draw(fb, area);
    const std::string capped = row_text(fb, 4);
    check(capped.find("22%") != std::string::npos,
          "panel: a heap too large to sort keeps its percentage");
    check(capped.find("max hole") == std::string::npos,
          "panel: and is not told there are no holes in it");
}

void test_the_hole_gives_way_before_the_percentage() {
    hv::Framebuffer fb;
    fb.resize(hv::kMetricsMinCols, 8);
    fb.clear(hv::Cell{U' ', 0x00FFFFFF, 0, 0});

    hv::Metrics m;
    m.set_fragmentation(63);
    m.set_largest_gap(1048576, true);
    m.draw(fb, hv::Rect{0, 0, hv::kMetricsMinCols, hv::kMetricsRows});

    const std::string row = row_text(fb, 4);
    check(row.find("63%") != std::string::npos,
          "narrow: the percentage survives the narrowest panel");
    check(row.find("[High]") != std::string::npos,
          "narrow: and so does its badge");
    check(row.find("max hole") == std::string::npos,
          "narrow: the hole size is what gives way, not the warning");
}

} // namespace

int main() {
    test_nothing_to_measure();
    test_a_packed_heap_measures_zero();
    test_a_hole_is_measured_exactly();
    test_the_ends_are_headroom_not_fragmentation();
    test_freed_chunks_are_free_space();
    test_chunks_outside_the_known_regions();
    test_arenas_are_measured_apart();
    test_a_chunk_running_past_a_stale_region_end();
    test_the_percentage_survives_a_heap_too_large_to_sort();
    test_it_runs_on_its_own_clock();
    test_an_unchanged_heap_reports_no_change();
    test_the_panel_draws_what_the_pass_found();
    test_the_hole_gives_way_before_the_percentage();

    if (g_failures != 0) {
        std::fprintf(stderr, "fragmentation: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("fragmentation: all checks passed\n");
    return 0;
}
