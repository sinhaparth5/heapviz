/* heapviz - terminal capability detection and fallback (M4.4).
 *
 * Copyright (C) 2026 Parth Sinha
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "tui/capabilities.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>

namespace hv {

namespace {

bool str_eq(const char *a, const char *b) noexcept {
    return a != nullptr && b != nullptr && std::strcmp(a, b) == 0;
}

bool contains(const char *hay, const char *needle) noexcept {
    return hay != nullptr && std::strstr(hay, needle) != nullptr;
}

/* Terminals that really do stop at eight colours plus bright. Kept short and
 * exact: this list only has to cover the ones still in use on the far end of a
 * serial console or a rescue shell, and a wrong entry here costs a user 240
 * colours they had. */
bool is_known_8_colour(const char *term) noexcept {
    static const char *const kEight[] = {
        "linux", "ansi", "vt100", "vt102", "vt220", "cons25", "screen.linux",
    };
    for (const char *name : kEight) {
        if (str_eq(term, name)) return true;
    }
    return false;
}

/* The 6x6x6 cube's per-channel steps. Not evenly spaced: the gap from 0 to 95
 * is the widest, which is why dark colours quantise worse than bright ones. */
constexpr int kCubeLevels[6] = {0, 95, 135, 175, 215, 255};

int nearest_cube_level(int v) noexcept {
    int best = 0;
    int best_d = kCubeLevels[0] > v ? kCubeLevels[0] - v : v - kCubeLevels[0];
    for (int i = 1; i < 6; ++i) {
        const int d = kCubeLevels[i] > v ? kCubeLevels[i] - v : v - kCubeLevels[i];
        if (d < best_d) { best_d = d; best = i; }
    }
    return best;
}

int dist2(int r1, int g1, int b1, int r2, int g2, int b2) noexcept {
    const int dr = r1 - r2, dg = g1 - g2, db = b1 - b2;
    return dr * dr + dg * dg + db * db;
}

/* The standard xterm palette for indices 0..15. A user's theme may have
 * redefined every one of these, which is not discoverable from inside the
 * process; these are the reference values the codes are specified against. */
constexpr Rgb kAnsi16[16] = {
    0x00000000, 0x00800000, 0x00008000, 0x00808000,
    0x00000080, 0x00800080, 0x00008080, 0x00C0C0C0,
    0x00808080, 0x00FF0000, 0x0000FF00, 0x00FFFF00,
    0x000000FF, 0x00FF00FF, 0x0000FFFF, 0x00FFFFFF,
};

} // namespace

const char *color_mode_str(ColorMode m) noexcept {
    switch (m) {
    case ColorMode::TrueColor: return "truecolor (24-bit)";
    case ColorMode::Cube256:   return "256-colour";
    case ColorMode::Ansi16:    return "16-colour";
    }
    return "unknown";
}

GlyphSet glyphs_for(const Capabilities &caps) noexcept {
    if (caps.unicode) {
        return GlyphSet{U'█', U'▓', U'░', BoxStyle::Rounded};
    }
    return GlyphSet{U'#', U'=', U'.', BoxStyle::Ascii};
}

Capabilities detect_capabilities(const char *colorterm, const char *term,
                                 bool force_ascii) noexcept {
    Capabilities caps;
    caps.unicode = !force_ascii;

    /* COLORTERM is the only positive, unambiguous signal, so it wins outright.
     * Exact matches only: the convention is a bare "truecolor" or "24bit", and
     * treating any non-empty COLORTERM as 24-bit would promote the terminals
     * that set it to "1" meaning something else entirely. */
    if (str_eq(colorterm, "truecolor") || str_eq(colorterm, "24bit")) {
        caps.color = ColorMode::TrueColor;
        return caps;
    }

    if (term == nullptr || term[0] == '\0' || str_eq(term, "dumb")) {
        caps.color = ColorMode::Ansi16;
        return caps;
    }

    /* terminfo's direct-colour entries (xterm-direct, tmux-direct) describe a
     * terminal that takes 24-bit colour without advertising COLORTERM. Checked
     * before "256color" because "xterm-direct256" contains both. */
    if (contains(term, "direct")) {
        caps.color = ColorMode::TrueColor;
    } else if (contains(term, "16color") || is_known_8_colour(term)) {
        caps.color = ColorMode::Ansi16;
    } else {
        /* Both "256color" and everything unrecognised land here; see the header
         * for why the unknown default is 256 rather than 16. */
        caps.color = ColorMode::Cube256;
    }
    return caps;
}

Capabilities detect_capabilities_from_env(bool force_ascii) noexcept {
    return detect_capabilities(std::getenv("COLORTERM"), std::getenv("TERM"),
                               force_ascii);
}

std::uint8_t rgb_to_cube256(Rgb c) noexcept {
    const int r = static_cast<int>((c >> 16) & 0xFFu);
    const int g = static_cast<int>((c >> 8) & 0xFFu);
    const int b = static_cast<int>(c & 0xFFu);

    const int ri = nearest_cube_level(r);
    const int gi = nearest_cube_level(g);
    const int bi = nearest_cube_level(b);
    const int cube_err = dist2(r, g, b,
                               kCubeLevels[ri], kCubeLevels[gi], kCubeLevels[bi]);

    /* The grey ramp runs 8, 18, ... 238 in steps of ten. Anything outside that
     * span is closer to the cube's black or white corner than to any ramp
     * entry, so clamp rather than extrapolate. */
    const int avg = (r + g + b) / 3;
    int gray_i;
    if (avg <= 8)        gray_i = 0;
    else if (avg >= 238) gray_i = 23;
    else                 gray_i = (avg - 3) / 10; /* round to nearest step */
    const int gray_v = 8 + 10 * gray_i;
    const int gray_err = dist2(r, g, b, gray_v, gray_v, gray_v);

    if (gray_err < cube_err) {
        return static_cast<std::uint8_t>(232 + gray_i);
    }
    return static_cast<std::uint8_t>(16 + 36 * ri + 6 * gi + bi);
}

std::uint8_t rgb_to_ansi16(Rgb c) noexcept {
    const int r = static_cast<int>((c >> 16) & 0xFFu);
    const int g = static_cast<int>((c >> 8) & 0xFFu);
    const int b = static_cast<int>(c & 0xFFu);

    int best = 0;
    int best_d = -1;
    for (int i = 0; i < 16; ++i) {
        const Rgb p = kAnsi16[i];
        const int d = dist2(r, g, b,
                            static_cast<int>((p >> 16) & 0xFFu),
                            static_cast<int>((p >> 8) & 0xFFu),
                            static_cast<int>(p & 0xFFu));
        if (best_d < 0 || d < best_d) { best_d = d; best = i; }
    }
    return static_cast<std::uint8_t>(best);
}

bool size_is_usable(int w, int h) noexcept {
    return w >= kMinUsableWidth && h >= kMinUsableHeight;
}

void report_too_small(int fd, int w, int h) noexcept {
    char msg[256];
    const int n = std::snprintf(
        msg, sizeof msg,
        "heapviz: terminal is %dx%d; at least %dx%d is needed.\n"
        "heapviz: resize the window and try again.\n",
        w, h, kMinUsableWidth, kMinUsableHeight);
    if (n <= 0) return;

    const char *p = msg;
    std::size_t left = static_cast<std::size_t>(n) < sizeof msg
                           ? static_cast<std::size_t>(n)
                           : sizeof msg - 1;
    while (left > 0) {
        const ssize_t wrote = ::write(fd, p, left);
        if (wrote < 0) {
            if (errno == EINTR) continue;
            return; /* nowhere left to complain to */
        }
        p += wrote;
        left -= static_cast<std::size_t>(wrote);
    }
}

} // namespace hv
