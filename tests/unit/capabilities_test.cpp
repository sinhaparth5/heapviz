/* heapviz - terminal capability detection and quantiser checks (M4.4).
 *
 * Copyright (C) 2026 Parth Sinha
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Detection is a pure function of two strings, so none of this touches the
 * environment: `setenv` would leak process-wide state into every other test in
 * the binary, and a detection rule that could only be tested by mutating the
 * environment would be a rule nobody tests.
 *
 * The quantisers are checked by round-trip rather than by a table of expected
 * indices. A table only says the implementation still agrees with whatever it
 * printed the day it was written; "every colour the palette can express
 * quantises back to itself" is the property that actually has to hold, and it
 * covers all 240 of them.
 */

#include "tui/capabilities.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <string>
#include <unistd.h>

namespace {

int g_failures = 0;

void check(bool ok, const char *what) {
    if (!ok) { std::fprintf(stderr, "  FAIL %s\n", what); ++g_failures; }
}

void check_mode(const char *colorterm, const char *term, hv::ColorMode want,
                const char *what) {
    const hv::Capabilities caps = hv::detect_capabilities(colorterm, term);
    if (caps.color != want) {
        std::fprintf(stderr, "  FAIL %s (COLORTERM=%s TERM=%s: got %s)\n", what,
                     colorterm ? colorterm : "(unset)",
                     term ? term : "(unset)",
                     hv::color_mode_str(caps.color));
        ++g_failures;
    }
}

/* The 6x6x6 cube's levels, repeated here rather than shared: a test that
 * imports the implementation's constant cannot catch that constant changing. */
constexpr int kLevels[6] = {0, 95, 135, 175, 215, 255};

hv::Rgb rgb(int r, int g, int b) {
    return (static_cast<hv::Rgb>(r) << 16) | (static_cast<hv::Rgb>(g) << 8) |
           static_cast<hv::Rgb>(b);
}

void test_truecolor_detection() {
    check_mode("truecolor", "xterm", hv::ColorMode::TrueColor,
               "COLORTERM=truecolor wins outright");
    check_mode("24bit", "linux", hv::ColorMode::TrueColor,
               "COLORTERM=24bit beats even an 8-colour TERM");
    check_mode(nullptr, "xterm-direct", hv::ColorMode::TrueColor,
               "terminfo direct-colour entries are 24-bit");
    check_mode(nullptr, "xterm-direct256", hv::ColorMode::TrueColor,
               "direct is checked before 256color, not after");

    /* COLORTERM is matched exactly. Terminals that set it to something else
     * entirely must not be promoted to 24-bit on the strength of it existing. */
    check_mode("yes", "xterm-256color", hv::ColorMode::Cube256,
               "a non-standard COLORTERM does not imply truecolor");
    check_mode("", "xterm-256color", hv::ColorMode::Cube256,
               "an empty COLORTERM does not imply truecolor");
}

void test_downgrade_detection() {
    check_mode(nullptr, "xterm-256color", hv::ColorMode::Cube256,
               "256color in TERM means the cube");
    check_mode(nullptr, "screen-256color", hv::ColorMode::Cube256,
               "so does it under screen");

    check_mode(nullptr, nullptr, hv::ColorMode::Ansi16,
               "an unset TERM is the most conservative case");
    check_mode(nullptr, "", hv::ColorMode::Ansi16, "an empty TERM likewise");
    check_mode(nullptr, "dumb", hv::ColorMode::Ansi16, "TERM=dumb likewise");
    check_mode(nullptr, "linux", hv::ColorMode::Ansi16,
               "the kernel console is 8 colours plus bright");
    check_mode(nullptr, "vt220", hv::ColorMode::Ansi16, "so is a vt220");
    check_mode(nullptr, "xterm-16color", hv::ColorMode::Ansi16,
               "16color in TERM is taken at its word");

    /* The documented default, and the one judgement call in the table: a bare
     * TERM=xterm is far more often a 256-colour terminal that never said so
     * than a genuine 8-colour one. */
    check_mode(nullptr, "xterm", hv::ColorMode::Cube256,
               "an unrecognised TERM defaults to 256, not 16");
}

void test_unicode_flag() {
    const hv::Capabilities on = hv::detect_capabilities("truecolor", "xterm");
    check(on.unicode, "unicode is on unless asked otherwise");
    check(hv::glyphs_for(on).full == U'█', "unicode ramp uses FULL BLOCK");
    check(hv::glyphs_for(on).box == hv::BoxStyle::Rounded,
          "unicode borders are rounded");

    const hv::Capabilities off =
        hv::detect_capabilities("truecolor", "xterm", /*force_ascii=*/true);
    check(!off.unicode, "--no-unicode turns unicode off");
    check(off.color == hv::ColorMode::TrueColor,
          "--no-unicode does not touch colour depth");

    const hv::GlyphSet g = hv::glyphs_for(off);
    check(g.full == U'#' && g.dark == U'O' && g.medium == U'=' &&
              g.light == U'.',
          "the ASCII fallback keeps four visibly distinct weights");
    check(g.box == hv::BoxStyle::Ascii, "ASCII mode draws ASCII borders");

    const hv::GlyphSet unicode = hv::glyphs_for(on);
    check(hv::glyph_is_single_width(unicode.full) &&
              hv::glyph_is_single_width(unicode.dark) &&
              hv::glyph_is_single_width(unicode.medium) &&
              hv::glyph_is_single_width(unicode.light) &&
              hv::glyph_is_single_width(unicode.half),
          "every map glyph is explicitly single-width");
    check(!hv::glyph_is_single_width(U'界'),
          "a nearby wide CJK glyph is rejected, so it cannot drift columns");
}

/* Every colour the 6x6x6 cube can express must quantise back to its own index,
 * and so must every step of the grey ramp. Between them that is all 240
 * non-system entries of the palette. */
void test_cube_round_trips() {
    int bad_cube = 0;
    for (int r = 0; r < 6; ++r) {
        for (int g = 0; g < 6; ++g) {
            for (int b = 0; b < 6; ++b) {
                const auto want =
                    static_cast<std::uint8_t>(16 + 36 * r + 6 * g + b);
                if (hv::rgb_to_cube256(rgb(kLevels[r], kLevels[g], kLevels[b])) !=
                    want) {
                    ++bad_cube;
                }
            }
        }
    }
    check(bad_cube == 0, "cube: all 216 cube colours quantise to themselves");

    int bad_gray = 0;
    for (int i = 0; i < 24; ++i) {
        const int v = 8 + 10 * i;
        if (hv::rgb_to_cube256(rgb(v, v, v)) !=
            static_cast<std::uint8_t>(232 + i)) {
            ++bad_gray;
        }
    }
    check(bad_gray == 0, "cube: all 24 grey steps quantise to themselves");
}

/* The grey ramp exists because the cube's grey diagonal is coarse: its steps
 * are 40 apart where the ramp's are 10. A quantiser that only looked at the
 * cube would band a grey UI, so pin the cases that prove the ramp is consulted. */
void test_cube_prefers_the_grey_ramp() {
    check(hv::rgb_to_cube256(rgb(128, 128, 128)) == 244,
          "cube: mid grey lands on the ramp, not the cube's 135");
    check(hv::rgb_to_cube256(rgb(0x70, 0x78, 0x80)) >= 232,
          "cube: a near-grey panel colour still lands on the ramp");

    /* ...but pure black and white are cube corners with zero error, and must
     * not be dragged onto a ramp that starts at 8 and stops at 238. */
    check(hv::rgb_to_cube256(rgb(0, 0, 0)) == 16, "cube: black is the cube's 16");
    check(hv::rgb_to_cube256(rgb(255, 255, 255)) == 231,
          "cube: white is the cube's 231");

    /* Saturated colours have no business near the ramp. */
    check(hv::rgb_to_cube256(rgb(255, 0, 0)) == 196, "cube: pure red is 196");
    check(hv::rgb_to_cube256(rgb(0, 255, 0)) == 46, "cube: pure green is 46");
    check(hv::rgb_to_cube256(rgb(0, 0, 255)) == 21, "cube: pure blue is 21");
}

void test_cube_never_uses_system_colours() {
    /* Indices 0-15 are whatever the user's theme redefined them to, so the
     * quantiser must never reach for one: it cannot predict what would appear. */
    int below = 0;
    for (int r = 0; r < 256; r += 5) {
        for (int g = 0; g < 256; g += 5) {
            for (int b = 0; b < 256; b += 5) {
                if (hv::rgb_to_cube256(rgb(r, g, b)) < 16) ++below;
            }
        }
    }
    check(below == 0, "cube: never emits a system-palette index");
}

void test_ansi16_round_trips() {
    static const hv::Rgb kPalette[16] = {
        0x00000000, 0x00800000, 0x00008000, 0x00808000,
        0x00000080, 0x00800080, 0x00008080, 0x00C0C0C0,
        0x00808080, 0x00FF0000, 0x0000FF00, 0x00FFFF00,
        0x000000FF, 0x00FF00FF, 0x0000FFFF, 0x00FFFFFF,
    };
    int bad = 0;
    for (int i = 0; i < 16; ++i) {
        if (hv::rgb_to_ansi16(kPalette[i]) != static_cast<std::uint8_t>(i)) ++bad;
    }
    check(bad == 0, "ansi16: every palette entry quantises to itself");

    check(hv::rgb_to_ansi16(rgb(250, 10, 10)) == 9,
          "ansi16: near-red picks bright red");
    check(hv::rgb_to_ansi16(rgb(20, 20, 20)) == 0,
          "ansi16: near-black picks black");

    int out_of_range = 0;
    for (int r = 0; r < 256; r += 7) {
        for (int g = 0; g < 256; g += 7) {
            for (int b = 0; b < 256; b += 7) {
                if (hv::rgb_to_ansi16(rgb(r, g, b)) > 15) ++out_of_range;
            }
        }
    }
    check(out_of_range == 0, "ansi16: always returns 0..15");
}

void test_minimum_size() {
    check(hv::size_is_usable(80, 24), "size: exactly 80x24 is usable");
    check(!hv::size_is_usable(79, 24), "size: one column short is not");
    check(!hv::size_is_usable(80, 23), "size: one row short is not");
    check(hv::size_is_usable(200, 50), "size: a large terminal is usable");
    check(!hv::size_is_usable(0, 0), "size: a degenerate terminal is not");
}

/* The refusal has to name both numbers. "Terminal too small" without saying how
 * small, or without saying what is needed, leaves the user resizing blind. */
void test_refusal_message_names_both_sizes() {
    int fds[2];
    if (::pipe(fds) != 0) {
        check(false, "message: pipe() failed");
        return;
    }

    hv::report_too_small(fds[1], 40, 12);
    ::close(fds[1]);

    std::string got;
    char buf[512];
    for (;;) {
        const ssize_t n = ::read(fds[0], buf, sizeof buf);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        got.append(buf, static_cast<std::size_t>(n));
    }
    ::close(fds[0]);

    check(!got.empty(), "message: something was written");
    check(got.find("40x12") != std::string::npos,
          "message: names the size that was found");
    check(got.find("80x24") != std::string::npos,
          "message: names the size that is needed");
    check(got.find("heapviz") != std::string::npos,
          "message: says which program is refusing");
}

} // namespace

int main() {
    test_truecolor_detection();
    test_downgrade_detection();
    test_unicode_flag();
    test_cube_round_trips();
    test_cube_prefers_the_grey_ramp();
    test_cube_never_uses_system_colours();
    test_ansi16_round_trips();
    test_minimum_size();
    test_refusal_message_names_both_sizes();

    if (g_failures != 0) {
        std::fprintf(stderr, "capabilities_test: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("capabilities_test: detection, both quantisers, the glyph "
                "fallback and the size refusal all hold\n");
    return 0;
}
