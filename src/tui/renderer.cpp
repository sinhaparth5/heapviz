/* heapviz - differential ANSI streamer (M4.3).
 *
 * Copyright (C) 2026 Parth Sinha
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "tui/renderer.h"

#include <cerrno>
#include <cstring>
#include <unistd.h>

namespace hv {

namespace {

constexpr char32_t kReplacement = 0xFFFD;

/* Fixed overhead per frame: the SGR reset in the epilogue, plus slack. */
constexpr std::size_t kFrameOverhead = 32;

} // namespace

void utf8_encode(std::vector<char> &out, char32_t cp) {
    if (cp > 0x10FFFFu || (cp >= 0xD800u && cp <= 0xDFFFu)) cp = kReplacement;

    if (cp < 0x80u) {
        out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800u) {
        out.push_back(static_cast<char>(0xC0u | (cp >> 6)));
        out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
    } else if (cp < 0x10000u) {
        out.push_back(static_cast<char>(0xE0u | (cp >> 12)));
        out.push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
    } else {
        out.push_back(static_cast<char>(0xF0u | (cp >> 18)));
        out.push_back(static_cast<char>(0x80u | ((cp >> 12) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
    }
}

void Renderer::append(const char *s, std::size_t n) {
    out_.insert(out_.end(), s, s + n);
}

void Renderer::append_lit(const char *s) { append(s, std::strlen(s)); }

/* Digits are written by hand rather than through snprintf. At three integers
 * per colour and two per cursor move, a printf call per number would dominate
 * the frame. */
void Renderer::put_uint(std::uint32_t v) {
    char tmp[10];
    int  n = 0;
    do {
        tmp[n++] = static_cast<char>('0' + (v % 10u));
        v /= 10u;
    } while (v != 0);
    while (n-- > 0) out_.push_back(tmp[n]);
}

void Renderer::put_utf8(char32_t cp) {
    /* A zeroed cell means "never drawn"; render it as a space rather than
     * emitting a NUL into the terminal. */
    utf8_encode(out_, cp == 0 ? U' ' : cp);
}

void Renderer::move_to(int x, int y) {
    if (cur_valid_ && cur_x_ == x && cur_y_ == y) return;
    append_lit("\033[");
    put_uint(static_cast<std::uint32_t>(y) + 1u); /* ANSI is 1-based */
    out_.push_back(';');
    put_uint(static_cast<std::uint32_t>(x) + 1u);
    out_.push_back('H');
    cur_x_     = x;
    cur_y_     = y;
    cur_valid_ = true;
}

void Renderer::set_color_mode(ColorMode m) noexcept {
    mode_ = m;
    pen_valid_ = false; /* the held pen is in the old mode's units */
}

std::uint32_t Renderer::resolve(Rgb c) const noexcept {
    switch (mode_) {
    case ColorMode::TrueColor: return c;
    case ColorMode::Cube256:   return rgb_to_cube256(c);
    case ColorMode::Ansi16:    return rgb_to_ansi16(c);
    }
    return c;
}

void Renderer::emit_color(bool foreground, std::uint32_t code) {
    switch (mode_) {
    case ColorMode::TrueColor:
        append_lit(foreground ? "\033[38;2;" : "\033[48;2;");
        put_uint((code >> 16) & 0xFFu);
        out_.push_back(';');
        put_uint((code >> 8) & 0xFFu);
        out_.push_back(';');
        put_uint(code & 0xFFu);
        out_.push_back('m');
        return;

    case ColorMode::Cube256:
        append_lit(foreground ? "\033[38;5;" : "\033[48;5;");
        put_uint(code);
        out_.push_back('m');
        return;

    case ColorMode::Ansi16:
        /* 30-37 and 40-47 for the first eight; the brights got bolted on later
         * as 90-97 and 100-107 rather than extending the original range. */
        append_lit("\033[");
        if (code < 8u) {
            put_uint((foreground ? 30u : 40u) + code);
        } else {
            put_uint((foreground ? 90u : 100u) + (code - 8u));
        }
        out_.push_back('m');
        return;
    }
}

void Renderer::emit_pen(const Cell &c) {
    /* Attributes have no portable "turn just this one off", so any change to
     * them goes through a full reset and re-application. Colour-only changes,
     * which are the common case in a heatmap, skip that. */
    if (!pen_valid_ || c.attrs != pen_attrs_) {
        append_lit("\033[0m");
        pen_valid_ = false;

        if (c.attrs != 0) {
            append_lit("\033[");
            bool first = true;
            const auto code = [&](std::uint8_t bit, const char *sgr) {
                if ((c.attrs & bit) == 0) return;
                if (!first) out_.push_back(';');
                append_lit(sgr);
                first = false;
            };
            code(kAttrBold, "1");
            code(kAttrDim, "2");
            code(kAttrUnderline, "4");
            code(kAttrReverse, "7");
            out_.push_back('m');
        }
        pen_attrs_ = c.attrs;
    }

    /* Resolved before the comparison, not after: in a quantising mode the
     * question "does this cell need a new colour sequence" is about the code
     * that would be emitted, not about the RGB it came from. */
    const std::uint32_t fg = resolve(c.fg);
    const std::uint32_t bg = resolve(c.bg);

    if (!pen_valid_ || fg != pen_fg_) {
        emit_color(/*foreground=*/true, fg);
        pen_fg_ = fg;
    }

    if (!pen_valid_ || bg != pen_bg_) {
        emit_color(/*foreground=*/false, bg);
        pen_bg_ = bg;
    }

    pen_valid_ = true;
}

void Renderer::reserve(int w, int h) {
    if (w <= 0 || h <= 0) return;
    const auto cells = static_cast<std::size_t>(w) * static_cast<std::size_t>(h);
    out_.reserve(cells * kMaxBytesPerCell + kFrameOverhead);
}

std::size_t Renderer::render(const Framebuffer &fb, bool full_repaint) {
    out_.clear(); /* keeps the capacity: this is what makes render allocation-free */

    pen_valid_ = false;
    cur_valid_ = false;
    emitted_   = 0;
    examined_  = 0;

    if (fb.empty()) return 0;

    const int w = fb.width();
    const int h = fb.height();

    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            ++examined_;
            const Cell &now    = fb.at_back(x, y);
            const Cell &before = fb.at_front(x, y);
            if (!full_repaint && now == before) continue;

            move_to(x, y);
            emit_pen(now);
            put_utf8(now.glyph);
            ++emitted_;

            /* The terminal advanced the cursor for us. Past the last column,
             * whether it wrapped depends on the terminal's autowrap setting,
             * so stop trusting the position rather than guess.
             *
             * This is currently belt-and-braces and no test can distinguish it:
             * cur_x_ == w never equals a valid target column, so move_to()
             * emits a move regardless. It matters the moment move_to() learns
             * relative motion (CUF, or \r\n for the next row), where believing
             * an impossible position would put a glyph in the wrong place. */
            ++cur_x_;
            if (cur_x_ >= w) cur_valid_ = false;
        }
    }

    /* Leave no colour set. A crash or a scroll after this point must not tint
     * whatever the shell prints next. Skipped entirely on an idle frame so
     * that "nothing changed" really costs nothing. */
    if (!out_.empty()) append_lit("\033[0m");

    return out_.size();
}

bool Renderer::flush(int fd, WriteFn writer) {
    const char *p = out_.data();
    std::size_t n = out_.size();
    if (n == 0) return true;

    if (writer == nullptr) {
        writer = [](int f, const void *b, std::size_t c) -> ssize_t {
            return ::write(f, b, c);
        };
    }

    while (n > 0) {
        const ssize_t written = writer(fd, p, n);
        if (written < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        p += written;
        n -= static_cast<std::size_t>(written);
    }
    return true;
}

} // namespace hv
