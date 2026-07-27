/* heapviz - double-buffered cell grid (M4.2).
 *
 * Copyright (C) 2026 Parth Sinha
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "tui/framebuffer.h"

#include <algorithm>

namespace hv {

namespace {

/* A 200x50 terminal is 10k cells. This cap exists only so that a bogus
 * ioctl(TIOCGWINSZ) result cannot turn into a gigabyte allocation. */
constexpr std::size_t kMaxCells = 4u << 20;

constexpr char32_t kReplacement = U'�';

struct BoxGlyphs {
    char32_t h, v, tl, tr, bl, br;
};

constexpr BoxGlyphs glyphs_for(BoxStyle s) noexcept {
    switch (s) {
    case BoxStyle::Light:
        return {U'─', U'│', U'┌', U'┐', U'└', U'┘'};
    case BoxStyle::Heavy:
        return {U'━', U'┃', U'┏', U'┓', U'┗', U'┛'};
    case BoxStyle::Rounded:
        return {U'─', U'│', U'╭', U'╮', U'╰', U'╯'};
    case BoxStyle::Ascii:
        return {U'-', U'|', U'+', U'+', U'+', U'+'};
    }
    return {U'-', U'|', U'+', U'+', U'+', U'+'};
}

} // namespace

std::size_t utf8_decode(std::string_view s, std::size_t i, char32_t &cp) noexcept {
    const std::size_t n = s.size();
    if (i >= n) { cp = kReplacement; return 1; }

    const auto b0 = static_cast<unsigned char>(s[i]);

    if (b0 < 0x80u) { cp = static_cast<char32_t>(b0); return 1; }

    /* A continuation byte where a leading byte belongs, or one of the two
     * bytes that never appear in valid UTF-8. */
    if (b0 < 0xC2u || b0 > 0xF4u) { cp = kReplacement; return 1; }

    const auto cont = [&](std::size_t k) -> bool {
        return i + k < n &&
               (static_cast<unsigned char>(s[i + k]) & 0xC0u) == 0x80u;
    };
    const auto bits = [&](std::size_t k) -> char32_t {
        return static_cast<char32_t>(static_cast<unsigned char>(s[i + k]) & 0x3Fu);
    };

    if (b0 < 0xE0u) { /* 2 bytes; b0 >= 0xC2 already rules out overlong */
        if (!cont(1)) { cp = kReplacement; return 1; }
        cp = (static_cast<char32_t>(b0 & 0x1Fu) << 6) | bits(1);
        return 2;
    }

    if (b0 < 0xF0u) { /* 3 bytes */
        if (!cont(1) || !cont(2)) { cp = kReplacement; return 1; }
        const char32_t v = (static_cast<char32_t>(b0 & 0x0Fu) << 12) |
                           (bits(1) << 6) | bits(2);
        /* Overlong, or a UTF-16 surrogate that has no business here. */
        if (v < 0x800u || (v >= 0xD800u && v <= 0xDFFFu)) {
            cp = kReplacement;
            return 1;
        }
        cp = v;
        return 3;
    }

    /* 4 bytes */
    if (!cont(1) || !cont(2) || !cont(3)) { cp = kReplacement; return 1; }
    const char32_t v = (static_cast<char32_t>(b0 & 0x07u) << 18) |
                       (bits(1) << 12) | (bits(2) << 6) | bits(3);
    if (v < 0x10000u || v > 0x10FFFFu) { cp = kReplacement; return 1; }
    cp = v;
    return 4;
}

bool clip_rect(Rect &r, int w, int h) noexcept {
    if (r.w <= 0 || r.h <= 0 || w <= 0 || h <= 0) return false;

    /* 64-bit so that a rect starting near INT_MAX cannot wrap. */
    long long x0 = r.x, y0 = r.y;
    long long x1 = static_cast<long long>(r.x) + r.w;
    long long y1 = static_cast<long long>(r.y) + r.h;

    x0 = std::max<long long>(x0, 0);
    y0 = std::max<long long>(y0, 0);
    x1 = std::min<long long>(x1, w);
    y1 = std::min<long long>(y1, h);

    if (x1 <= x0 || y1 <= y0) return false;

    r.x = static_cast<int>(x0);
    r.y = static_cast<int>(y0);
    r.w = static_cast<int>(x1 - x0);
    r.h = static_cast<int>(y1 - y0);
    return true;
}

bool Framebuffer::resize(int w, int h) {
    if (w <= 0 || h <= 0) return false;

    const auto cells = static_cast<std::size_t>(w) * static_cast<std::size_t>(h);
    if (cells > kMaxCells) return false;

    /* assign() on an already-correctly-sized vector reuses the storage, so a
     * SIGWINCH that reports the same dimensions costs nothing. */
    a_.assign(cells, empty_cell());
    b_.assign(cells, empty_cell());
    w_ = w;
    h_ = h;
    return true;
}

void Framebuffer::clear(Cell c) noexcept {
    std::fill(back_->begin(), back_->end(), c);
}

void Framebuffer::put(int x, int y, Cell c) noexcept {
    if (!contains(x, y)) return;
    (*back_)[index(x, y)] = c;
}

int Framebuffer::text(int x, int y, std::string_view utf8, Rgb fg, Rgb bg,
                      std::uint8_t attrs) noexcept {
    if (y < 0 || y >= h_ || w_ <= 0) return 0;

    int col     = x;
    int written = 0;
    for (std::size_t i = 0; i < utf8.size() && col < w_;) {
        char32_t cp = 0;
        i += utf8_decode(utf8, i, cp);
        if (col >= 0) {
            (*back_)[index(col, y)] = Cell{cp, fg, bg, attrs};
            ++written;
        }
        ++col;
    }
    return written;
}

void Framebuffer::fill(Rect r, Cell c) noexcept {
    if (!clip_rect(r, w_, h_)) return;
    for (int y = r.y; y < r.y + r.h; ++y) {
        Cell *row = back_->data() + index(0, y);
        std::fill(row + r.x, row + r.x + r.w, c);
    }
}

void Framebuffer::hline(int x, int y, int len, char32_t glyph, Rgb fg, Rgb bg,
                        std::uint8_t attrs) noexcept {
    fill(Rect{x, y, len, 1}, Cell{glyph, fg, bg, attrs});
}

void Framebuffer::vline(int x, int y, int len, char32_t glyph, Rgb fg, Rgb bg,
                        std::uint8_t attrs) noexcept {
    fill(Rect{x, y, 1, len}, Cell{glyph, fg, bg, attrs});
}

void Framebuffer::box(Rect r, BoxStyle style, Rgb fg, Rgb bg,
                      std::uint8_t attrs) noexcept {
    if (r.w <= 0 || r.h <= 0 || empty()) return;

    const BoxGlyphs g = glyphs_for(style);
    const auto cell   = [&](char32_t ch) { return Cell{ch, fg, bg, attrs}; };

    const long long x0 = r.x;
    const long long y0 = r.y;
    const long long x1 = static_cast<long long>(r.x) + r.w - 1;
    const long long y1 = static_cast<long long>(r.y) + r.h - 1;

    /* Clamp the edge loops to the visible span. put() would clip anyway, but a
     * rect two million columns wide would otherwise spin through two million
     * rejected calls. */
    const long long xs = std::max<long long>(x0 + 1, 0);
    const long long xe = std::min<long long>(x1 - 1, w_ - 1);
    for (long long x = xs; x <= xe; ++x) {
        put(static_cast<int>(x), static_cast<int>(y0), cell(g.h));
        if (y1 != y0) put(static_cast<int>(x), static_cast<int>(y1), cell(g.h));
    }

    const long long ys = std::max<long long>(y0 + 1, 0);
    const long long ye = std::min<long long>(y1 - 1, h_ - 1);
    for (long long y = ys; y <= ye; ++y) {
        put(static_cast<int>(x0), static_cast<int>(y), cell(g.v));
        if (x1 != x0) put(static_cast<int>(x1), static_cast<int>(y), cell(g.v));
    }

    /* Corners last so they win over the edges on a 2-wide or 2-tall box.
     * The guards collapse the corner set for degenerate rects rather than
     * writing the same cell four times with different glyphs. */
    const bool wide = (x1 != x0);
    const bool tall = (y1 != y0);
    put(static_cast<int>(x0), static_cast<int>(y0), cell(g.tl));
    if (wide)         put(static_cast<int>(x1), static_cast<int>(y0), cell(g.tr));
    if (tall)         put(static_cast<int>(x0), static_cast<int>(y1), cell(g.bl));
    if (wide && tall) put(static_cast<int>(x1), static_cast<int>(y1), cell(g.br));
}

void Framebuffer::swap(Cell fill_with) noexcept {
    std::swap(front_, back_);
    std::fill(back_->begin(), back_->end(), fill_with);
}

} // namespace hv
