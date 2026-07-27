/* heapviz - double-buffered cell grid (M4.2).
 *
 * Copyright (C) 2026 Parth Sinha
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Two flat grids of cells, front and back. Drawing goes to the back buffer;
 * M4.3 diffs it against the front and emits only what changed, then swap()
 * makes it the new front.
 *
 * Ground rule #5: nothing in the render path allocates. Storage is sized in
 * resize() and reused for every frame after it, so a 200x50 terminal costs one
 * allocation per resize rather than one per frame.
 *
 * Every drawing entry point clips. An off-by-one on a narrow terminal must not
 * become a heap overflow in the tool people run to find heap overflows.
 */

#ifndef HEAPVIZ_TUI_FRAMEBUFFER_H
#define HEAPVIZ_TUI_FRAMEBUFFER_H

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

namespace hv {

/* 0x00RRGGBB. The top byte is unused rather than alpha: terminals do not
 * composite, so there is nothing to blend against. */
using Rgb = std::uint32_t;

enum Attr : std::uint8_t {
    kAttrNone      = 0,
    kAttrBold      = 1u << 0,
    kAttrDim       = 1u << 1,
    kAttrUnderline = 1u << 2,
    kAttrReverse   = 1u << 3,
};

/* One terminal cell, 16 bytes.
 *
 * `attrs` is a byte followed by three bytes of padding, so cells must never be
 * compared or hashed with memcmp: padding is indeterminate and two visually
 * identical cells could differ in it, which in the M4.3 diff would mean
 * redrawing cells that did not change. Use operator==.
 */
struct Cell {
    char32_t      glyph;
    Rgb           fg;
    Rgb           bg;
    std::uint8_t  attrs;
};

static_assert(sizeof(Cell) == 16, "Cell must stay at 16 bytes");
static_assert(alignof(Cell) == 4, "Cell alignment feeds the 16-byte size");
static_assert(offsetof(Cell, glyph) == 0, "Cell layout is pinned");
static_assert(offsetof(Cell, fg)    == 4, "Cell layout is pinned");
static_assert(offsetof(Cell, bg)    == 8, "Cell layout is pinned");
static_assert(offsetof(Cell, attrs) == 12, "Cell layout is pinned");

constexpr bool operator==(const Cell &a, const Cell &b) noexcept {
    return a.glyph == b.glyph && a.fg == b.fg && a.bg == b.bg &&
           a.attrs == b.attrs;
}
constexpr bool operator!=(const Cell &a, const Cell &b) noexcept {
    return !(a == b);
}

constexpr Rgb kDefaultFg = 0x00C0C0C0;
constexpr Rgb kDefaultBg = 0x00000000;

/* What a cleared buffer holds. M6.1 replaces these with real design tokens. */
constexpr Cell empty_cell() noexcept {
    return Cell{U' ', kDefaultFg, kDefaultBg, kAttrNone};
}

/* Half-open in the sense that x + w is one past the last column. Signed so
 * that arithmetic producing a negative origin clips instead of wrapping to a
 * huge unsigned value. */
struct Rect {
    int x = 0, y = 0, w = 0, h = 0;
};

enum class BoxStyle {
    Light,   /* U+2500 family */
    Heavy,   /* U+2501 family */
    Rounded, /* light, with U+256D..U+2570 corners */
    Ascii,   /* +-| for terminals with broken box-drawing fonts */
};

class Framebuffer {
public:
    /* Sizes both buffers and clears them. This is the only call that
     * allocates. Returns false for non-positive dimensions or if the area
     * would overflow; the buffer is left untouched in that case. */
    bool resize(int w, int h);

    int  width()  const noexcept { return w_; }
    int  height() const noexcept { return h_; }
    bool empty()  const noexcept { return w_ <= 0 || h_ <= 0; }

    /* --- drawing, all on the back buffer, all clipped --- */

    void clear(Cell c = empty_cell()) noexcept;
    void put(int x, int y, Cell c) noexcept;

    /* UTF-8 in, cells out. Returns the number of columns actually written,
     * which is less than the string's length when it hit the right edge.
     * Malformed bytes become U+FFFD rather than being skipped, so a decoding
     * bug shows up on screen instead of silently shifting the layout.
     *
     * Assumes every code point is one column wide. True for ASCII and for the
     * block-drawing glyphs the design uses; not true for CJK or emoji, which
     * this tool has no reason to render. */
    int  text(int x, int y, std::string_view utf8, Rgb fg, Rgb bg,
              std::uint8_t attrs = kAttrNone) noexcept;

    void fill(Rect r, Cell c) noexcept;
    void hline(int x, int y, int len, char32_t glyph, Rgb fg, Rgb bg,
               std::uint8_t attrs = kAttrNone) noexcept;
    void vline(int x, int y, int len, char32_t glyph, Rgb fg, Rgb bg,
               std::uint8_t attrs = kAttrNone) noexcept;

    /* Draws the border only; the interior is left alone. Degenerate rects
     * (width or height below 2) draw what fits rather than reaching outside. */
    void box(Rect r, BoxStyle style, Rgb fg, Rgb bg,
             std::uint8_t attrs = kAttrNone) noexcept;

    /* --- buffer access, for the M4.3 differ --- */

    const Cell *front() const noexcept { return front_->data(); }
    const Cell *back()  const noexcept { return back_->data(); }

    const Cell &at_front(int x, int y) const noexcept {
        return (*front_)[index(x, y)];
    }
    const Cell &at_back(int x, int y) const noexcept {
        return (*back_)[index(x, y)];
    }

    /* Call after the frame has been flushed. O(1): swaps two pointers, then
     * clears the new back buffer so the next frame starts from a known state
     * rather than from two frames ago. */
    void swap(Cell fill = empty_cell()) noexcept;

    /* True while (x, y) is inside the grid. */
    bool contains(int x, int y) const noexcept {
        return x >= 0 && y >= 0 && x < w_ && y < h_;
    }

private:
    std::size_t index(int x, int y) const noexcept {
        return static_cast<std::size_t>(y) * static_cast<std::size_t>(w_) +
               static_cast<std::size_t>(x);
    }

    std::vector<Cell>  a_, b_;
    std::vector<Cell> *front_ = &a_;
    std::vector<Cell> *back_  = &b_;
    int w_ = 0, h_ = 0;
};

/* Exposed for the tests and for M4.3, which needs the inverse. Returns the
 * number of bytes consumed, always at least 1 so callers cannot loop forever
 * on malformed input. */
std::size_t utf8_decode(std::string_view s, std::size_t i,
                        char32_t &cp) noexcept;

/* Intersects r with the w x h grid. Returns false when nothing survives. */
bool clip_rect(Rect &r, int w, int h) noexcept;

} // namespace hv

#endif /* HEAPVIZ_TUI_FRAMEBUFFER_H */
