/* heapviz - differential ANSI streamer (M4.3).
 *
 * Copyright (C) 2026 Parth Sinha
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Turns the difference between the front and back buffers into a byte stream,
 * then hands the terminal that stream in one write(2).
 *
 * Three elisions carry the performance, and each is worth milliseconds on a
 * large terminal:
 *
 *   1. Unchanged cells produce nothing at all.
 *   2. Pen state is tracked, so a run of cells sharing a colour emits one SGR
 *      sequence rather than one per cell. A naive renderer spends ~40 bytes of
 *      colour on every cell; a heatmap is mostly runs of one colour.
 *   3. Cursor position is tracked, so writing consecutive cells emits no
 *      motion at all.
 *
 * Ground rules #3 and #5: exactly one write(2) per frame, looped on partial
 * writes, and no allocation anywhere in here. The output buffer is reserved
 * once by reserve() and only ever cleared, never grown.
 */

#ifndef HEAPVIZ_TUI_RENDERER_H
#define HEAPVIZ_TUI_RENDERER_H

#include "tui/capabilities.h"
#include "tui/framebuffer.h"

#include <cstddef>
#include <cstdint>
#include <sys/types.h> /* ssize_t, for the write(2)-shaped WriteFn */
#include <vector>

namespace hv {

/* Upper bound on what one cell can cost: a cursor move (14), an SGR reset (4),
 * attributes (11), two TrueColor sequences (19 each), and a 4-byte glyph. The
 * real figure is 71; the slack keeps the arithmetic obvious and the buffer is
 * sized once. reserve() uses this to guarantee render() never reallocates. */
constexpr std::size_t kMaxBytesPerCell = 80;

class Renderer {
public:
    /* Colour depth to emit (M4.4). TrueColor is the default, so a Renderer
     * built without asking behaves exactly as it did before capability
     * detection existed. Takes effect on the next frame. */
    void set_color_mode(ColorMode m) noexcept;
    ColorMode color_mode() const noexcept { return mode_; }

    /* Sizes the output buffer for a w x h terminal. The only call that
     * allocates. Safe to call on every resize.
     *
     * TrueColor is the most expensive mode per cell, so a buffer sized here is
     * large enough for any mode; switching to Cube256 later cannot overflow
     * what was reserved. */
    void reserve(int w, int h);

    /* Diffs front against back and fills the output buffer. Returns the number
     * of bytes produced, which is zero when nothing changed: an idle frame
     * costs no syscall at all.
     *
     * `full_repaint` ignores the front buffer and emits every cell, which is
     * what a SIGWINCH needs, since the terminal has discarded its contents and
     * the front buffer no longer describes what is on screen. */
    std::size_t render(const Framebuffer &fb, bool full_repaint = false);

    /* Signature of write(2), so a test can substitute one that returns short.
     * A blocking fd transfers everything unless a signal lands mid-write, so
     * the partial-write path cannot be reached reliably from outside; SIGWINCH
     * arriving during a large frame is exactly when it does happen in
     * production, and truncating there would corrupt the screen. */
    using WriteFn = ssize_t (*)(int fd, const void *buf, std::size_t n);

    /* One write(2), looped over partial writes and EINTR. Returns false only
     * if the terminal went away. Writing an empty buffer is a no-op and does
     * not touch the fd. Passing a writer overrides ::write, for tests. */
    bool flush(int fd, WriteFn writer = nullptr);

    const char  *data() const noexcept { return out_.data(); }
    std::size_t  size() const noexcept { return out_.size(); }

    /* How many cells the last render() actually emitted, against how many it
     * looked at. Feeds the debug timing overlay in M4.5. */
    std::size_t cells_emitted() const noexcept { return emitted_; }
    std::size_t cells_examined() const noexcept { return examined_; }

private:
    void append(const char *s, std::size_t n);
    void append_lit(const char *s);
    void put_uint(std::uint32_t v);
    void put_utf8(char32_t cp);
    void move_to(int x, int y);
    void emit_pen(const Cell &c);

    /* Rgb -> whatever the current mode actually puts on the wire: the RGB
     * itself, a 256-cube index, or an ANSI 0..15. */
    std::uint32_t resolve(Rgb c) const noexcept;
    void emit_color(bool foreground, std::uint32_t code);

    std::vector<char> out_;

    ColorMode mode_ = ColorMode::TrueColor;

    /* Pen and cursor are per-frame: the frame epilogue resets SGR and leaves
     * the cursor wherever the last glyph put it, so carrying either across
     * frames would be a guess.
     *
     * The two colours are held *resolved*, not as the Cell's RGB. Quantising
     * is many-to-one, so two cells differing in RGB can emit the same code;
     * comparing the raw values would emit that code twice and hand a 256-colour
     * terminal more bytes per frame than a TrueColor one gets. */
    std::uint32_t pen_fg_    = 0;
    std::uint32_t pen_bg_    = 0;
    std::uint8_t  pen_attrs_ = 0;
    bool          pen_valid_ = false;

    int  cur_x_     = 0;
    int  cur_y_     = 0;
    bool cur_valid_ = false;

    std::size_t emitted_  = 0;
    std::size_t examined_ = 0;
};

/* Exposed for the tests. Appends the UTF-8 encoding of `cp`, substituting
 * U+FFFD for surrogates and anything above U+10FFFF. */
void utf8_encode(std::vector<char> &out, char32_t cp);

} // namespace hv

#endif /* HEAPVIZ_TUI_RENDERER_H */
