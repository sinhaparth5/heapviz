/* heapviz - drawing the spatial heap map (M3.1 legend and gutter).
 *
 * Copyright (C) 2026 Parth Sinha
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "tui/map_view.h"

#include <cstdio>
#include <string_view>

namespace hv {
namespace {

/* The two density thresholds. They are not evenly spaced: the interesting
 * distinction on a real heap is between "this cell holds something" and "this
 * cell is packed", and an even split puts both boundaries where almost no cell
 * ever sits. A quarter full already reads as occupied. */
constexpr float kFullFrom   = 0.66f;
constexpr float kMediumFrom = 0.25f;

/* A left-to-right cursor that stops at the right edge of the legend rather than
 * at the right edge of the framebuffer.
 *
 * `Framebuffer::text` clips, but it clips to the buffer, so a legend drawn in a
 * panel would spill across whatever sits beside it. Segments are dropped whole
 * rather than truncated: half a word looks like a bug, and the legend is a list
 * of independent items where losing the last one costs nothing. */
class Pen {
public:
    Pen(Framebuffer &fb, Rect r) noexcept
        : fb_(fb), x_(r.x), y_(r.y), limit_(r.x + r.w) {}

    void text(std::string_view s, Rgb fg, Rgb bg,
              std::uint8_t attrs = kAttrNone) noexcept {
        const auto len = static_cast<int>(s.size()); /* ASCII only, see below */
        if (x_ + len > limit_) { x_ = limit_; return; }
        x_ += fb_.text(x_, y_, s, fg, bg, attrs);
    }

    /* Separate from `text` because the swatches are block-drawing characters:
     * one glyph is three UTF-8 bytes, so a byte count would misplace everything
     * after it. */
    void glyph(char32_t g, Rgb fg, Rgb bg) noexcept {
        if (x_ >= limit_) return;
        fb_.put(x_, y_, Cell{g, fg, bg, kAttrNone});
        ++x_;
    }

    bool room_for(int cols) const noexcept { return x_ + cols <= limit_; }

private:
    Framebuffer &fb_;
    int x_, y_, limit_;
};

} // namespace

MapLayout map_layout(Rect area) noexcept {
    MapLayout out;
    if (area.w <= 0 || area.h <= 0) return out;

    /* The legend is the first thing to go, because a map with no legend is
     * still a map. It only survives if a row is left for cells underneath. */
    const bool legend = area.h >= 2;
    if (legend) out.legend = Rect{area.x, area.y, area.w, 1};

    const int cells_y = area.y + (legend ? 1 : 0);
    const int cells_h = area.h - (legend ? 1 : 0);

    const int gutter_cols = static_cast<int>(kGutterWidth) + 1; /* label + gap */
    const bool gutter = area.w >= gutter_cols + kMinMapCols;

    out.gutter_x = gutter ? area.x : -1;
    out.cells    = Rect{area.x + (gutter ? gutter_cols : 0), cells_y,
                        area.w - (gutter ? gutter_cols : 0), cells_h};
    out.valid    = out.cells.w > 0 && out.cells.h > 0;
    return out;
}

bool fit_grid(Grid &g, Rect area) noexcept {
    const MapLayout l = map_layout(area);
    if (!l.valid) {
        /* Still tell the grid, so it goes invalid rather than answering
         * queries for a viewport that is no longer on screen. */
        g.set_viewport(0, 0);
        return false;
    }
    return g.set_viewport(l.cells.w, l.cells.h);
}

MapView::MapView(Capabilities caps, const HeatPalette &p,
                 const HeatTimings &t) noexcept
    : glyphs_(glyphs_for(caps)), ramp_(p, t), palette_(p) {}

char32_t MapView::glyph_for(const CellAggregate &a,
                            std::uint64_t cell_bytes) const noexcept {
    /* Nothing here at all: the faintest shade, so unallocated address space
     * reads as a field rather than as a hole. The colour says which of the two
     * kinds of nothing it is -- untouched, or freed a moment ago. */
    if (a.n_live == 0 && a.overhead_bytes == 0) return glyphs_.light;
    if (cell_bytes == 0) return glyphs_.full; /* no density information */

    const float d = static_cast<float>(a.live_bytes) /
                    static_cast<float>(cell_bytes);
    if (d >= kFullFrom)   return glyphs_.full;
    if (d >= kMediumFrom) return glyphs_.medium;
    return glyphs_.light;
}

bool MapView::animating(const HeatMap &map, std::uint32_t now_ms) noexcept {
    const HeatTimings &t = map.timings();
    const std::size_t n = map.cell_count();
    for (std::size_t i = 0; i < n; ++i)
        if (cell_animating(map.at(i), t, now_ms)) return true;
    return false;
}

void MapView::draw_legend(Framebuffer &fb, Rect r, const Grid &g) const noexcept {
    if (r.w <= 0 || r.h <= 0) return;

    Pen pen(fb, r);

    /* The live figure, never a constant: it changes under a resize and under
     * the target's heap growing, and a legend that lagged either would be
     * describing a grid that no longer exists (ROADMAP M3.1). */
    if (!g.valid()) {
        pen.text("no heap bounds yet", style_.dim, style_.bg);
        return;
    }

    char size[24];
    format_byte_size(size, sizeof size, g.cell_bytes());

    char head[48];
    std::snprintf(head, sizeof head, "1 cell = %s", size);
    pen.text(head, style_.ink, style_.bg);

    struct Swatch { Rgb color; const char *label; };
    const Swatch swatches[] = {
        {palette_.settled,  " live"},
        {palette_.fresh,    " new"},
        {palette_.freed,    " freed"},
        {palette_.overhead, " overhead"},
        {palette_.empty,    " unused"},
    };
    for (const Swatch &s : swatches) {
        const auto label_len = static_cast<int>(std::string_view{s.label}.size());
        if (!pen.room_for(2 + 1 + label_len)) break;
        pen.text("  ", style_.dim, style_.bg);
        pen.glyph(glyphs_.full, s.color, style_.bg);
        pen.text(s.label, style_.dim, style_.bg);
    }

    /* A span too wide for any granularity on screen loses its top, and the
     * README's promise is that heapviz says what it missed rather than showing
     * a fraction of the heap and calling it the heap. */
    if (!g.covers_whole_span())
        pen.text("   top of range not shown", style_.warn, style_.bg);
}

void MapView::draw(Framebuffer &fb, Rect area, const HeatMap &map,
                   std::uint32_t now_ms) const noexcept {
    const MapLayout l = map_layout(area);
    if (!l.valid) return;

    const Grid &g = map.grid();
    draw_legend(fb, l.legend, g);
    if (!g.valid()) return;

    /* The grid should have been sized by `fit_grid`, but a caller that drew
     * before reconfiguring gets a clipped map rather than cells landing in
     * whatever sits below this rectangle. */
    const int rows = g.rows() < l.cells.h ? g.rows() : l.cells.h;
    const int cols = g.cols() < l.cells.w ? g.cols() : l.cells.w;
    const std::uint64_t cell_bytes = g.cell_bytes();

    char label[kGutterWidth + 1];

    for (int row = 0; row < rows; ++row) {
        const int y = l.cells.y + row;

        if (l.gutter_x >= 0) {
            format_offset(label, sizeof label, g.offset_of_row(row));
            fb.text(l.gutter_x, y, label, style_.dim, style_.bg);
        }

        const auto base = static_cast<std::size_t>(row) *
                          static_cast<std::size_t>(g.cols());
        for (int col = 0; col < cols; ++col) {
            const std::size_t i = base + static_cast<std::size_t>(col);
            if (i >= map.cell_count()) return;

            const CellAggregate &a = map.at(i);
            fb.put(l.cells.x + col, y,
                   Cell{glyph_for(a, cell_bytes),
                        ramp_.color(a, cell_bytes, now_ms), style_.bg,
                        kAttrNone});
        }
    }
}

} // namespace hv
