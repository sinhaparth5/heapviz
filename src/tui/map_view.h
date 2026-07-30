/* heapviz - drawing the spatial heap map (M3.1 legend and gutter).
 *
 * Copyright (C) 2026 Parth Sinha
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * M3.1 buckets the address space, M3.3 aggregates each bucket and M3.4 colours
 * it. This is the piece that puts all three on the screen: a rectangle of cells,
 * an address gutter down the left, and a legend row naming the live granularity.
 *
 * ONE FUNCTION OWNS THE GEOMETRY
 * ------------------------------
 * `Grid::configure` is the only code that computes a granularity, for a reason
 * spelled out in grid.h: two callers computing it their own way would eventually
 * disagree, leaving addresses mapped into one grid while the gutter labels
 * describe another. Drawing extends that hazard by one level. The grid has to be
 * configured with exactly the number of columns and rows that end up being
 * painted, and those two numbers are a function of the rectangle *minus* the
 * gutter and the legend -- which is to say, of a layout decision.
 *
 * So `map_layout` is the single place that decides what fits, `fit_grid` is the
 * only supported way to size a grid for an area, and `draw` reads back the same
 * layout rather than recomputing anything. A gutter that appears in one and not
 * the other does not misalign the display by a column; it labels row 7 with the
 * address of row 6, which is a plausible-looking lie.
 *
 * GLYPH CARRIES DENSITY, COLOUR CARRIES STATE
 * -------------------------------------------
 * Both encode fill: a cell that is nearly full is a full block *and* a brighter
 * blue. That redundancy is the point rather than an oversight. M4.4 degrades
 * colour to a 6x6x6 cube or to sixteen fixed values, and `--no-unicode` degrades
 * the blocks to `#`, `=` and `.`, but the two degrade independently -- so on the
 * terminal where the palette has collapsed, the density ramp is still readable,
 * and on the terminal with no block-drawing font, the colour still is.
 *
 * EVERY CELL, EVERY FRAME
 * -----------------------
 * `draw` paints the whole map on every frame it is called for, with no attempt
 * to touch only the cells that changed. That is affordable because M3.4 made it
 * so: a settled cell costs 6.7 ns, so a 200x50 map is 67 us against a 1 ms
 * budget. The M4.3 differ is what turns those repainted cells back into no
 * output when nothing actually moved, and it does that by comparing cells rather
 * than by trusting the drawing code -- which is why the drawing code does not
 * need to be clever, and why a frame in which one cell aged still costs one
 * cell's worth of bytes on the wire.
 *
 * Ground rule #5 holds here: `draw` allocates nothing.
 */

#ifndef HEAPVIZ_TUI_MAP_VIEW_H
#define HEAPVIZ_TUI_MAP_VIEW_H

#include "tui/capabilities.h"
#include "tui/cursor.h"
#include "tui/framebuffer.h"
#include "tui/grid.h"
#include "tui/heat_color.h"
#include "tui/heatmap.h"
#include "tui/snapshot.h"

#include <cstdint>

namespace hv {

/* Below this the map is a novelty rather than a display, and the gutter is
 * costing more columns than it explains. Narrower than this and the gutter is
 * dropped so the cells get the whole width. */
constexpr int kMinMapCols = 8;

/* Where each part of the map lands inside the area it was given.
 *
 * `legend.h` is 0 when there was no room for it, and `gutter_x` is negative
 * when the gutter was dropped. Both are ordinary outcomes on a small terminal,
 * not errors: `size_is_usable` already refuses the sizes where this would be
 * illegible, so what is left is a window being dragged. */
struct MapLayout {
    Rect legend{};
    Rect cells{};
    int  gutter_x = -1;
    bool valid    = false;
};

MapLayout map_layout(Rect area) noexcept;

/* Sizes `g` for the cells `area` leaves room for, keeping its current bounds.
 * The only supported way to prepare a grid for `MapView::draw`. False when the
 * area holds no map, or when the bounds are degenerate -- see Grid::configure,
 * which treats both as normal operation. */
bool fit_grid(Grid &g, Rect area) noexcept;

/* The colours that are not the heat palette: the chrome around the map. M6.1's
 * theme supplies these; the defaults are its tokens. */
struct MapStyle {
    Rgb ink    = 0x00D8D8D8; /* text  */
    Rgb dim    = 0x007A7A7A; /* label */
    Rgb warn   = 0x00F5A623; /* accent, used for the truncation notice */
    Rgb bg     = 0x000C0C0C; /* canvas */
    Rgb cursor = 0x0033D7E8; /* M6.1's `cursor` token */

    /* M5.5's diff mode. Magenta because it is the one hue the heat palette does
     * not use: fresh green, freed red, settled blue, overhead yellow and empty
     * grey are all spoken for, and an overlay in a colour the map already means
     * something by would be read as a heat state rather than as a mode. */
    Rgb leak   = 0x00E040FB;
};

class MapView {
public:
    explicit MapView(Capabilities caps, const HeatPalette &p = kDefaultPalette,
                     const HeatTimings &t = kDefaultTimings) noexcept;

    void             set_style(const MapStyle &s) noexcept { style_ = s; }
    const MapStyle  &style() const noexcept { return style_; }
    const HeatRamp  &ramp()  const noexcept { return ramp_; }

    /* Paints legend, gutter and cells into `area`. Nothing is written outside
     * it, including when the map has more cells than the area can hold: the
     * grid should have been sized by `fit_grid`, but a caller that skipped that
     * gets a clipped map rather than a corrupted framebuffer. */
    void draw(Framebuffer &fb, Rect area, const HeatMap &map,
              std::uint32_t now_ms) const noexcept;

    /* Paints M5.1's cursor over a map already drawn into the same `area`.
     *
     * A second pass rather than a branch inside `draw`'s per-cell loop: the
     * inner loop runs tens of thousands of times a frame against a 1 ms budget,
     * and a compare that is true for exactly one of those iterations does not
     * belong in it.
     *
     * It goes through `MapView` rather than being drawn by the caller because
     * `map_layout` is the only code that knows where the cells landed. A cursor
     * positioned from the caller's own idea of the geometry is M3.1's gutter bug
     * again -- the highlight would sit on a neighbouring cell and look entirely
     * plausible while naming the wrong address.
     *
     * No-op when the grid is invalid or the cursor's cell is off the drawn
     * area, which a caller that has not refit after a resize can produce. */
    void draw_cursor(Framebuffer &fb, Rect area, const HeatMap &map,
                     const MapCursor &cur) const noexcept;

    /* Repaints M5.5's leak candidates over a map already drawn into the same
     * `area`, in `MapStyle::leak`.
     *
     * A third pass, for `draw_cursor`'s reason: the per-cell loop runs tens of
     * thousands of times a frame against a 1 ms budget, and diff mode is off in
     * almost every session. This walks the overlay's non-zero cells instead, so
     * a map with forty leak candidates costs forty writes rather than ten
     * thousand branches.
     *
     * The glyph is kept and only the foreground changes. Density is what the
     * glyph encodes (see the header), and a mode that flattened every
     * highlighted cell to one block would answer "where are the candidates"
     * while destroying "how much is there" -- which is the next question anyone
     * asks. It runs before `draw_cursor` so the cursor stays visible on a cell
     * that is also a candidate.
     *
     * No-op when the grid is invalid, when diff mode is off, or when the
     * overlay was computed against a different geometry -- the last of which a
     * caller that has not re-analyzed after a resize can produce. */
    void draw_leaks(Framebuffer &fb, Rect area, const HeatMap &map,
                    const SnapshotDiff &diff) const noexcept;

    /* Which of the four density glyphs a cell earns. Public because it is half
     * of what the display encodes, and a test that only checked colour would
     * miss a fallback terminal showing a flat field of blocks. */
    char32_t glyph_for(const CellAggregate &a,
                       std::uint64_t cell_bytes) const noexcept;

    /* True while any cell is still inside a flash or fade window, i.e. while
     * the next frame would differ from this one with no new events at all.
     * This is what a `LoopApp::animating` should return: without it the map
     * freezes mid-fade until the target happens to allocate again. */
    static bool animating(const HeatMap &map, std::uint32_t now_ms) noexcept;

private:
    void draw_legend(Framebuffer &fb, Rect r, const Grid &g) const noexcept;

    GlyphSet    glyphs_;
    HeatRamp    ramp_;
    HeatPalette palette_;
    MapStyle    style_{};
};

} // namespace hv

#endif /* HEAPVIZ_TUI_MAP_VIEW_H */
