/* heapviz - framebuffer checks (M4.2).
 *
 * Copyright (C) 2026 Parth Sinha
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Two properties matter more than the drawing itself.
 *
 * Clipping: this tool exists to find heap overflows, so an off-by-one in its
 * own renderer writing past the end of a cell buffer would be embarrassing in
 * a specific way. Every drawing entry point is therefore given coordinates
 * that are negative, past the edge, and large enough to overflow int
 * arithmetic.
 *
 * No allocation while drawing (ground rule #5): the test counts calls to
 * global operator new across a full frame's worth of drawing and requires
 * zero.
 */

#include "tui/framebuffer.h"

#include <atomic>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <string>

/* --- allocation counter ----------------------------------------------------
 * ASan replaces global operator new itself; defining our own on top of it is
 * asking for trouble, and the property under test is about our code rather
 * than about the allocator, so the counter is simply absent there. */
#ifndef __SANITIZE_ADDRESS__
#define HV_COUNT_ALLOCS 1
namespace { std::atomic<long> g_allocs{0}; }

void *operator new(std::size_t n) {
    g_allocs.fetch_add(1, std::memory_order_relaxed);
    void *p = std::malloc(n != 0 ? n : 1);
    if (p == nullptr) throw std::bad_alloc();
    return p;
}
void *operator new[](std::size_t n) { return ::operator new(n); }
void  operator delete(void *p) noexcept { std::free(p); }
void  operator delete[](void *p) noexcept { std::free(p); }
void  operator delete(void *p, std::size_t) noexcept { std::free(p); }
void  operator delete[](void *p, std::size_t) noexcept { std::free(p); }
#endif

namespace {

int g_failures = 0;

void check(bool ok, const char *what) {
    if (!ok) { std::fprintf(stderr, "  FAIL %s\n", what); ++g_failures; }
}

using hv::Cell;
using hv::Framebuffer;
using hv::Rect;

constexpr hv::Rgb kFg = 0x00FF0000;
constexpr hv::Rgb kBg = 0x00000040;

Cell glyph_at_back(const Framebuffer &fb, int x, int y) { return fb.at_back(x, y); }

bool is_empty(const Cell &c) { return c == hv::empty_cell(); }

/* Counts cells in the back buffer that are not the empty cell. Catches writes
 * that landed somewhere unintended. */
int painted(const Framebuffer &fb) {
    int n = 0;
    for (int y = 0; y < fb.height(); ++y)
        for (int x = 0; x < fb.width(); ++x)
            if (!is_empty(fb.at_back(x, y))) ++n;
    return n;
}

void test_cell_layout() {
    const Cell a{U'x', 1, 2, 3};
    Cell b = a;
    check(a == b, "cell: equal cells compare equal");
    b.attrs = 4;
    check(a != b, "cell: attrs participate in comparison");
    b = a; b.glyph = U'y';
    check(a != b, "cell: glyph participates in comparison");
    b = a; b.bg = 99;
    check(a != b, "cell: bg participates in comparison");
}

void test_resize() {
    Framebuffer fb;
    check(fb.empty(), "resize: starts empty");
    check(!fb.resize(0, 10),  "resize: rejects zero width");
    check(!fb.resize(10, -1), "resize: rejects negative height");
    check(!fb.resize(1 << 20, 1 << 20), "resize: rejects an absurd area");
    check(fb.empty(), "resize: a rejected resize leaves the buffer untouched");

    check(fb.resize(20, 5), "resize: accepts 20x5");
    check(fb.width() == 20 && fb.height() == 5, "resize: reports its dimensions");
    check(painted(fb) == 0, "resize: starts cleared");
}

void test_put_clipping() {
    Framebuffer fb;
    fb.resize(10, 4);
    const Cell c{U'#', kFg, kBg, 0};

    /* Every one of these must be a no-op, not a write. */
    fb.put(-1, 0, c);
    fb.put(0, -1, c);
    fb.put(10, 0, c);          /* x == width */
    fb.put(0, 4, c);           /* y == height */
    fb.put(INT_MAX, INT_MAX, c);
    fb.put(INT_MIN, INT_MIN, c);
    check(painted(fb) == 0, "put: every out-of-bounds write was dropped");

    fb.put(0, 0, c);
    fb.put(9, 3, c);           /* the two real corners */
    check(painted(fb) == 2, "put: in-bounds writes landed");
    check(glyph_at_back(fb, 9, 3).glyph == U'#', "put: wrote the right cell");
}

void test_text() {
    Framebuffer fb;
    fb.resize(8, 3);

    check(fb.text(0, 0, "hello", kFg, kBg) == 5, "text: reports columns written");
    check(glyph_at_back(fb, 4, 0).glyph == U'o', "text: laid out left to right");

    /* Truncation at the right edge, not a write past it. */
    check(fb.text(6, 1, "abcdef", kFg, kBg) == 2, "text: truncates at the edge");
    check(glyph_at_back(fb, 7, 1).glyph == U'b', "text: wrote up to the edge");

    /* A negative origin skips the off-screen prefix but keeps alignment. */
    check(fb.text(-2, 2, "abcdef", kFg, kBg) == 4, "text: clips a negative origin");
    check(glyph_at_back(fb, 0, 2).glyph == U'c', "text: kept column alignment");

    check(fb.text(0, 99, "off", kFg, kBg) == 0, "text: off-screen row writes nothing");
    check(fb.text(0, -1, "off", kFg, kBg) == 0, "text: negative row writes nothing");

    /* Multi-byte code points occupy one cell each. */
    fb.resize(8, 3);
    check(fb.text(0, 0, "─│┌", kFg, kBg) == 3, "text: 3-byte glyphs are one column each");
    check(glyph_at_back(fb, 0, 0).glyph == U'─', "text: decoded U+2500");
    check(glyph_at_back(fb, 2, 0).glyph == U'┌', "text: decoded U+250C");
}

void test_utf8_decode() {
    char32_t cp = 0;

    check(hv::utf8_decode("A", 0, cp) == 1 && cp == U'A', "utf8: ascii");
    check(hv::utf8_decode("\xC3\xA9", 0, cp) == 2 && cp == U'é', "utf8: 2-byte");
    check(hv::utf8_decode("\xE2\x94\x80", 0, cp) == 3 && cp == U'─', "utf8: 3-byte");
    check(hv::utf8_decode("\xF0\x9F\x92\xA9", 0, cp) == 4 && cp == 0x1F4A9,
          "utf8: 4-byte");

    /* Malformed input must consume at least one byte, or callers loop
     * forever. Each of these yields U+FFFD. */
    const char *bad[] = {
        "\x80",             /* stray continuation */
        "\xC0\x80",         /* overlong two-byte NUL */
        "\xC2",             /* truncated */
        "\xE0\x80\x80",     /* overlong three-byte */
        "\xED\xA0\x80",     /* UTF-16 surrogate D800 */
        "\xF5\x80\x80\x80", /* beyond U+10FFFF */
        "\xFF",             /* never valid */
    };
    for (const char *s : bad) {
        const std::size_t n = hv::utf8_decode(s, 0, cp);
        check(n >= 1, "utf8: malformed input always advances");
        check(cp == 0xFFFD, "utf8: malformed input yields U+FFFD");
    }
    check(hv::utf8_decode("", 0, cp) == 1, "utf8: empty input still advances");
}

void test_fill_and_lines() {
    Framebuffer fb;
    fb.resize(10, 6);
    const Cell c{U'*', kFg, kBg, 0};

    fb.fill(Rect{2, 1, 3, 2}, c);
    check(painted(fb) == 6, "fill: painted exactly w*h cells");
    check(glyph_at_back(fb, 2, 1).glyph == U'*' &&
          glyph_at_back(fb, 4, 2).glyph == U'*', "fill: covered its rect");
    check(is_empty(glyph_at_back(fb, 5, 1)), "fill: stopped at the right edge");

    /* Rects that hang off every side, and one that overflows int arithmetic. */
    fb.resize(10, 6);
    fb.fill(Rect{-5, -5, 3, 3}, c);
    check(painted(fb) == 0, "fill: a fully off-screen rect paints nothing");

    fb.fill(Rect{-2, -2, 4, 4}, c);
    check(painted(fb) == 4, "fill: a straddling rect paints only the overlap");

    fb.resize(10, 6);
    fb.fill(Rect{INT_MAX - 1, 0, 100, 100}, c);
    check(painted(fb) == 0, "fill: x + w near INT_MAX does not wrap");

    fb.fill(Rect{0, 0, INT_MAX, INT_MAX}, c);
    check(painted(fb) == 60, "fill: an enormous rect clips to the buffer");

    /* Same idea, but from a non-zero origin, which is what actually catches
     * 32-bit clipping arithmetic: 2 + INT_MAX wraps to a large negative
     * number, and the rect silently vanishes instead of covering the screen.
     * With the origin at 0 there is nothing to wrap, so that case cannot tell
     * the two implementations apart. */
    fb.resize(10, 6);
    fb.fill(Rect{2, 1, INT_MAX, INT_MAX}, c);
    check(painted(fb) == 8 * 5, "fill: x + w overflowing int still clips correctly");

    fb.resize(10, 6);
    fb.hline(1, 0, 4, U'-', kFg, kBg);
    fb.vline(0, 1, 3, U'|', kFg, kBg);
    check(painted(fb) == 7, "lines: painted 4 + 3 cells");
    fb.hline(1, 0, 0, U'-', kFg, kBg);
    fb.hline(1, 0, -5, U'-', kFg, kBg);
    check(painted(fb) == 7, "lines: zero and negative lengths draw nothing");
}

void test_box() {
    Framebuffer fb;
    fb.resize(10, 5);

    fb.box(Rect{0, 0, 4, 3}, hv::BoxStyle::Light, kFg, kBg);
    check(glyph_at_back(fb, 0, 0).glyph == U'┌', "box: top-left corner");
    check(glyph_at_back(fb, 3, 0).glyph == U'┐', "box: top-right corner");
    check(glyph_at_back(fb, 0, 2).glyph == U'└', "box: bottom-left corner");
    check(glyph_at_back(fb, 3, 2).glyph == U'┘', "box: bottom-right corner");
    check(glyph_at_back(fb, 1, 0).glyph == U'─', "box: top edge");
    check(glyph_at_back(fb, 0, 1).glyph == U'│', "box: left edge");
    check(is_empty(glyph_at_back(fb, 1, 1)), "box: interior untouched");
    /* 4x3 perimeter: 2*4 + 2*3 - 4 shared corners = 10. */
    check(painted(fb) == 10, "box: drew the border only");

    fb.resize(10, 5);
    fb.box(Rect{0, 0, 4, 3}, hv::BoxStyle::Ascii, kFg, kBg);
    check(glyph_at_back(fb, 0, 0).glyph == U'+' &&
          glyph_at_back(fb, 1, 0).glyph == U'-', "box: ascii style");

    fb.resize(10, 5);
    fb.box(Rect{0, 0, 4, 3}, hv::BoxStyle::Rounded, kFg, kBg);
    check(glyph_at_back(fb, 0, 0).glyph == U'╭' &&
          glyph_at_back(fb, 3, 2).glyph == U'╯', "box: rounded corners");

    /* Degenerate rects draw what fits instead of reaching outside. */
    fb.resize(10, 5);
    fb.box(Rect{1, 1, 1, 1}, hv::BoxStyle::Light, kFg, kBg);
    check(painted(fb) == 1, "box: a 1x1 box is one cell");

    fb.resize(10, 5);
    fb.box(Rect{0, 0, 2, 2}, hv::BoxStyle::Light, kFg, kBg);
    check(painted(fb) == 4, "box: a 2x2 box is four corners");

    fb.resize(10, 5);
    fb.box(Rect{0, 0, 1, 4}, hv::BoxStyle::Light, kFg, kBg);
    check(painted(fb) == 4, "box: a 1-wide box is a vertical line");

    fb.resize(10, 5);
    fb.box(Rect{0, 0, 0, 5}, hv::BoxStyle::Light, kFg, kBg);
    fb.box(Rect{0, 0, 5, 0}, hv::BoxStyle::Light, kFg, kBg);
    check(painted(fb) == 0, "box: empty rects draw nothing");

    /* Off-screen and enormous. If the edge loops were not clamped this would
     * run for two million iterations per edge; the test timing out is the
     * failure mode. */
    fb.resize(10, 5);
    fb.box(Rect{-1000000, -2, 2000000, 9}, hv::BoxStyle::Light, kFg, kBg);
    check(painted(fb) == 0,
          "box: a huge box whose edges all fall outside paints nothing");

    fb.resize(10, 5);
    fb.box(Rect{-3, -1, 20, 10}, hv::BoxStyle::Light, kFg, kBg);
    check(painted(fb) == 0, "box: a box larger than the screen has no visible border");
}

void test_double_buffering() {
    Framebuffer fb;
    fb.resize(6, 2);
    const Cell c{U'@', kFg, kBg, 0};

    fb.put(1, 1, c);
    check(fb.at_back(1, 1) == c, "swap: drawing goes to the back buffer");
    check(is_empty(fb.at_front(1, 1)), "swap: the front buffer is untouched");

    fb.swap();
    check(fb.at_front(1, 1) == c, "swap: the drawing became the front buffer");
    check(fb.front() != fb.back(), "swap: front and back are distinct storage");

    /* Two frames are needed to catch a swap that does not clear. After one
     * swap the incoming back buffer is still empty from resize(), so it looks
     * correct either way. After two, the buffer coming back around holds
     * frame 1 unless swap() cleared it, and frame 3 would then be drawn on top
     * of frame 1's leftovers. */
    const Cell c2{U'%', kFg, kBg, 0};
    fb.put(4, 0, c2);
    fb.swap();

    check(fb.at_front(4, 0) == c2, "swap: frame 2 became the front buffer");
    check(is_empty(fb.at_back(1, 1)),
          "swap: the recycled buffer was cleared, not left holding frame 1");
    check(painted(fb) == 0, "swap: the whole recycled buffer is empty");
}

#ifdef HV_COUNT_ALLOCS
void test_no_allocation_while_drawing() {
    Framebuffer fb;
    fb.resize(80, 24);

    const Cell *back_before  = fb.back();
    const Cell *front_before = fb.front();

    const long before = g_allocs.load(std::memory_order_relaxed);

    /* A frame's worth of work: clear, panels, borders, labels, swap. */
    for (int frame = 0; frame < 10; ++frame) {
        fb.clear();
        fb.fill(Rect{0, 0, 80, 1}, Cell{U' ', kFg, kBg, 0});
        fb.box(Rect{0, 1, 60, 20}, hv::BoxStyle::Light, kFg, kBg);
        fb.box(Rect{60, 1, 20, 20}, hv::BoxStyle::Rounded, kFg, kBg);
        fb.text(2, 0, "heapviz - live heap allocation map", kFg, kBg);
        fb.text(2, 22, "1 cell = 256 B", kFg, kBg);
        fb.hline(0, 21, 80, U'─', kFg, kBg);
        for (int y = 2; y < 20; ++y)
            for (int x = 1; x < 59; ++x)
                fb.put(x, y, Cell{U'█', kFg, kBg, 0});
        fb.swap();
    }

    const long after = g_allocs.load(std::memory_order_relaxed);

    check(after == before, "render: ten frames of drawing allocated nothing");

    /* An even number of swaps returns the pair to its original orientation, so
     * assert on the set rather than on which is which. The claim being tested
     * is that no third buffer was ever allocated. */
    const bool same_pair =
        (fb.front() == front_before && fb.back() == back_before) ||
        (fb.front() == back_before  && fb.back() == front_before);
    check(same_pair, "render: swapping reuses the same two buffers throughout");

    /* A SIGWINCH that reports the dimensions we already have must cost
     * nothing. Terminals emit those, and reallocating on each one would put an
     * allocation back into a path that is supposed to have none. */
    const Cell *f = fb.front();
    const Cell *b = fb.back();
    const long before_noop = g_allocs.load(std::memory_order_relaxed);
    check(fb.resize(80, 24), "resize: a same-size resize succeeds");
    check(g_allocs.load(std::memory_order_relaxed) == before_noop,
          "resize: a same-size resize allocates nothing");
    check(fb.front() == f && fb.back() == b,
          "resize: a same-size resize keeps the existing storage");
}
#endif

} // namespace

int main() {
    test_cell_layout();
    test_resize();
    test_put_clipping();
    test_text();
    test_utf8_decode();
    test_fill_and_lines();
    test_box();
    test_double_buffering();
#ifdef HV_COUNT_ALLOCS
    test_no_allocation_while_drawing();
#endif

    if (g_failures != 0) {
        std::fprintf(stderr, "framebuffer_test: %d failure(s)\n", g_failures);
        return 1;
    }
#ifdef HV_COUNT_ALLOCS
    std::printf("framebuffer_test: clipping holds on every entry point, "
                "drawing allocates nothing\n");
#else
    std::printf("framebuffer_test: clipping holds on every entry point "
                "(allocation counting skipped under ASan)\n");
#endif
    return 0;
}
