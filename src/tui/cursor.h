/* heapviz - the spatial cursor over the heap map (M5.1).
 *
 * Copyright (C) 2026 Parth Sinha
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Everything M5 inspects is "the thing under the cursor", so this is the object
 * the rest of the milestone hangs off: M5.2's chunk panel reads the cell it
 * names, M5.5's diff mode highlights relative to it. It owns one decision --
 * which cell the user is pointing at -- and nothing else.
 *
 * THE POSITION IS A COORDINATE, NOT A CELL
 * ----------------------------------------
 * The obvious representation is a (row, column) or a flat cell index, and it is
 * wrong for the same reason `Grid` funnels everything through one `configure`:
 * the number of cells is a property of the terminal, and the terminal changes.
 * A cursor holding cell 400 is pointing at a different part of the heap after a
 * resize than it was before, with no event to tell the user their inspection
 * target moved -- which ROADMAP M5.1 asks for explicitly ("keep the address if
 * possible rather than the coordinate").
 *
 * So the stored state is the grid coordinate, and the cell index is derived from
 * it on demand. A resize then reflows the display underneath a cursor that has
 * not moved, which is what a reader expects. The coordinate is what `Grid`
 * buckets, so with a `RegionMap` in play it is a packed offset and with none it
 * is the address itself -- the same duality `HeatMap::coordinate_of` already
 * carries, and deliberately not a second convention.
 *
 * MOVEMENT IS FLAT, NOT PER-ROW
 * -----------------------------
 * `h` at column 0 steps onto the last column of the row above rather than
 * stopping. The map is one linear ribbon of address space wrapped at the
 * terminal's width, so the cell to the left of the first column really is the
 * cell before it; refusing to go there would make the cursor unable to reach
 * addresses that are on screen. Vertical movement keeps the column, because
 * there the wrap is a display artefact rather than an ordering.
 *
 * WHERE THE HEAP ENDS
 * -------------------
 * The grid usually has more cells than the target has heap -- `cell_bytes` is
 * rounded up to a power of two, so the last row is generally part filler. Every
 * move clamps to the last cell that covers real memory rather than to the last
 * cell in the buffer, so `G` and a held-down `l` land on the end of the heap
 * instead of somewhere in the blank tail.
 */

#ifndef HEAPVIZ_TUI_CURSOR_H
#define HEAPVIZ_TUI_CURSOR_H

#include "tui/grid.h"
#include "tui/heatmap.h"

#include <cstddef>
#include <cstdint>

namespace hv {

/* How far the shifted keys jump. Ten rather than a screenful because a screen
 * is 50 rows of 200 cells and a "page" of that is most of the heap: on a real
 * map the useful coarse step is a handful of rows, and `g` / `G` are already
 * there for the ends. */
constexpr std::size_t kCursorJump = 10;

enum class CursorMove : std::uint8_t {
    Left,     /* h */
    Right,    /* l */
    Up,       /* k */
    Down,     /* j */
    LeftFar,  /* H */
    RightFar, /* L */
    UpFar,    /* K */
    DownFar,  /* J */
    Start,    /* g */
    End,      /* G */
    NextLive, /* n */
    PrevLive, /* N */
};

/* The keys above, decoded. Returns false for a byte that means something else,
 * so the caller can keep its own bindings -- `q` reaches this function and must
 * not be swallowed by it. */
bool cursor_move_for_key(char byte, CursorMove &out) noexcept;

/* Last cell of `g` that covers memory the target actually has. Free rather than
 * a method so `Grid` keeps knowing nothing about a cursor. Zero for an invalid
 * grid, which is also its first cell, so callers need no special case. */
std::size_t last_heap_cell(const Grid &g) noexcept;

class MapCursor {
public:
    /* Where in the coordinate space the cursor sits. Always the first byte of
     * whichever cell it last landed on, under the grid that was live at the
     * time -- a finer grid after a resize resolves it to a smaller cell inside
     * the same span, which is the behaviour that keeps the address. */
    std::uint64_t coord() const noexcept { return coord_; }

    /* Cell the cursor is on. Total: an invalid grid answers 0, and a coordinate
     * that has fallen off either end answers the nearest cell rather than
     * failing, so drawing code never has to ask whether the cursor exists. */
    std::size_t index(const Grid &g) const noexcept;

    int row(const Grid &g) const noexcept { return g.row_of(index(g)); }
    int col(const Grid &g) const noexcept { return g.col_of(index(g)); }

    /* Applies one movement. Returns true when the cursor actually moved, which
     * is the caller's redraw signal -- `l` held down at the end of the heap must
     * not repaint the screen sixty times a second. */
    bool move(const HeatMap &map, CursorMove m) noexcept;

    /* Re-clamps after the geometry changed. Returns true if the coordinate had
     * to be pulled back, i.e. the cursor was pointing past the end of a heap
     * that shrank or a grid that lost rows. */
    bool refit(const Grid &g) noexcept;

    /* Puts the cursor on a coordinate directly, snapping to its cell. For M5.2's
     * "jump to this chunk" and for tests. */
    bool set_coord(const Grid &g, std::uint64_t coord) noexcept;

private:
    std::uint64_t coord_ = 0;
};

} // namespace hv

#endif /* HEAPVIZ_TUI_CURSOR_H */
