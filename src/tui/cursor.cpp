/* heapviz - the spatial cursor over the heap map (M5.1).
 *
 * Copyright (C) 2026 Parth Sinha
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "tui/cursor.h"

namespace hv {
namespace {

/* First coordinate of cell `i`. The inverse of `Grid::index_of` for a cell
 * start, which is the only place this is ever asked for -- the cursor snaps to
 * cell boundaries so that moving n cells and back returns to where it began
 * rather than drifting by the sub-cell remainder each time. */
std::uint64_t coord_of_cell(const Grid &g, std::size_t i) noexcept {
    return g.base() + (static_cast<std::uint64_t>(i) << g.log2_cell_bytes());
}

} // namespace

std::size_t last_heap_cell(const Grid &g) noexcept {
    const std::size_t n = g.cell_count();
    if (!g.valid() || n == 0) return 0;

    /* `span` is at least 1 for a valid grid (configure rejects end <= base), so
     * the decrement is safe and the result is the cell holding the last byte the
     * target has rather than the last cell on screen. Those differ by up to
     * a row: cell_bytes is rounded up to a power of two. */
    const std::uint64_t last = (g.span() - 1) >> g.log2_cell_bytes();
    if (last >= n) return n - 1; /* span wider than the grid covers */
    return static_cast<std::size_t>(last);
}

bool cursor_move_for_key(char byte, CursorMove &out) noexcept {
    switch (byte) {
    case 'h': out = CursorMove::Left;     return true;
    case 'l': out = CursorMove::Right;    return true;
    case 'k': out = CursorMove::Up;       return true;
    case 'j': out = CursorMove::Down;     return true;
    case 'H': out = CursorMove::LeftFar;  return true;
    case 'L': out = CursorMove::RightFar; return true;
    case 'K': out = CursorMove::UpFar;    return true;
    case 'J': out = CursorMove::DownFar;  return true;
    case 'g': out = CursorMove::Start;    return true;
    case 'G': out = CursorMove::End;      return true;
    case 'n': out = CursorMove::NextLive; return true;
    case 'N': out = CursorMove::PrevLive; return true;
    default: return false;
    }
}

std::size_t MapCursor::index(const Grid &g) const noexcept {
    if (!g.valid()) return 0;

    const std::size_t last = last_heap_cell(g);

    std::size_t i = 0;
    if (!g.index_of(coord_, i)) {
        /* Below the base or past what the grid covers. Both happen for real:
         * the bounds move when the target's heap grows, and a coordinate that
         * was inside the old range can be outside the new one. Nearest end
         * rather than a failure -- a cursor that vanished on a heap growth
         * would be a worse answer than one that slid to the edge. */
        return coord_ <= g.base() ? 0 : last;
    }
    return i < last ? i : last;
}

bool MapCursor::move(const HeatMap &map, CursorMove m) noexcept {
    const Grid &g = map.grid();
    if (!g.valid() || g.cols() <= 0) return false;

    const std::size_t last = last_heap_cell(g);
    const std::size_t from = index(g);
    const auto cols = static_cast<std::size_t>(g.cols());
    std::size_t to = from;

    /* Every arm saturates rather than wrapping. `to` is unsigned, so a
     * subtraction that ran past zero would put the cursor at the far end of the
     * heap -- the single most confusing thing a cursor can do. */
    switch (m) {
    case CursorMove::Left:
        if (from > 0) to = from - 1;
        break;
    case CursorMove::Right:
        if (from < last) to = from + 1;
        break;
    case CursorMove::LeftFar:
        to = from > kCursorJump ? from - kCursorJump : 0;
        break;
    case CursorMove::RightFar:
        to = last - from > kCursorJump ? from + kCursorJump : last;
        break;

    /* Vertical movement keeps the column, and clamps to the first or last row
     * that has one rather than refusing when there are fewer than the jump
     * distance left. `K` near the top means "go up", not "beep". */
    case CursorMove::Up:
    case CursorMove::UpFar: {
        const std::size_t step = (m == CursorMove::Up) ? 1 : kCursorJump;
        const std::size_t rows_above = from / cols;
        to = from - cols * (step < rows_above ? step : rows_above);
        break;
    }
    case CursorMove::Down:
    case CursorMove::DownFar: {
        const std::size_t step = (m == CursorMove::Down) ? 1 : kCursorJump;
        const std::size_t rows_below = (last - from) / cols;
        to = from + cols * (step < rows_below ? step : rows_below);
        break;
    }

    case CursorMove::Start:
        to = 0;
        break;
    case CursorMove::End:
        to = last;
        break;

    /* `n` / `N` are what make a sparse map navigable: on a 40 MiB heap at 16 KiB
     * a cell, most cells hold nothing, and stepping to the next allocation by
     * hand is hundreds of keypresses. Finding nothing leaves the cursor where it
     * was and reports no change, so the display does not repaint. */
    case CursorMove::NextLive: {
        const std::size_t n = map.cell_count();
        const std::size_t end = last < n ? last : (n == 0 ? 0 : n - 1);
        for (std::size_t i = from + 1; i <= end && n != 0; ++i)
            if (cell_occupied(map.at(i))) { to = i; break; }
        break;
    }
    case CursorMove::PrevLive: {
        const std::size_t n = map.cell_count();
        for (std::size_t i = from; i-- > 0 && n != 0;)
            if (i < n && cell_occupied(map.at(i))) { to = i; break; }
        break;
    }
    }

    if (to == from) return false;
    coord_ = coord_of_cell(g, to);
    return true;
}

bool MapCursor::refit(const Grid &g) noexcept {
    if (!g.valid()) return false;

    /* Deliberately not `coord_ = coord_of_cell(g, index(g))` unconditionally:
     * that would snap the coordinate to the new grid's cell start on every
     * resize, and a run of resizes at coarsening granularities would walk the
     * cursor backwards a cell at a time. Only a coordinate that no longer lands
     * anywhere is moved. */
    std::size_t i = 0;
    if (g.index_of(coord_, i) && i <= last_heap_cell(g)) return false;

    coord_ = coord_of_cell(g, index(g));
    return true;
}

bool MapCursor::set_coord(const Grid &g, std::uint64_t coord) noexcept {
    if (!g.valid()) return false;

    const std::uint64_t was = coord_;
    coord_ = coord;
    coord_ = coord_of_cell(g, index(g)); /* snap, and clamp through index() */
    return coord_ != was;
}

} // namespace hv
