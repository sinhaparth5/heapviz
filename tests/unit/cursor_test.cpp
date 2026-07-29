/* heapviz - spatial cursor checks (M5.1).
 *
 * Copyright (C) 2026 Parth Sinha
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Three properties, and each of them has already been a bug in something else in
 * this codebase:
 *
 * **Movement saturates.** Every arm of `move` does unsigned arithmetic on a cell
 * index, so a subtraction that ran past zero would not clamp -- it would put the
 * cursor at the far end of the heap. `h` at cell 0 and `k` on the top row are
 * therefore checked explicitly rather than assumed.
 *
 * **A resize keeps the address, not the cell.** This is the property ROADMAP
 * M5.1 asks for by name, and the reason the cursor stores a coordinate instead
 * of a row and column. The check is the round trip: put the cursor on a known
 * address, reflow the grid to a different width, and require the address under
 * it to be unchanged.
 *
 * **`n` agrees with what is on screen.** `MapView::glyph_for` and the cursor
 * both ask `cell_occupied`, and a cursor that stopped on a cell the user reads
 * as dark looks like `n` is broken rather than like two predicates that drifted.
 * The test drives allocations through the real `HeatMap` and requires every cell
 * `n` lands on to be one the map agrees is occupied.
 */

#include "tui/cursor.h"
#include "tui/heatmap.h"
#include "tui/map_view.h"

#include <cstdio>
#include <vector>

namespace {

int g_failures = 0;

void check(bool ok, const char *what) {
    if (!ok) { std::fprintf(stderr, "  FAIL %s\n", what); ++g_failures; }
}

/* A grid over a heap that is an exact multiple of the cell size, so "the last
 * cell of the heap" and "the last cell of the grid" coincide and a test about
 * movement is not also a test about the filler tail. 40 cols x 10 rows over
 * 400 * 4096 bytes gives exactly 4 KiB per cell. */
constexpr std::uint64_t kBase  = 0x600000000000ull;
constexpr int           kCols  = 40;
constexpr int           kRows  = 10;
constexpr std::uint64_t kCell  = 4096;

hv::Grid grid_of(int cols, int rows) {
    hv::Grid g;
    g.configure(kBase, kBase + kCell * static_cast<std::uint64_t>(kCols * kRows),
                cols, rows);
    return g;
}

hv::HeatMap map_of(const hv::Grid &g) {
    hv::HeatMap m;
    m.configure(g);
    return m;
}

/* --- movement --------------------------------------------------------------- */

void test_the_cursor_starts_at_the_heap_base() {
    const hv::Grid g = grid_of(kCols, kRows);
    const hv::MapCursor c;

    check(g.valid(), "setup: the grid configured");
    check(g.cell_bytes() == kCell, "setup: 4 KiB cells, as the arithmetic says");
    check(c.index(g) == 0, "start: cell 0");
    check(c.coord() == 0, "start: coordinate 0, before anything sets it");
}

void test_single_steps_move_one_cell() {
    const hv::Grid g = grid_of(kCols, kRows);
    hv::HeatMap m = map_of(g);
    hv::MapCursor c;
    c.set_coord(g, kBase + kCell * 45); /* row 1, col 5 */

    check(c.index(g) == 45, "step: set_coord lands where it says");
    check(c.row(g) == 1 && c.col(g) == 5, "step: row and column agree");

    check(c.move(m, hv::CursorMove::Right), "step: l reports a move");
    check(c.index(g) == 46, "step: l is one cell");
    check(c.move(m, hv::CursorMove::Left) && c.index(g) == 45, "step: h undoes it");
    check(c.move(m, hv::CursorMove::Down) && c.index(g) == 45 + kCols,
          "step: j is one row");
    check(c.move(m, hv::CursorMove::Up) && c.index(g) == 45, "step: k undoes it");

    /* Vertical movement keeps the column; horizontal movement does not keep the
     * row, because the map is one ribbon of address space wrapped at the
     * terminal's width and the cell before column 0 is the previous row's last. */
    check(c.col(g) == 5, "step: j then k keeps the column");
    c.set_coord(g, kBase + kCell * static_cast<std::uint64_t>(kCols));
    check(c.move(m, hv::CursorMove::Left), "step: h at column 0 moves");
    check(c.index(g) == static_cast<std::size_t>(kCols) - 1,
          "step: h at column 0 lands on the previous row's last cell");
}

void test_jumps_are_ten_and_clamp() {
    /* Taller than the ten-row default on purpose: a `J` on a ten-row grid can
     * only ever clamp, so testing the jump distance there would be testing the
     * clamp twice and the distance not at all. */
    const hv::Grid g = grid_of(kCols, 30);
    hv::HeatMap m = map_of(g);
    hv::MapCursor c;

    /* Through the grid's own granularity rather than kCell: more rows over the
     * same heap is a finer cell, and an address computed from the wrong one
     * would land on a different index than the arithmetic below assumes. */
    const std::uint64_t cell = g.cell_bytes();

    c.set_coord(g, kBase + cell * 200);
    check(c.move(m, hv::CursorMove::RightFar) && c.index(g) == 210,
          "jump: L is ten cells");
    check(c.move(m, hv::CursorMove::LeftFar) && c.index(g) == 200,
          "jump: H undoes it");
    check(c.move(m, hv::CursorMove::DownFar) &&
              c.index(g) == 200 + 10 * static_cast<std::size_t>(kCols),
          "jump: J is ten rows");
    check(c.move(m, hv::CursorMove::UpFar) && c.index(g) == 200,
          "jump: K undoes it");

    /* Fewer than ten rows above: clamp to the top of the column rather than
     * refusing, which is what makes K usable near the start of the heap. */
    c.set_coord(g, kBase + cell * (2 * static_cast<std::uint64_t>(kCols) + 7));
    check(c.move(m, hv::CursorMove::UpFar), "jump: K near the top still moves");
    check(c.index(g) == 7, "jump: K clamps to row 0, keeping the column");

    c.set_coord(g, kBase + cell * 3);
    check(c.move(m, hv::CursorMove::LeftFar), "jump: H near the start moves");
    check(c.index(g) == 0, "jump: H clamps to the first cell");
}

void test_movement_never_wraps_past_either_end() {
    const hv::Grid g = grid_of(kCols, kRows);
    hv::HeatMap m = map_of(g);
    hv::MapCursor c;

    /* The whole point of this test: `to` is a std::size_t, so an unclamped
     * decrement at cell 0 is not a no-op, it is 2^64-1 -- and the cursor would
     * appear at the end of the heap on a keypress that means "left". */
    check(!c.move(m, hv::CursorMove::Left), "bounds: h at cell 0 is refused");
    check(c.index(g) == 0, "bounds: and does not move");
    check(!c.move(m, hv::CursorMove::Up), "bounds: k on row 0 is refused");
    check(!c.move(m, hv::CursorMove::LeftFar), "bounds: H at cell 0 is refused");
    check(!c.move(m, hv::CursorMove::UpFar), "bounds: K on row 0 is refused");
    check(c.index(g) == 0, "bounds: none of the four moved it");

    const std::size_t last = hv::last_heap_cell(g);
    check(last == static_cast<std::size_t>(kCols * kRows) - 1,
          "bounds: an exact heap ends on the grid's last cell");

    c.move(m, hv::CursorMove::End);
    check(c.index(g) == last, "bounds: G is the last cell of the heap");
    check(!c.move(m, hv::CursorMove::Right), "bounds: l at the end is refused");
    check(!c.move(m, hv::CursorMove::Down), "bounds: j on the last row is refused");
    check(!c.move(m, hv::CursorMove::RightFar), "bounds: L at the end is refused");
    check(!c.move(m, hv::CursorMove::DownFar), "bounds: J on the last row is refused");
    check(c.index(g) == last, "bounds: none of the four moved it");

    check(c.move(m, hv::CursorMove::Start) && c.index(g) == 0, "bounds: g is cell 0");
    check(!c.move(m, hv::CursorMove::Start), "bounds: g again reports no change");
}

/* The grid usually has more cells than the target has heap: `cell_bytes` is
 * rounded up to a power of two, so the last row is part filler. `G` has to land
 * on the end of the *heap*, not in the blank tail -- the roadmap's words are
 * "jump to heap start / end". */
void test_the_end_is_the_end_of_the_heap_not_of_the_grid() {
    hv::Grid g;
    /* 400 cells over a 600 KiB span: 1536 B a cell rounds up to 2048, so the
     * heap runs out 100 cells before the grid does. */
    g.configure(kBase, kBase + 600 * 1024, kCols, kRows);
    hv::HeatMap m = map_of(g);

    check(g.cell_bytes() == 2048, "tail: the granularity rounded up as expected");
    const std::size_t last = hv::last_heap_cell(g);
    check(last == 299, "tail: the heap's last byte is in cell 299");
    check(last < g.cell_count() - 1, "tail: and the grid has cells beyond it");

    hv::MapCursor c;
    c.move(m, hv::CursorMove::End);
    check(c.index(g) == last, "tail: G stops at the end of the heap");
    check(!c.move(m, hv::CursorMove::Right), "tail: and l will not go further");
}

/* --- resize ----------------------------------------------------------------- */

void test_a_resize_keeps_the_address() {
    const hv::Grid wide = grid_of(kCols, kRows);
    hv::HeatMap m = map_of(wide);

    hv::MapCursor c;
    c.set_coord(wide, kBase + kCell * 137);
    const std::uint64_t addr = c.coord();
    check(c.index(wide) == 137, "resize: parked on cell 137");

    /* A narrower terminal over the same heap. Same total bytes, different
     * number of cells, so the cell *index* of that address must change and the
     * address must not. A cursor holding a row and column would silently start
     * pointing somewhere else here, with nothing to tell the user. */
    const hv::Grid narrow = grid_of(25, kRows);
    check(narrow.valid(), "resize: the narrow grid configured");
    check(narrow.cell_bytes() != wide.cell_bytes(),
          "resize: the granularity really did change");

    c.refit(narrow);
    check(c.coord() == addr, "resize: the address under the cursor is unchanged");

    std::size_t expect = 0;
    check(narrow.index_of(addr, expect), "resize: the address is still on screen");
    check(c.index(narrow) == expect, "resize: the cell index followed the reflow");

    /* And the other direction: a heap that shrank out from under the cursor
     * pulls it back rather than leaving it pointing at nothing. */
    hv::Grid small;
    small.configure(kBase, kBase + kCell * 8, kCols, kRows);
    check(c.refit(small), "resize: a shrunk heap reports the cursor was moved");
    check(c.index(small) <= hv::last_heap_cell(small),
          "resize: and it lands inside the new heap");
}

/* --- n / N ------------------------------------------------------------------ */

/* Allocations at four widely separated cells, so "next non-empty" has an answer
 * the test knows without asking the map. */
std::vector<std::size_t> seed_sparse(hv::HeatMap &m, const hv::Grid &g) {
    const std::vector<std::size_t> at{3, 90, 91, 322};
    for (std::size_t i : at)
        m.on_alloc(kBase + kCell * i, 64, 80, 10);
    (void)g;
    return at;
}

void test_n_and_N_visit_exactly_the_occupied_cells() {
    const hv::Grid g = grid_of(kCols, kRows);
    hv::HeatMap m = map_of(g);
    const std::vector<std::size_t> at = seed_sparse(m, g);

    hv::MapCursor c;
    check(c.index(g) == 0, "n: starting before the first allocation");

    for (std::size_t want : at) {
        check(c.move(m, hv::CursorMove::NextLive), "n: found the next allocation");
        check(c.index(g) == want, "n: and it is the next one in address order");
        /* The predicate the display uses, asked of the cell the cursor chose. A
         * cursor stopping on a cell the map draws as dark is the failure this
         * shares its definition of "empty" to prevent. */
        check(hv::cell_occupied(m.at(c.index(g))),
              "n: the cell it stopped on is one the map draws as occupied");
    }

    check(!c.move(m, hv::CursorMove::NextLive),
          "n: past the last allocation there is nothing to find");
    check(c.index(g) == at.back(), "n: and the cursor stayed put");

    for (std::size_t i = at.size() - 1; i-- > 0;) {
        check(c.move(m, hv::CursorMove::PrevLive), "N: found the previous one");
        check(c.index(g) == at[i], "N: in descending address order");
    }
    check(!c.move(m, hv::CursorMove::PrevLive),
          "N: before the first allocation there is nothing to find");
    check(c.index(g) == at.front(), "N: and the cursor stayed put");
}

void test_n_skips_a_cell_whose_chunk_was_freed() {
    const hv::Grid g = grid_of(kCols, kRows);
    hv::HeatMap m = map_of(g);

    m.on_alloc(kBase + kCell * 50, 64, 80, 10);
    m.on_alloc(kBase + kCell * 60, 64, 80, 10);

    hv::MapCursor c;
    check(c.move(m, hv::CursorMove::NextLive) && c.index(g) == 50,
          "freed: n finds the first while it is live");

    /* `on_free` leaves the cell flashing red for a couple of seconds but holding
     * nothing, and the whole reason `n` exists is to reach allocations. A cell
     * whose only content is a fading memory of one is not a destination. */
    c.set_coord(g, kBase);
    m.on_free(kBase + kCell * 50, 64, 80, 20);
    check(!hv::cell_occupied(m.at(50)), "freed: the cell is empty again");
    check(c.move(m, hv::CursorMove::NextLive) && c.index(g) == 60,
          "freed: n skips it and finds the live one beyond");
}

/* --- keys ------------------------------------------------------------------- */

void test_the_bindings_are_the_ones_documented() {
    const struct { char key; hv::CursorMove want; } table[] = {
        {'h', hv::CursorMove::Left},     {'l', hv::CursorMove::Right},
        {'k', hv::CursorMove::Up},       {'j', hv::CursorMove::Down},
        {'H', hv::CursorMove::LeftFar},  {'L', hv::CursorMove::RightFar},
        {'K', hv::CursorMove::UpFar},    {'J', hv::CursorMove::DownFar},
        {'g', hv::CursorMove::Start},    {'G', hv::CursorMove::End},
        {'n', hv::CursorMove::NextLive}, {'N', hv::CursorMove::PrevLive},
    };
    for (const auto &row : table) {
        hv::CursorMove got{};
        check(hv::cursor_move_for_key(row.key, got) && got == row.want,
              "keys: the binding is what the footer and the README claim");
    }

    /* `q` above all: it is the only key the event loop has no opinion about, so
     * a cursor that claimed it would leave a heapviz that has to be killed. */
    hv::CursorMove ignored{};
    check(!hv::cursor_move_for_key('q', ignored), "keys: q is not a movement");
    check(!hv::cursor_move_for_key('\x03', ignored), "keys: nor is Ctrl-C");
    check(!hv::cursor_move_for_key('a', ignored), "keys: nor is an unbound key");
}

/* --- drawing ---------------------------------------------------------------- */

void test_the_overlay_marks_one_cell_and_keeps_its_colours() {
    const hv::Grid g = grid_of(kCols, kRows);
    hv::HeatMap m = map_of(g);
    m.on_alloc(kBase + kCell * 45, 2048, 2048, 10);

    hv::Framebuffer fb;
    fb.resize(120, 40);

    hv::Capabilities caps;
    caps.color   = hv::ColorMode::TrueColor;
    caps.unicode = true;
    hv::MapView view(caps);

    /* Wide enough for the gutter, and tall enough for the legend plus every
     * row, so the layout under test is the full one. */
    const hv::Rect area{0, 5, 120, kRows + 1};
    view.draw(fb, area, m, 500);

    const hv::MapLayout l = hv::map_layout(area);
    check(l.valid && l.gutter_x >= 0, "draw: the layout has a gutter");

    hv::MapCursor c;
    c.set_coord(g, kBase + kCell * 45);
    const int x = l.cells.x + c.col(g);
    const int y = l.cells.y + c.row(g);

    const hv::Cell before = fb.at_back(x, y);
    const hv::Cell right  = fb.at_back(x + 1, y);
    view.draw_cursor(fb, area, m, c);

    const hv::Cell after = fb.at_back(x, y);
    check(after.bg == view.style().cursor, "draw: the cursor cell is highlighted");
    check(after.glyph == before.glyph,
          "draw: and keeps its density glyph, so the cursor hides nothing");
    check(after.fg == before.fg,
          "draw: and keeps its heat colour, which is what the cell means");
    check(fb.at_back(x + 1, y) == right,
          "draw: the neighbour is untouched -- no box drawn through it");

    /* The gutter label for the row is recoloured, which is the only other cell
     * the overlay is allowed to touch. */
    check(fb.at_back(l.gutter_x, y).fg == view.style().cursor,
          "draw: the row's gutter label is marked");
    check(fb.at_back(l.gutter_x, y + 1).fg != view.style().cursor,
          "draw: but only that row's");
}

void test_the_overlay_is_a_no_op_without_a_grid() {
    const hv::HeatMap m; /* never configured */
    hv::Framebuffer fb;
    fb.resize(80, 24);
    fb.clear();

    hv::Capabilities caps;
    hv::MapView view(caps);
    const hv::MapCursor c;

    view.draw_cursor(fb, hv::Rect{0, 0, 80, 24}, m, c);
    for (int y = 0; y < 24; ++y)
        for (int x = 0; x < 80; ++x)
            check(fb.at_back(x, y) == hv::empty_cell(),
                  "draw: nothing is painted before the map has bounds");
}

} // namespace

int main() {
    test_the_cursor_starts_at_the_heap_base();
    test_single_steps_move_one_cell();
    test_jumps_are_ten_and_clamp();
    test_movement_never_wraps_past_either_end();
    test_the_end_is_the_end_of_the_heap_not_of_the_grid();
    test_a_resize_keeps_the_address();
    test_n_and_N_visit_exactly_the_occupied_cells();
    test_n_skips_a_cell_whose_chunk_was_freed();
    test_the_bindings_are_the_ones_documented();
    test_the_overlay_marks_one_cell_and_keeps_its_colours();
    test_the_overlay_is_a_no_op_without_a_grid();

    if (g_failures != 0) {
        std::fprintf(stderr, "cursor: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("cursor: all checks passed\n");
    return 0;
}
