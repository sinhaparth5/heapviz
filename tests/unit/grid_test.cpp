/* heapviz - address space bucketizer checks (M3.1).
 *
 * Copyright (C) 2026 Parth Sinha
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The grid's contract is a round trip: every address inside the mapped span
 * lands in exactly one cell, and every cell's own address range lands back on
 * that cell. Most of these check that property across a whole grid rather than
 * spot-checking indices, because an off-by-one in the shift produces a grid
 * that is wrong everywhere but still looks plausible in any single example.
 *
 * The degenerate cases get as much attention as the working ones. A span of
 * zero and a viewport of zero cells both happen in normal operation -- before
 * the first event is drained, and while a window is being dragged narrow -- and
 * both would be a divide-by-zero or a shift by a negative amount if the guard
 * were dropped.
 */

#include "tui/grid.h"

#include <cstdio>
#include <cstring>
#include <string>

namespace {

int g_failures = 0;

void check(bool ok, const char *what) {
    if (!ok) { std::fprintf(stderr, "  FAIL %s\n", what); ++g_failures; }
}

bool is_pow2(std::uint64_t v) { return v != 0 && (v & (v - 1)) == 0; }

std::string offset_label(std::uint64_t off) {
    char buf[hv::kGutterWidth + 1];
    hv::format_offset(buf, sizeof buf, off);
    return std::string(buf);
}

std::string size_label(std::uint64_t bytes) {
    char buf[32];
    hv::format_byte_size(buf, sizeof buf, bytes);
    return std::string(buf);
}

/* --- granularity ---------------------------------------------------------- */

void test_granularity_is_a_power_of_two() {
    hv::Grid g;

    /* 640000 bytes over 200x50 cells is 64 bytes exactly: the one case where
     * the ceiling and the power-of-two rounding both do nothing. */
    constexpr std::uint64_t kExact = 64 * 200 * 50;
    check(g.configure(0x1000, 0x1000 + kExact, 200, 50), "gran: configured");
    check(g.cell_bytes() == 64, "gran: an exact fit is not rounded up");

    /* One byte more must not stay at 64: the grid has to cover the span. */
    check(g.configure(0x1000, 0x1000 + kExact + 1, 200, 50),
          "gran: configured, one byte over");
    check(g.cell_bytes() == 128, "gran: one byte over goes to the next power");

    int not_pow2 = 0;
    for (std::uint64_t span = 1; span < (std::uint64_t{1} << 40); span *= 3) {
        g.configure(0, span, 200, 50);
        if (!is_pow2(g.cell_bytes())) ++not_pow2;
        if (g.log2_cell_bytes() != static_cast<unsigned>(__builtin_ctzll(g.cell_bytes())))
            ++not_pow2;
    }
    check(not_pow2 == 0,
          "gran: always a power of two, and log2 always matches it");
}

void test_granularity_is_clamped() {
    hv::Grid g;

    /* A tiny heap must not produce sub-allocator-granularity cells. */
    g.configure(0, 1024, 200, 50);
    check(g.cell_bytes() == hv::kMinCellBytes,
          "gran: a tiny span clamps up to the minimum cell");

    /* A span no granularity can cover clamps down, and says so. */
    g.configure(0, std::uint64_t{1} << 62, 200, 50);
    check(g.cell_bytes() == hv::kMaxCellBytes,
          "gran: a vast span clamps down to the maximum cell");
    check(!g.covers_whole_span(),
          "gran: and admits it is not showing the whole span");

    g.configure(0x1000, 0x1000 + 640 * 1024, 200, 50);
    check(g.covers_whole_span(), "gran: an ordinary heap is covered whole");
}

/* --- the round trip ------------------------------------------------------- */

void test_every_cell_round_trips() {
    hv::Grid g;
    check(g.configure(0x55000000, 0x55000000 + 3 * 1024 * 1024, 80, 24),
          "trip: configured");

    const std::size_t cells = g.cell_count();
    check(cells == 80 * 24, "trip: cell count is the viewport");

    int bad = 0;
    for (std::size_t i = 0; i < cells; ++i) {
        const std::uint64_t first = g.base() + i * g.cell_bytes();
        const std::uint64_t last  = first + g.cell_bytes() - 1;

        std::size_t got_first = 0, got_last = 0;
        if (!g.index_of(first, got_first) || got_first != i) ++bad;
        if (!g.index_of(last, got_last) || got_last != i) ++bad;
    }
    check(bad == 0, "trip: every cell's first and last address map to it");

    /* Adjacent cells must not overlap, which the check above cannot see on its
     * own: a shift that was one too small would map both ends of every cell
     * correctly and still collide with the neighbour. */
    std::size_t a = 0, b = 0;
    const std::uint64_t boundary = g.base() + g.cell_bytes();
    check(g.index_of(boundary - 1, a) && g.index_of(boundary, b) && a == 0 && b == 1,
          "trip: the cell boundary falls exactly on cell_bytes");
}

void test_addresses_outside_the_grid() {
    hv::Grid g;
    g.configure(0x1000, 0x1000 + 64 * 1024, 80, 24);

    std::size_t idx = 0;
    check(!g.index_of(0x1000 - 1, idx), "bounds: below the base is rejected");
    check(!g.index_of(0, idx), "bounds: a null pointer is rejected");
    check(g.index_of(0x1000, idx) && idx == 0, "bounds: the base itself is cell 0");

    /* Past what the grid covers. Not an error -- a target allocates outside the
     * displayed region constantly -- but it must not produce an index that
     * would be written out of bounds. */
    const std::uint64_t past =
        g.base() + g.cell_bytes() * static_cast<std::uint64_t>(g.cell_count());
    check(!g.index_of(past, idx), "bounds: one past the last cell is rejected");
    check(!g.index_of(past + 4096, idx), "bounds: well past it too");
    check(g.index_of(past - 1, idx) && idx == g.cell_count() - 1,
          "bounds: the last covered address is the last cell");
}

/* --- degenerate inputs ---------------------------------------------------- */

void test_degenerate_inputs_are_safe() {
    hv::Grid g;
    std::size_t idx = 0;

    check(!g.configure(0x1000, 0x1000, 80, 24), "degenerate: an empty span fails");
    check(!g.valid(), "degenerate: and leaves the grid invalid");
    check(!g.index_of(0x1000, idx), "degenerate: which answers queries safely");
    check(g.log2_cell_bytes() >= 6, "degenerate: the shift stays legal");

    check(!g.configure(0x2000, 0x1000, 80, 24),
          "degenerate: an inverted span fails");
    check(!g.configure(0x1000, 0x2000, 0, 24), "degenerate: zero columns fails");
    check(!g.configure(0x1000, 0x2000, 80, 0), "degenerate: zero rows fails");
    check(!g.configure(0x1000, 0x2000, -5, -5),
          "degenerate: a negative viewport fails");
    check(g.log2_cell_bytes() >= 6,
          "degenerate: the shift is still legal after all of them");

    /* A one-cell terminal is degenerate-looking but perfectly well defined. */
    check(g.configure(0x1000, 0x1000 + 4096, 1, 1),
          "degenerate: a 1x1 viewport is valid");
    check(g.index_of(0x1000 + 4095, idx) && idx == 0,
          "degenerate: and puts the whole span in its single cell");

    /* The largest span that cannot overflow the ceiling division. */
    check(g.configure(0, UINT64_MAX, 200, 50),
          "degenerate: a full 64-bit span does not overflow");
    check(g.cell_bytes() == hv::kMaxCellBytes,
          "degenerate: and clamps rather than wrapping to something small");
}

/* --- one recompute path --------------------------------------------------- */

void test_both_paths_agree() {
    /* The invariant that keeps the gutter labels describing the same grid the
     * addresses are mapped into: arriving at a geometry via a resize and via a
     * bounds change must produce identical state. */
    hv::Grid viaresize;
    viaresize.configure(0x4000, 0x4000 + 8 * 1024 * 1024, 80, 24);
    viaresize.set_viewport(200, 50);

    hv::Grid viabounds;
    viabounds.configure(0x9999, 0x9999 + 512, 200, 50);
    viabounds.set_bounds(0x4000, 0x4000 + 8 * 1024 * 1024);

    check(viaresize.cell_bytes() == viabounds.cell_bytes() &&
              viaresize.log2_cell_bytes() == viabounds.log2_cell_bytes() &&
              viaresize.base() == viabounds.base() &&
              viaresize.end() == viabounds.end() &&
              viaresize.cols() == viabounds.cols() &&
              viaresize.rows() == viabounds.rows(),
          "recompute: a resize and a bounds change reach the same geometry");

    /* And the granularity actually tracks the viewport, rather than being
     * computed once and kept. */
    hv::Grid g;
    g.configure(0, 4 * 1024 * 1024, 200, 50);
    const std::uint64_t wide = g.cell_bytes();
    g.set_viewport(80, 24);
    check(g.cell_bytes() > wide,
          "recompute: a smaller terminal means coarser cells");
    g.set_viewport(200, 50);
    check(g.cell_bytes() == wide, "recompute: and resizing back restores it");
}

void test_rows_and_columns() {
    hv::Grid g;
    g.configure(0x1000, 0x1000 + 1024 * 1024, 80, 24);

    check(g.row_of(0) == 0 && g.col_of(0) == 0, "rowcol: cell 0 is at 0,0");
    check(g.row_of(79) == 0 && g.col_of(79) == 79, "rowcol: cell 79 ends row 0");
    check(g.row_of(80) == 1 && g.col_of(80) == 0, "rowcol: cell 80 starts row 1");

    check(g.offset_of_row(0) == 0, "rowcol: row 0 is at offset zero");
    check(g.offset_of_row(1) == 80 * g.cell_bytes(),
          "rowcol: each row advances by a full row of cells");
    check(g.addr_of_row(2) == g.base() + 2 * 80 * g.cell_bytes(),
          "rowcol: row addresses are offsets from the base");

    /* The first address of each row must be the address of that row's cell 0,
     * or the gutter is labelling a different grid than the one being drawn. */
    int bad = 0;
    for (int r = 0; r < g.rows(); ++r) {
        std::size_t idx = 0;
        if (!g.index_of(g.addr_of_row(r), idx)) { ++bad; continue; }
        if (g.row_of(idx) != r || g.col_of(idx) != 0) ++bad;
    }
    check(bad == 0, "rowcol: every row label points at that row's first cell");
}

/* --- the two formatters --------------------------------------------------- */

void test_byte_size_formatting() {
    check(size_label(64) == "64 B", "legend: bytes");
    check(size_label(256) == "256 B", "legend: the mockup's 256 B");
    check(size_label(4096) == "4 KB", "legend: whole kilobytes carry no decimal");
    check(size_label(1536) == "1.5 KB", "legend: a half kilobyte does");
    check(size_label(1024 * 1024) == "1 MB", "legend: megabytes");
    check(size_label(hv::kMaxCellBytes) == "1 GB", "legend: the maximum cell");

    /* The legend has to show the live figure, so check it against a grid rather
     * than only against constants. */
    hv::Grid g;
    g.configure(0, 64 * 200 * 50, 200, 50);
    check(size_label(g.cell_bytes()) == "64 B", "legend: reads a live grid");
}

void test_gutter_labels_are_fixed_width() {
    const std::uint64_t offsets[] = {
        0, 1, 512, 1023, 1024, 1536, 65536, 1024 * 1024,
        3 * 1024 * 1024 / 2, std::uint64_t{1} << 30, std::uint64_t{1} << 40,
        std::uint64_t{1} << 50, UINT64_MAX,
    };

    int wrong_width = 0;
    for (std::uint64_t off : offsets) {
        if (offset_label(off).size() != hv::kGutterWidth) ++wrong_width;
    }
    check(wrong_width == 0, "gutter: every label is exactly kGutterWidth wide");

    check(offset_label(0) == "     0B", "gutter: zero is right-aligned");
    check(offset_label(512) == "   512B", "gutter: bytes");
    check(offset_label(1024) == "    1KB", "gutter: exact kilobytes");
    check(offset_label(1536) == "  1.5KB", "gutter: fractional kilobytes");
    check(offset_label(1024 * 1024) == "    1MB", "gutter: megabytes");

    /* The buffer contract: too small a buffer must produce an empty string, not
     * a partial label written past the end of it. */
    char tiny[4] = {'x', 'x', 'x', 'x'};
    check(hv::format_offset(tiny, sizeof tiny, 4096) == 0 && tiny[0] == '\0',
          "gutter: an undersized buffer is refused, not overrun");
}

} // namespace

int main() {
    test_granularity_is_a_power_of_two();
    test_granularity_is_clamped();
    test_every_cell_round_trips();
    test_addresses_outside_the_grid();
    test_degenerate_inputs_are_safe();
    test_both_paths_agree();
    test_rows_and_columns();
    test_byte_size_formatting();
    test_gutter_labels_are_fixed_width();

    if (g_failures != 0) {
        std::fprintf(stderr, "grid_test: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("grid_test: granularity, the address round trip, degenerate "
                "viewports and both label formats all hold\n");
    return 0;
}
