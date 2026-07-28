/* heapviz - heap map drawing checks (M3.1 legend and gutter).
 *
 * Copyright (C) 2026 Parth Sinha
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The failure this file is mostly about is not a crash. If the grid is
 * configured for one viewport and the map is painted into another, every cell
 * still gets a colour, the gutter still gets a plausible label, and the display
 * is simply wrong by a row -- so the checks that matter compare what was drawn
 * against what the grid says should have been drawn, rather than against a
 * constant somebody typed twice.
 *
 * Reading the framebuffer back is deliberate. Asserting on the layout struct
 * would test the arithmetic and miss the part where the arithmetic is used.
 */

#include "tui/framebuffer.h"
#include "tui/map_view.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <string>

namespace {

int g_failures = 0;

void check(bool ok, const char *what) {
    if (!ok) { std::fprintf(stderr, "  FAIL %s\n", what); ++g_failures; }
}

constexpr std::uint64_t kBase = 0x55A0000000ull;
constexpr std::uint64_t kSpan = 4 * 1024 * 1024;

const hv::Capabilities kUnicode{hv::ColorMode::TrueColor, true};
const hv::Capabilities kAscii{hv::ColorMode::Ansi16, false};

/* A map over `area`, sized the way a caller is supposed to size it. */
hv::HeatMap make_map(hv::Rect area, std::uint64_t span = kSpan) {
    hv::Grid g;
    g.set_bounds(kBase, kBase + span);
    hv::fit_grid(g, area);

    hv::HeatMap m;
    m.configure(g);
    return m;
}

/* Reads a run of cells back as a UTF-8 string, for the text checks. */
std::string read_text(const hv::Framebuffer &fb, int x, int y, int len) {
    std::string out;
    for (int i = 0; i < len && fb.contains(x + i, y); ++i) {
        const char32_t g = fb.at_back(x + i, y).glyph;
        out.push_back(g < 128 ? static_cast<char>(g) : '?');
    }
    return out;
}

/* --- geometry ------------------------------------------------------------- */

/* The whole point of `fit_grid`: the grid is configured for exactly the cells
 * that get painted. Checked by painting a map whose every cell is distinct and
 * then finding the last one on screen. */
void test_the_grid_is_sized_to_what_is_drawn() {
    const hv::Rect area{0, 0, 100, 30};

    hv::Grid g;
    g.set_bounds(kBase, kBase + kSpan);
    check(hv::fit_grid(g, area), "fit: a 100x30 area holds a map");

    const hv::MapLayout l = hv::map_layout(area);
    check(g.cols() == l.cells.w, "fit: grid columns are the drawn columns");
    check(g.rows() == l.cells.h, "fit: grid rows are the drawn rows");
    check(g.cell_count() == static_cast<std::size_t>(l.cells.w) *
                            static_cast<std::size_t>(l.cells.h),
          "fit: every grid cell has somewhere to go");

    /* The gutter costs its width plus a gap; the legend costs one row. */
    check(l.cells.w == area.w - static_cast<int>(hv::kGutterWidth) - 1,
          "fit: the gutter is paid for out of the columns");
    check(l.cells.h == area.h - 1, "fit: the legend is paid for out of the rows");
}

/* Fill every cell of the map, then require the painted rectangle to end exactly
 * where the layout says. One cell too many is a map drawn over the row below
 * it; one too few is a column of address space that is never displayed. */
void test_nothing_is_drawn_outside_the_area() {
    const hv::Rect area{4, 3, 60, 20};

    hv::Framebuffer fb;
    fb.resize(80, 30);

    /* A background nothing in the map draws, so any escape is visible. */
    const hv::Cell marker{U'X', 0x00FF00FF, 0x00FF00FF, 0};
    fb.clear(marker);

    hv::HeatMap m = make_map(area);
    for (std::size_t i = 0; i < m.cell_count(); ++i) {
        const std::uint64_t addr =
            m.grid().base() + m.grid().cell_bytes() * i;
        m.on_alloc(addr, 4096, 4096, 100);
    }

    const hv::MapView view{kUnicode};
    view.draw(fb, area, m, 5000);

    int escaped = 0, painted = 0;
    for (int y = 0; y < fb.height(); ++y) {
        for (int x = 0; x < fb.width(); ++x) {
            const bool inside = x >= area.x && x < area.x + area.w &&
                                y >= area.y && y < area.y + area.h;
            const bool changed = fb.at_back(x, y) != marker;
            if (changed && !inside) ++escaped;
            if (changed && inside)  ++painted;
        }
    }
    check(escaped == 0, "clip: nothing was drawn outside the area");
    check(painted > 0, "clip: something was drawn inside it");

    /* And specifically: the cells occupy the rectangle the layout named. */
    const hv::MapLayout l = hv::map_layout(area);
    for (int row = 0; row < l.cells.h; ++row) {
        check(fb.at_back(l.cells.x + l.cells.w - 1, l.cells.y + row) != marker,
              "clip: the last column of every row is painted");
    }
    check(fb.at_back(l.cells.x + l.cells.w, l.cells.y) == marker ||
              l.cells.x + l.cells.w >= area.x + area.w,
          "clip: nothing is painted past the last column");
}

/* A window being dragged narrow is ordinary operation, not an error path. */
void test_degenerate_areas_draw_nothing_and_do_not_crash() {
    hv::Framebuffer fb;
    fb.resize(40, 10);

    const hv::Cell marker{U'X', 0x00FF00FF, 0x00FF00FF, 0};
    const hv::MapView view{kUnicode};

    const hv::Rect degenerate[] = {
        {0, 0, 0, 0}, {0, 0, -5, 10}, {0, 0, 10, 0}, {0, 0, 1, 1},
        {38, 8, 20, 20}, /* runs off the buffer */
    };

    for (const hv::Rect &r : degenerate) {
        fb.clear(marker);
        hv::HeatMap m = make_map(r);
        view.draw(fb, r, m, 1000);

        for (int y = 0; y < fb.height(); ++y)
            for (int x = 0; x < fb.width(); ++x) {
                const bool inside = x >= r.x && x < r.x + r.w &&
                                    y >= r.y && y < r.y + r.h;
                if (!inside)
                    check(fb.at_back(x, y) == marker,
                          "degenerate: nothing escapes a degenerate area");
            }
    }

    /* And the grid is told, rather than left describing a viewport that is no
     * longer on screen. */
    hv::Grid g;
    g.set_bounds(kBase, kBase + kSpan);
    hv::fit_grid(g, hv::Rect{0, 0, 100, 30});
    check(g.valid(), "degenerate: valid before");
    check(!hv::fit_grid(g, hv::Rect{0, 0, 0, 12}),
          "degenerate: a zero-width area is refused");
    check(!g.valid(), "degenerate: the grid goes invalid rather than stale");

    /* But a tiny area is not a degenerate one. Three columns of address space
     * is a useless display and a legal grid, and conflating the two would mean
     * a window dragged narrow came back blank instead of coarse. */
    check(hv::fit_grid(g, hv::Rect{0, 0, 3, 1}),
          "degenerate: 3x1 is small, not invalid");
    check(g.cell_count() == 3, "degenerate: and it holds exactly three cells");
}

/* Below kMinMapCols the gutter is costing more columns than it explains, so it
 * is dropped and the cells get the width. The two consumers of that decision
 * must agree, which is why there is only one of it. */
void test_the_gutter_is_dropped_before_the_map_is() {
    const hv::Rect narrow{0, 0, 12, 8};
    const hv::MapLayout l = hv::map_layout(narrow);

    check(l.valid, "narrow: 12 columns still holds a map");
    check(l.gutter_x < 0, "narrow: the gutter was dropped");
    check(l.cells.w == narrow.w, "narrow: the cells got the whole width");

    hv::Framebuffer fb;
    fb.resize(12, 8);
    const hv::Cell marker{U'X', 0x00FF00FF, 0x00FF00FF, 0};
    fb.clear(marker);

    hv::HeatMap m = make_map(narrow);
    const hv::MapView view{kUnicode};
    view.draw(fb, narrow, m, 1000);

    /* Column 0 of a map row is a cell, not a label. */
    check(fb.at_back(0, l.cells.y).glyph != U' ',
          "narrow: column 0 is map, not gutter");

    const hv::Rect wide{0, 0, 40, 8};
    check(hv::map_layout(wide).gutter_x == 0,
          "wide: the gutter comes back when there is room");
}

/* --- the gutter ----------------------------------------------------------- */

/* Each label must be the offset of *its own* row. The check recomputes them
 * from the grid, so a map drawn one row out of step fails on every row rather
 * than on none. */
void test_gutter_labels_name_their_own_row() {
    const hv::Rect area{2, 1, 90, 25};

    hv::Framebuffer fb;
    fb.resize(100, 30);
    fb.clear();

    hv::HeatMap m = make_map(area);
    const hv::MapView view{kUnicode};
    view.draw(fb, area, m, 1000);

    const hv::MapLayout l = hv::map_layout(area);
    const hv::Grid     &g = m.grid();
    check(l.gutter_x >= 0, "gutter: it is present at 90 columns");

    int mismatches = 0, distinct = 0;
    std::string previous;
    for (int row = 0; row < g.rows(); ++row) {
        char expect[hv::kGutterWidth + 1];
        hv::format_offset(expect, sizeof expect, g.offset_of_row(row));

        const std::string got =
            read_text(fb, l.gutter_x, l.cells.y + row,
                      static_cast<int>(hv::kGutterWidth));
        if (got != expect) ++mismatches;
        if (got != previous) { ++distinct; previous = got; }
    }
    check(mismatches == 0, "gutter: every label is its row's offset");

    /* Anti-vacuity: a column of identical strings would also have matched a
     * broken `offset_of_row` returning a constant. */
    check(distinct == g.rows(), "gutter: the labels actually differ per row");

    /* Fixed width, so the map does not shift sideways as the heap grows. */
    for (int row = 0; row < g.rows(); ++row) {
        const std::string got =
            read_text(fb, l.gutter_x, l.cells.y + row,
                      static_cast<int>(hv::kGutterWidth) + 1);
        check(got.size() == hv::kGutterWidth + 1 && got.back() == ' ',
              "gutter: a gap separates the label from the map");
    }
}

/* --- the legend ----------------------------------------------------------- */

/* The mockup's `(1 cell = 256 B)` has to be the live value. A constant there is
 * a lie the moment the window is resized -- which is exactly what this drives. */
void test_the_legend_shows_the_live_granularity() {
    hv::Framebuffer fb;
    fb.resize(120, 40);

    const hv::MapView view{kUnicode};

    struct Size { int w, h; };
    const Size sizes[] = {{120, 40}, {90, 30}, {80, 24}};

    std::string first_seen;
    int changes = 0;

    for (const Size &s : sizes) {
        const hv::Rect area{0, 0, s.w, s.h};
        fb.clear();

        hv::HeatMap m = make_map(area);
        view.draw(fb, area, m, 1000);

        char expect[24];
        hv::format_byte_size(expect, sizeof expect, m.grid().cell_bytes());

        char want[48];
        std::snprintf(want, sizeof want, "1 cell = %s", expect);

        const std::string got =
            read_text(fb, area.x, area.y, static_cast<int>(std::strlen(want)));
        check(got == want, "legend: the granularity is the grid's live value");

        if (first_seen.empty()) first_seen = got;
        else if (got != first_seen) ++changes;
    }

    /* Anti-vacuity: if the legend never changed across those three sizes, the
     * check above would pass against a hardcoded string. */
    check(changes > 0, "legend: the value moves when the geometry does");
}

/* A span too wide for any granularity on screen loses its top. The README's
 * commitment is that heapviz says so. */
void test_a_truncated_span_is_reported() {
    const hv::Rect area{0, 0, 100, 30};

    hv::Framebuffer fb;
    fb.resize(100, 30);
    const hv::MapView view{kUnicode};

    /* The gap between a brk heap and an mmap region: about 47 bits, which no
     * clamped granularity can cover. */
    fb.clear();
    hv::HeatMap wide = make_map(area, std::uint64_t{1} << 47);
    check(!wide.grid().covers_whole_span(), "truncated: the span really is short");
    view.draw(fb, area, wide, 1000);
    const std::string wide_legend = read_text(fb, 0, 0, area.w);
    check(wide_legend.find("top of range not shown") != std::string::npos,
          "truncated: the legend says the top is missing");

    fb.clear();
    hv::HeatMap ok = make_map(area);
    check(ok.grid().covers_whole_span(), "truncated: 4 MiB fits");
    view.draw(fb, area, ok, 1000);
    const std::string ok_legend = read_text(fb, 0, 0, area.w);
    check(ok_legend.find("top of range not shown") == std::string::npos,
          "truncated: and stays quiet when nothing is missing");
}

/* Before the first event there are no bounds, and a legend claiming "1 cell =
 * 64 B" would be describing a grid that does not exist. */
void test_no_bounds_says_so() {
    const hv::Rect area{0, 0, 80, 24};

    hv::Framebuffer fb;
    fb.resize(80, 24);
    fb.clear();

    hv::HeatMap m; /* never configured: no bounds, no cells */
    const hv::MapView view{kUnicode};
    view.draw(fb, area, m, 1000);

    const std::string legend = read_text(fb, 0, 0, area.w);
    check(legend.find("no heap bounds yet") != std::string::npos,
          "unbounded: the legend says there is nothing to describe");

    /* And says *only* that. A granularity printed beside it would be describing
     * a grid that does not exist, which is the failure the message replaces. */
    check(legend.find("1 cell") == std::string::npos,
          "unbounded: and does not also quote a granularity");
}

/* The legend is a row inside somebody else's layout, and `Framebuffer::text`
 * clips to the buffer rather than to that row. A legend long enough to overrun
 * a narrow panel must lose its last item, not paint over whatever sits beside
 * it. */
void test_the_legend_does_not_spill_out_of_its_rect() {
    const hv::Rect area{2, 1, 30, 10};

    hv::Framebuffer fb;
    fb.resize(80, 20);
    const hv::Cell marker{U'X', 0x00FF00FF, 0x00FF00FF, 0};
    fb.clear(marker);

    /* A span that cannot be covered, so the legend has one more thing it wants
     * to say than it has room for. */
    hv::HeatMap m = make_map(area, std::uint64_t{1} << 47);
    check(!m.grid().covers_whole_span(), "spill: the notice is wanted");

    const hv::MapView view{kUnicode};
    view.draw(fb, area, m, 1000);

    int spilled = 0;
    for (int x = 0; x < fb.width(); ++x) {
        const bool inside = x >= area.x && x < area.x + area.w;
        if (!inside && fb.at_back(x, area.y) != marker) ++spilled;
    }
    check(spilled == 0, "spill: the legend stopped at the edge of its row");

    const std::string legend = read_text(fb, area.x, area.y, area.w);
    check(legend.find("1 cell") != std::string::npos,
          "spill: and kept the item that did fit");
}

/* `fit_grid` is how a caller is supposed to size the grid, but a resize that
 * reached the draw before it reached the model is a real ordering, and the
 * answer to it is a clipped map rather than cells landing in the panel below. */
void test_a_map_larger_than_its_area_is_clipped() {
    const hv::Rect big{0, 0, 100, 30};
    const hv::Rect small{0, 0, 40, 12};

    hv::Framebuffer fb;
    fb.resize(100, 30);
    const hv::Cell marker{U'X', 0x00FF00FF, 0x00FF00FF, 0};
    fb.clear(marker);

    hv::HeatMap m = make_map(big); /* grid sized for the *old*, larger area */
    check(m.grid().cols() > hv::map_layout(small).cells.w,
          "stale: the grid really is wider than the area");

    const hv::MapView view{kUnicode};
    view.draw(fb, small, m, 1000);

    int escaped = 0;
    for (int y = 0; y < fb.height(); ++y)
        for (int x = 0; x < fb.width(); ++x) {
            const bool inside = x >= small.x && x < small.x + small.w &&
                                y >= small.y && y < small.y + small.h;
            if (!inside && fb.at_back(x, y) != marker) ++escaped;
        }
    check(escaped == 0, "stale: a map wider than its area is clipped, not spilled");
}

/* --- what a cell looks like ----------------------------------------------- */

/* The view must not invent colours: whatever M3.4 says the cell is, is what
 * lands in the framebuffer. */
void test_cell_colours_come_from_the_ramp() {
    const hv::Rect area{0, 0, 80, 24};

    hv::Framebuffer fb;
    fb.resize(80, 24);
    fb.clear();

    hv::HeatMap m = make_map(area);
    const hv::Grid &g = m.grid();

    /* Three cells in three different states at t = 1000. */
    m.on_alloc(g.base() + g.cell_bytes() * 2, 4096, 4096, 950);  /* fresh   */
    m.on_alloc(g.base() + g.cell_bytes() * 3, 4096, 4096, 0);    /* settled */
    m.on_alloc(g.base() + g.cell_bytes() * 4, 4096, 4096, 0);
    m.on_free (g.base() + g.cell_bytes() * 4, 4096, 4096, 900);  /* freed   */

    const hv::MapView view{kUnicode};
    view.draw(fb, area, m, 1000);

    const hv::MapLayout l = hv::map_layout(area);
    int mismatches = 0, distinct_colours = 0;
    hv::Rgb previous = 0xFFFFFFFFu;

    for (int col = 2; col <= 4; ++col) {
        const hv::Rgb want =
            view.ramp().color(m.at(static_cast<std::size_t>(col)),
                              g.cell_bytes(), 1000);
        const hv::Cell &got = fb.at_back(l.cells.x + col, l.cells.y);
        if (got.fg != want) ++mismatches;
        if (got.fg != previous) { ++distinct_colours; previous = got.fg; }
    }
    check(mismatches == 0, "colour: the cell is what the ramp says it is");
    check(distinct_colours == 3, "colour: the three states really do differ");
}

/* Glyph and colour encode fill independently, because M4.4 degrades one and
 * --no-unicode degrades the other. A ramp that is not monotone in density is
 * not a ramp. */
void test_the_glyph_ramp_is_monotone_in_density() {
    const hv::MapView unicode{kUnicode};
    const hv::MapView ascii{kAscii};

    const hv::GlyphSet u = hv::glyphs_for(kUnicode);
    const hv::GlyphSet a = hv::glyphs_for(kAscii);

    auto weight = [](char32_t glyph, const hv::GlyphSet &set) {
        if (glyph == set.full)   return 3;
        if (glyph == set.medium) return 2;
        if (glyph == set.light)  return 1;
        return 0;
    };

    constexpr std::uint64_t kCell = 4096;
    int last_u = 0, last_a = 0, saw_full = 0;
    bool reached[4] = {false, false, false, false};

    for (std::uint64_t bytes = 0; bytes <= kCell; bytes += 16) {
        hv::CellAggregate agg;
        agg.live_bytes = bytes;
        agg.n_live     = bytes == 0 ? 0u : 1u;

        const int wu = weight(unicode.glyph_for(agg, kCell), u);
        const int wa = weight(ascii.glyph_for(agg, kCell), a);

        check(wu >= 1, "glyph: every cell gets one of the three shades");
        check(wu >= last_u, "glyph: unicode weight never falls as fill rises");
        check(wa >= last_a, "glyph: ascii weight never falls as fill rises");
        check(wu == wa, "glyph: the two sets step at the same thresholds");

        if (wu == 3) ++saw_full;
        if (wu >= 0 && wu <= 3) reached[wu] = true;
        last_u = wu;
        last_a = wa;
    }

    /* Anti-vacuity twice over: a `glyph_for` that always returned `full` would
     * satisfy monotonicity, and one that never did would satisfy it too. */
    check(saw_full > 0, "glyph: a packed cell reaches the full block");

    /* All three shades have to be reachable, or the ramp has two steps wearing
     * three names -- which is monotone, and useless on the fallback terminal
     * where the glyph is the only thing left encoding density. */
    check(reached[1] && reached[2] && reached[3],
          "glyph: every shade is reachable somewhere in the range");
    hv::CellAggregate sliver;
    sliver.live_bytes = 32;
    sliver.n_live     = 1;
    check(weight(unicode.glyph_for(sliver, kCell), u) == 1,
          "glyph: one small chunk is the lightest shade, not a full block");

    /* Empty address space is drawn, not skipped: it is a field, and the colour
     * is what separates untouched from just-freed. */
    hv::CellAggregate nothing;
    check(weight(unicode.glyph_for(nothing, kCell), u) == 1,
          "glyph: unallocated space still gets a glyph");
}

/* --- animation ------------------------------------------------------------ */

/* Without this the map freezes mid-fade until the target happens to allocate
 * again, because M4.5 skips the draw on a frame where nothing reported a
 * change. */
void test_animating_tracks_the_fades() {
    const hv::Rect area{0, 0, 80, 24};

    hv::HeatMap m = make_map(area);
    const hv::Grid &g = m.grid();
    const hv::HeatTimings t = m.timings();

    check(!hv::MapView::animating(m, 1000),
          "animating: an untouched map is still");

    m.on_alloc(g.base() + g.cell_bytes() * 7, 4096, 4096, 1000);
    check(hv::MapView::animating(m, 1000),
          "animating: a fresh allocation is moving");
    check(hv::MapView::animating(m, 1000 + t.malloc_pulse_ms + t.malloc_fade_ms - 1),
          "animating: still moving one ms before the fade ends");
    check(!hv::MapView::animating(m, 1000 + t.malloc_pulse_ms + t.malloc_fade_ms + 1),
          "animating: settled once the fade is over");

    /* And it agrees with what is drawn: a frame the loop would skip must be a
     * frame whose pixels would not have changed. */
    hv::Framebuffer fb;
    fb.resize(80, 24);
    const hv::MapView view{kUnicode};

    const std::uint32_t settled = 1000 + t.malloc_pulse_ms + t.malloc_fade_ms + 50;
    fb.clear();
    view.draw(fb, area, m, settled);
    const hv::Rgb a = fb.at_back(hv::map_layout(area).cells.x + 7,
                                 hv::map_layout(area).cells.y).fg;
    fb.clear();
    view.draw(fb, area, m, settled + 2000);
    const hv::Rgb b = fb.at_back(hv::map_layout(area).cells.x + 7,
                                 hv::map_layout(area).cells.y).fg;
    check(a == b, "animating: a settled cell really does stop changing");
}

/* --- ground rule #5 ------------------------------------------------------- */

/* Nothing in the render path allocates. Buffers are sized on resize, not per
 * frame, and this is the newest code in that path. */
std::size_t g_allocs = 0;
bool        g_counting = false;

void test_draw_allocates_nothing() {
    const hv::Rect area{0, 0, 200, 50};

    hv::Framebuffer fb;
    fb.resize(200, 50);

    hv::HeatMap m = make_map(area);
    const hv::Grid &g = m.grid();
    for (std::size_t i = 0; i < m.cell_count(); i += 3)
        m.on_alloc(g.base() + g.cell_bytes() * i, 2048, 2048,
                   static_cast<std::uint32_t>(i));

    const hv::MapView view{kUnicode};
    view.draw(fb, area, m, 1000); /* warm anything lazy */

    g_allocs = 0;
    g_counting = true;
    for (std::uint32_t frame = 0; frame < 120; ++frame)
        view.draw(fb, area, m, 1000 + frame * 16);
    g_counting = false;

    check(g_allocs == 0, "ground rule 5: drawing 120 frames allocated nothing");
}

} // namespace

/* Counting rather than forbidding, so a failure reports a number instead of
 * aborting somewhere with no context. */
void *operator new(std::size_t n) {
    if (g_counting) ++g_allocs;
    void *p = std::malloc(n == 0 ? 1 : n);
    if (p == nullptr) throw std::bad_alloc{};
    return p;
}
void operator delete(void *p) noexcept { std::free(p); }
void operator delete(void *p, std::size_t) noexcept { std::free(p); }

int main() {
    test_the_grid_is_sized_to_what_is_drawn();
    test_nothing_is_drawn_outside_the_area();
    test_degenerate_areas_draw_nothing_and_do_not_crash();
    test_the_gutter_is_dropped_before_the_map_is();
    test_gutter_labels_name_their_own_row();
    test_the_legend_shows_the_live_granularity();
    test_a_truncated_span_is_reported();
    test_no_bounds_says_so();
    test_the_legend_does_not_spill_out_of_its_rect();
    test_a_map_larger_than_its_area_is_clipped();
    test_cell_colours_come_from_the_ramp();
    test_the_glyph_ramp_is_monotone_in_density();
    test_animating_tracks_the_fades();
    test_draw_allocates_nothing();

    if (g_failures != 0) {
        std::fprintf(stderr, "map_view_test: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("map_view_test: layout, gutter labels, live granularity, the "
                "density ramp and an allocation-free draw all hold\n");
    return 0;
}
