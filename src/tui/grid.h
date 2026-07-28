/* heapviz - address space to grid bucketizer (M3.1).
 *
 * Copyright (C) 2026 Parth Sinha
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * A terminal has a few thousand cells and a target has a 64-bit address space,
 * so every cell stands for a span of addresses. This decides how wide that span
 * is, converts an address into a cell, and formats the two numbers the user
 * needs to read the result: the granularity in the legend and the offset labels
 * down the left gutter.
 *
 * The span per cell is always a power of two, which makes address -> cell a
 * shift rather than a divide. That matters because the conversion runs once per
 * drained event, not once per frame: at ten million allocations a second a
 * 64-bit division on the hot path is not a rounding error in the budget.
 *
 * WHY ONE `configure`
 * -------------------
 * Two things move the geometry: a SIGWINCH changing the terminal, and the
 * target's heap growing. They arrive from unrelated places at unrelated times,
 * and if each recomputed the granularity its own way they would eventually
 * disagree, leaving addresses mapping to one grid while the gutter labels
 * describe another. `set_viewport` and `set_bounds` therefore both funnel into
 * `configure`, which is the only code that computes anything.
 */

#ifndef HEAPVIZ_TUI_GRID_H
#define HEAPVIZ_TUI_GRID_H

#include <cstddef>
#include <cstdint>

namespace hv {

/* A cell smaller than this is finer than the allocator's own granularity: the
 * smallest glibc chunk is 32 bytes and everything is 16-byte aligned, so cells
 * below 64 bytes would mostly be structurally empty and would spend screen
 * space saying nothing. */
constexpr std::uint64_t kMinCellBytes = 64;

/* And an upper bound so the shift stays sane and the legend keeps meaning
 * something. See `covers_whole_span` for what happens when a span needs more
 * than this. */
constexpr std::uint64_t kMaxCellBytes = std::uint64_t{1} << 30; /* 1 GiB */

/* Width of a formatted gutter label, in columns, including the unit. Fixed so
 * the map does not shift sideways when the heap crosses a power of ten. */
constexpr std::size_t kGutterWidth = 7;

class Grid {
public:
    /* Recomputes everything from the heap bounds and the viewport. Returns
     * false and leaves the grid invalid for a degenerate input: an empty or
     * inverted span, or a viewport with no cells in it (a one-column terminal
     * mid-drag is a real source of both). An invalid grid answers every query
     * safely rather than dividing by zero or shifting by a negative amount.
     *
     * `end` is exclusive. */
    bool configure(std::uint64_t base, std::uint64_t end, int cols,
                   int rows) noexcept;

    /* The two callers, kept separate so each site reads as what it is, but both
     * are `configure` underneath and cannot drift apart. */
    bool set_bounds(std::uint64_t base, std::uint64_t end) noexcept;
    bool set_viewport(int cols, int rows) noexcept;

    bool valid() const noexcept { return valid_; }

    std::uint64_t base()       const noexcept { return base_; }
    std::uint64_t end()        const noexcept { return end_; }
    std::uint64_t span()       const noexcept { return end_ - base_; }
    std::uint64_t cell_bytes() const noexcept { return cell_bytes_; }
    unsigned      log2_cell_bytes() const noexcept { return log2_cell_bytes_; }

    int         cols()       const noexcept { return cols_; }
    int         rows()       const noexcept { return rows_; }
    std::size_t cell_count() const noexcept;

    /* addr -> flat cell index. False when the address is below the base or
     * beyond what the grid covers, which is not an error: a target allocates
     * outside the region heapviz is currently displaying all the time. */
    bool index_of(std::uint64_t addr, std::size_t &out) const noexcept;

    /* Flat index -> row and column. Only meaningful for an index that
     * `index_of` produced. */
    int row_of(std::size_t index) const noexcept;
    int col_of(std::size_t index) const noexcept;

    /* First address covered by a row, and its offset from `base`. These are
     * what the gutter labels, so they are defined for `row` in [0, rows). */
    std::uint64_t addr_of_row(int row) const noexcept;
    std::uint64_t offset_of_row(int row) const noexcept;

    /* True when cell_bytes * cell_count reaches the whole span.
     *
     * It can be false: a span wider than kMaxCellBytes * cell_count has no
     * granularity that fits on screen, and clamping means the top of the range
     * falls off the grid. heapviz says so rather than silently showing a
     * fraction of the heap and calling it the heap. M2 makes this rare by
     * tracking the target's regions separately instead of spanning the gap
     * between a brk heap at 0x55... and an mmap region at 0x7f..., which is
     * where a 47-bit span comes from in the first place. */
    bool covers_whole_span() const noexcept;

private:
    std::uint64_t base_ = 0;
    std::uint64_t end_  = 0;
    std::uint64_t cell_bytes_ = kMinCellBytes;
    unsigned      log2_cell_bytes_ = 6;
    int  cols_  = 0;
    int  rows_  = 0;
    bool valid_ = false;
};

/* "256 B", "4 KB", "1 GB" - the value shown in the legend as
 * `(1 cell = 256 B)`. Always the live figure; a constant there would be a lie
 * the moment the window is resized. Returns the length written. */
std::size_t format_byte_size(char *buf, std::size_t n,
                             std::uint64_t bytes) noexcept;

/* A gutter label: an offset from the heap base, right-aligned to exactly
 * kGutterWidth columns, auto-scaled to B / KB / MB / GB / TB.
 *
 * `buf` must hold kGutterWidth + 1 bytes. Runs once per row per frame rather
 * than once per cell, which is why it can afford snprintf where the renderer
 * could not. */
std::size_t format_offset(char *buf, std::size_t n,
                          std::uint64_t offset) noexcept;

} // namespace hv

#endif /* HEAPVIZ_TUI_GRID_H */
