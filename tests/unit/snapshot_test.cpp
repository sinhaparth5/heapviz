/* heapviz - snapshot and leak-candidate checks (M5.5).
 *
 * Copyright (C) 2026 Parth Sinha
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Six properties, and the first two are the whole reason this is a timestamp
 * rather than a set of pointers.
 *
 * **A recycled address is a candidate.** An allocation that was live at the
 * mark, was freed, and whose address was handed back out afterwards is memory
 * the program is holding that it was not holding before. A snapshot that
 * recorded the live pointers and tested membership excludes exactly that case --
 * and the addresses an allocator reuses are not a random sample, they are the
 * ones it reuses most. `insert_live` overwrites `alloc_ms`, so the comparison
 * gets it right without knowing the case exists.
 *
 * **Nothing before the mark counts**, however long it has been live. That is the
 * difference between this and the live-set figure the header already shows.
 *
 * **The overlay is indexed by cell, and the totals are not.** A candidate the
 * grid does not cover is counted in the summary and reported as off-map rather
 * than being dropped from one and not the other, because a banner reading 900
 * over a map highlighting 850 has to be explained by a number.
 *
 * **A pass runs on its own clock**, so the walk over the chunk table is a
 * quarter-second cost rather than a frame cost -- except when the geometry moved
 * under it, where the alternative is an overlay whose indices name cells that
 * have stopped meaning what they meant.
 *
 * **Off is free and inert.** `d` before `s` does nothing at all, and with diff
 * mode off `analyze` does not touch the table.
 */

#include "tui/snapshot.h"

#include "tui/map_view.h"

#include <cstdio>
#include <cstring>

namespace {

int g_failures = 0;

void check(bool ok, const char *what) {
    if (!ok) { std::fprintf(stderr, "  FAIL %s\n", what); ++g_failures; }
}

constexpr std::uint64_t kBase = 0x600000000000ull;
constexpr int           kCols = 20;
constexpr int           kRows = 10;
constexpr std::uint64_t kCell = 4096;

/* No RegionMap, so the coordinate is the address and a test can name a cell by
 * arithmetic -- `region_map_test` is what checks the packing, and translating
 * here would test that conversion twice and this class once. */
struct Fixture {
    hv::Grid         grid;
    hv::HeatMap      map;
    hv::ChunkTable   table;
    hv::SnapshotDiff diff;

    Fixture() {
        grid.configure(kBase, kBase + kCell * (kCols * kRows), kCols, kRows);
        map.configure(grid);
        /* Every pass in these tests is meant to run, so the interval is taken
         * out of the picture; `test_pass_runs_on_its_own_clock` puts it back. */
        diff.set_interval(0);
    }

    /* Both halves of the model, exactly as `HeapApp::apply` does it. */
    void alloc(std::uint64_t addr, std::uint32_t usable, std::uint32_t ms) {
        map.on_alloc(addr, usable, usable, ms);
        table.insert_live(addr, usable, usable, ms, 1234);
    }

    void free(std::uint64_t addr, std::uint32_t usable, std::uint32_t ms) {
        map.on_free(addr, usable, usable, ms);
        table.mark_freed(addr, ms);
    }

    std::uint64_t cell_addr(std::size_t cell) const {
        return kBase + kCell * cell;
    }

    /* One pass, unconditionally. */
    bool run(std::uint32_t now_ms) {
        return diff.analyze(table, map, now_ms, true);
    }
};

/* --- what counts as a candidate ------------------------------------------- */

void test_only_allocations_after_the_mark_count() {
    Fixture f;
    f.alloc(f.cell_addr(1), 64, 10);
    f.alloc(f.cell_addr(2), 64, 20);

    f.diff.take(100);
    f.diff.toggle();

    f.alloc(f.cell_addr(3), 128, 110);
    f.run(200);

    check(f.diff.report().chunks == 1, "candidates: only the post-mark chunk");
    check(f.diff.report().bytes == 128, "candidates: bytes are the post-mark ones");

    /* Age is not the question. Both pre-mark chunks are still live and have been
     * for longer than the candidate, which is exactly what the header's live-set
     * figure already reports and what this must not repeat. */
    check(f.table.size() == 3, "candidates: the pre-mark chunks are still live");
}

void test_a_chunk_allocated_on_the_mark_counts() {
    Fixture f;
    f.diff.take(100);
    f.diff.toggle();

    /* The allocation that raced the keypress belongs to the "since" side: `s` is
     * pressed to measure what happens next. */
    f.alloc(f.cell_addr(1), 64, 100);
    f.run(200);
    check(f.diff.report().chunks == 1, "candidates: the boundary millisecond is inside");
}

void test_a_recycled_address_is_a_candidate() {
    Fixture f;
    const std::uint64_t addr = f.cell_addr(4);

    /* Live at the mark, so a pointer-set snapshot records it. */
    f.alloc(addr, 64, 10);
    f.diff.take(100);
    f.diff.toggle();

    /* Freed and handed straight back out. The program is now holding an
     * allocation it was not holding at the mark, at an address that was in the
     * set -- which is the case a membership test gets backwards. */
    f.free(addr, 64, 110);
    f.alloc(addr, 64, 120);
    f.run(200);

    check(f.diff.report().chunks == 1, "recycled: the reallocation is a candidate");
    check(f.diff.report().bytes == 64, "recycled: its bytes are counted");
}

void test_a_freed_candidate_stops_counting() {
    Fixture f;
    f.diff.take(100);
    f.diff.toggle();

    f.alloc(f.cell_addr(5), 64, 110);
    f.run(200);
    check(f.diff.report().chunks == 1, "freed: allocated after the mark, so counted");

    /* Freed records are kept for the fade (M3.4), so this is a real chance to
     * count something that is not live. A leak candidate is memory still held. */
    f.free(f.cell_addr(5), 64, 210);
    f.run(300);
    check(f.diff.report().chunks == 0, "freed: no longer held, no longer a candidate");
    check(f.diff.hot_cells().empty(), "freed: and no longer highlighted");
}

/* --- the overlay ----------------------------------------------------------- */

void test_candidates_land_in_their_own_cells() {
    Fixture f;
    f.diff.take(100);
    f.diff.toggle();

    f.alloc(f.cell_addr(3), 64, 110);
    f.alloc(f.cell_addr(3) + 128, 64, 120); /* same cell */
    f.alloc(f.cell_addr(11), 64, 130);
    f.run(200);

    check(f.diff.report().chunks == 3, "overlay: every candidate is in the total");
    check(f.diff.cell_count(3) == 2, "overlay: two candidates share cell 3");
    check(f.diff.cell_count(11) == 1, "overlay: one in cell 11");
    check(f.diff.cell_count(0) == 0, "overlay: nothing anywhere else");
    check(f.diff.hot_cells().size() == 2,
          "overlay: a cell is listed once however many candidates it holds");
}

void test_a_candidate_off_the_grid_is_counted_and_said_so() {
    Fixture f;
    f.diff.take(100);
    f.diff.toggle();

    f.alloc(f.cell_addr(1), 64, 110);
    /* Below the grid's base: the target allocates outside the displayed region
     * constantly, and after a repack there is always a moment where it does. */
    f.alloc(kBase - 0x100000, 256, 120);
    f.run(200);

    check(f.diff.report().chunks == 2, "off map: still in the summary");
    check(f.diff.report().bytes == 320, "off map: its bytes are still counted");
    check(f.diff.report().offmap == 1, "off map: and reported as unshowable");
    check(f.diff.hot_cells().size() == 1, "off map: but not highlighted");
}

void test_a_geometry_change_reindexes_the_overlay() {
    Fixture f;
    f.diff.take(100);
    f.diff.toggle();
    f.alloc(f.cell_addr(7), 64, 110);
    f.run(200);
    check(f.diff.cell_span() == static_cast<std::size_t>(kCols * kRows),
          "reindex: the overlay covers the grid it was computed against");

    /* Half the columns: every cell now covers twice the address space, so every
     * index means something else. */
    f.grid.set_viewport(kCols / 2, kRows);
    f.map.configure(f.grid);
    f.map.rebuild(f.table);
    f.run(300);

    check(f.diff.cell_span() == f.map.cell_count(),
          "reindex: and is resized when that grid changes");
    std::size_t want = 0;
    check(f.grid.index_of(f.cell_addr(7), want), "reindex: the address still maps");
    check(f.diff.cell_count(want) == 1, "reindex: the candidate moved with it");
}

/* --- the modes ------------------------------------------------------------- */

void test_diff_before_a_snapshot_does_nothing() {
    Fixture f;
    f.alloc(f.cell_addr(1), 64, 10);

    check(!f.diff.toggle(), "no mark: `d` does not enter diff mode");
    check(!f.diff.diff_mode(), "no mark: and the mode stays off");
    check(!f.diff.has_snapshot(), "no mark: `d` does not silently take one");
    check(!f.run(100), "no mark: and no pass runs");
    check(f.diff.report().chunks == 0, "no mark: nothing is reported");
}

void test_off_costs_nothing() {
    Fixture f;
    f.diff.take(100);
    f.alloc(f.cell_addr(1), 64, 110);

    /* A mark with the mode off: `s` alone must not start painting the map. */
    check(!f.run(200), "off: a pass with diff mode off does not run");
    check(f.diff.report().chunks == 0, "off: and reports nothing");
    check(f.diff.hot_cells().empty(), "off: and highlights nothing");

    f.diff.toggle();
    f.run(300);
    check(f.diff.report().chunks == 1, "off: turning it on finds the candidate");

    f.diff.toggle();
    check(f.diff.report().chunks == 0, "off: turning it back off clears the summary");
    check(f.diff.hot_cells().empty(), "off: and the overlay with it");
}

void test_clearing_drops_the_mark_and_the_mode() {
    Fixture f;
    f.diff.take(100);
    f.diff.toggle();
    f.alloc(f.cell_addr(2), 64, 110);
    f.run(200);
    check(f.diff.diff_mode() && f.diff.report().chunks == 1, "clear: set up");

    f.diff.clear();
    check(!f.diff.has_snapshot(), "clear: the mark is gone");
    check(!f.diff.diff_mode(),
          "clear: and so is the mode -- an empty overlay reads as a clean heap");
    check(f.diff.report().chunks == 0, "clear: the summary goes with it");
    check(f.diff.hot_cells().empty(), "clear: and the highlights");
}

void test_a_second_mark_replaces_the_first() {
    Fixture f;
    f.diff.take(100);
    f.diff.toggle();
    f.alloc(f.cell_addr(1), 64, 110);
    f.run(200);
    check(f.diff.report().chunks == 1, "re-mark: set up");

    /* The chunk above is now on the far side of the mark, so it stops being a
     * candidate -- which is the whole point of pressing `s` again. */
    f.diff.take(300);
    check(f.diff.report().chunks == 0,
          "re-mark: the old mark's figures do not survive the new one");
    f.run(400);
    check(f.diff.report().chunks == 0, "re-mark: and the pass agrees");

    f.alloc(f.cell_addr(2), 32, 310);
    f.run(500);
    check(f.diff.report().chunks == 1, "re-mark: only what came after the new mark");
}

void test_the_mark_carries_a_wall_clock_label() {
    Fixture f;
    check(f.diff.taken_at()[0] == '\0', "label: nothing before a mark");
    f.diff.take(100);
    /* The session clock has no epoch a user could match against anything they
     * did, so the banner needs the other one. */
    check(std::strlen(f.diff.taken_at()) == 8, "label: HH:MM:SS");
    check(f.diff.taken_at()[2] == ':' && f.diff.taken_at()[5] == ':',
          "label: with its separators where a clock puts them");
    f.diff.clear();
    check(f.diff.taken_at()[0] == '\0', "label: and gone with the mark");
}

/* --- the clock -------------------------------------------------------------- */

void test_pass_runs_on_its_own_clock() {
    Fixture f;
    f.diff.set_interval(hv::kLeakIntervalMs);
    f.diff.take(100);
    f.diff.toggle();
    f.alloc(f.cell_addr(1), 64, 110);

    /* The first pass always runs: waiting a quarter second before the banner
     * says anything is indistinguishable from the mode being broken. */
    check(f.diff.analyze(f.table, f.map, 200, false), "clock: the first pass runs");
    check(f.diff.report().chunks == 1, "clock: and finds the candidate");

    f.alloc(f.cell_addr(2), 64, 210);
    check(!f.diff.analyze(f.table, f.map, 300, false),
          "clock: a pass inside the interval does not run");
    check(f.diff.report().chunks == 1, "clock: so the figure is the last pass's");

    check(f.diff.analyze(f.table, f.map, 200 + hv::kLeakIntervalMs, false),
          "clock: the next tick does");
    check(f.diff.report().chunks == 2, "clock: and picks up what arrived between");

    /* `force` is what a repack sets, and it has to beat the interval: the cells
     * have been renumbered and the overlay indexes the old numbering. */
    f.alloc(f.cell_addr(3), 64, 460);
    check(f.diff.analyze(f.table, f.map, 470, true), "clock: force beats the interval");
    check(f.diff.report().chunks == 3, "clock: and runs a real pass");
}

void test_an_unchanged_pass_reports_no_change() {
    Fixture f;
    f.diff.take(100);
    f.diff.toggle();
    f.alloc(f.cell_addr(1), 64, 110);

    check(f.run(200), "idle: the first pass changed something");
    check(!f.run(300), "idle: a pass over an unchanged heap changes nothing");

    /* Same count, same bytes, different cell. The summary cannot tell these
     * apart, and a frame reported as unchanged is a frame the loop does not
     * draw -- so the map would keep the old highlight. */
    f.free(f.cell_addr(1), 64, 310);
    f.alloc(f.cell_addr(9), 64, 320);
    check(f.run(400), "idle: a candidate moving between cells is a change");
    check(f.diff.report().chunks == 1, "idle: with the summary identical");
    check(f.diff.cell_count(9) == 1 && f.diff.cell_count(1) == 0,
          "idle: and the overlay following it");
}

/* --- the drawn overlay ------------------------------------------------------ */

void test_the_overlay_recolours_only_its_cells() {
    Fixture f;
    f.diff.take(100);
    f.diff.toggle();
    f.alloc(f.cell_addr(3), 2048, 110);
    f.alloc(f.cell_addr(1), 2048, 50); /* before the mark: not a candidate */
    f.run(200);

    hv::Capabilities caps{};
    hv::MapView view{caps};
    hv::Framebuffer fb;
    fb.resize(kCols + 10, kRows + 4);
    const hv::Rect area{0, 0, kCols + 10, kRows + 4};

    fb.clear(hv::Cell{U' ', 0x00FFFFFF, 0, 0});
    view.draw(fb, area, f.map, 200);

    const hv::MapLayout l = hv::map_layout(area);
    const hv::Cell before_candidate = fb.at_back(l.cells.x + 3, l.cells.y);
    const hv::Cell before_other     = fb.at_back(l.cells.x + 1, l.cells.y);

    view.draw_leaks(fb, area, f.map, f.diff);

    const hv::Cell after_candidate = fb.at_back(l.cells.x + 3, l.cells.y);
    const hv::Cell after_other     = fb.at_back(l.cells.x + 1, l.cells.y);

    check(after_candidate.fg == view.style().leak,
          "draw: the candidate's cell takes the leak colour");
    check(after_candidate.glyph == before_candidate.glyph,
          "draw: and keeps its glyph, which is what carries density");
    check(after_other.fg == before_other.fg,
          "draw: a live cell that is not a candidate is untouched");
    check(before_candidate.fg != view.style().leak,
          "draw: which the heat colour was not already");
}

void test_the_overlay_refuses_a_grid_it_was_not_computed_for() {
    Fixture f;
    f.diff.take(100);
    f.diff.toggle();
    f.alloc(f.cell_addr(3), 2048, 110);
    f.run(200);

    /* The grid moves and nothing re-analyzes: `HeapApp` forces a pass on any
     * geometry change, but a caller that has not is the case this guards. The
     * indices in the overlay now name different cells, and painting them would
     * be M3.1's gutter bug wearing a different colour. */
    f.grid.set_viewport(kCols / 2, kRows);
    f.map.configure(f.grid);
    f.map.rebuild(f.table);

    hv::Capabilities caps{};
    hv::MapView view{caps};
    hv::Framebuffer fb;
    fb.resize(kCols + 10, kRows + 4);
    const hv::Rect area{0, 0, kCols + 10, kRows + 4};

    fb.clear(hv::Cell{U' ', 0x00FFFFFF, 0, 0});
    view.draw(fb, area, f.map, 200);
    const hv::MapLayout l = hv::map_layout(area);
    const hv::Cell before = fb.at_back(l.cells.x + 3, l.cells.y);

    view.draw_leaks(fb, area, f.map, f.diff);
    check(fb.at_back(l.cells.x + 3, l.cells.y).fg == before.fg,
          "draw: a stale overlay paints nothing rather than the wrong cells");
}

} // namespace

int main() {
    test_only_allocations_after_the_mark_count();
    test_a_chunk_allocated_on_the_mark_counts();
    test_a_recycled_address_is_a_candidate();
    test_a_freed_candidate_stops_counting();

    test_candidates_land_in_their_own_cells();
    test_a_candidate_off_the_grid_is_counted_and_said_so();
    test_a_geometry_change_reindexes_the_overlay();

    test_diff_before_a_snapshot_does_nothing();
    test_off_costs_nothing();
    test_clearing_drops_the_mark_and_the_mode();
    test_a_second_mark_replaces_the_first();
    test_the_mark_carries_a_wall_clock_label();

    test_pass_runs_on_its_own_clock();
    test_an_unchanged_pass_reports_no_change();

    test_the_overlay_recolours_only_its_cells();
    test_the_overlay_refuses_a_grid_it_was_not_computed_for();

    if (g_failures != 0) {
        std::fprintf(stderr, "snapshot: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("snapshot: all checks passed\n");
    return 0;
}
