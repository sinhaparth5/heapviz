/* heapviz - terminal capability detection and fallback (M4.4).
 *
 * Copyright (C) 2026 Parth Sinha
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * M4.3 emits TrueColor unconditionally. That is right for the terminal the
 * author runs and wrong over an SSH session to a serial console, where
 * `\033[38;2;...m` is either ignored or printed as literal text. This decides
 * what the terminal can actually do, once, at startup, and the renderer emits
 * against that answer.
 *
 * Detection is a pure function of two environment strings rather than something
 * that reads the environment itself. `setenv` is not thread-safe and mutates
 * process-wide state that outlives the test using it, so a test that had to
 * install an environment to check a rule would be a test that interferes with
 * every other test in the binary.
 *
 * Nothing here queries the terminal directly. Asking (DA1, or XTGETTCAP) means
 * writing a query, then reading a reply that may never come, on a device that
 * is also the user's keyboard; the timeout alone would be a visible startup
 * delay on exactly the slow links this fallback exists for.
 */

#ifndef HEAPVIZ_TUI_CAPABILITIES_H
#define HEAPVIZ_TUI_CAPABILITIES_H

#include "tui/framebuffer.h"

#include <cstdint>

namespace hv {

enum class ColorMode {
    TrueColor, /* \033[38;2;R;G;Bm - 24-bit, what M4.3 always emitted     */
    Cube256,   /* \033[38;5;Nm     - xterm's 6x6x6 cube plus a grey ramp  */
    Ansi16,    /* \033[31m etc     - the original eight, plus the brights */
};

const char *color_mode_str(ColorMode m) noexcept;

/* The glyphs the heat map is drawn from, and the ASCII stand-ins for terminals
 * whose font has no block-drawing characters. The three ASCII shades are the
 * ones named in ROADMAP M4.4; they are deliberately different *weights* rather
 * than different characters, because the fallback still has to read as a
 * density ramp when colour is also degraded. */
struct GlyphSet {
    char32_t full;   /* U+2588 FULL BLOCK        / '#' */
    char32_t medium; /* U+2593 DARK SHADE        / '=' */
    char32_t light;  /* U+2591 LIGHT SHADE       / '.' */
    BoxStyle box;    /* Rounded                  / Ascii */
};

struct Capabilities {
    ColorMode color   = ColorMode::TrueColor;
    bool      unicode = true;
};

GlyphSet glyphs_for(const Capabilities &caps) noexcept;

/* Decides colour depth from `colorterm` (COLORTERM) and `term` (TERM), either
 * of which may be null. `force_ascii` is the `--no-unicode` flag; it only ever
 * turns Unicode off, since a user who passes it has already seen the mojibake
 * and detection has no way to know better.
 *
 * The rules, in order:
 *
 *   COLORTERM is "truecolor" or "24bit"  -> TrueColor. The de-facto standard,
 *                                           and the only positive signal.
 *   TERM contains "direct"               -> TrueColor. terminfo's direct-colour
 *                                           entries (xterm-direct) mean 24-bit.
 *   TERM contains "256color"             -> Cube256.
 *   TERM is unset, empty, or "dumb"      -> Ansi16.
 *   TERM contains "16color", or names a  -> Ansi16.
 *     known 8-colour terminal
 *   anything else                        -> Cube256.
 *
 * That last default is the debatable one. Falling back to 16 would be the
 * conservative choice, but a great many terminals still report a bare
 * TERM=xterm while supporting 256 colours, so conservatism there would degrade
 * the common case to protect a rare one. 16-colour mode is therefore opt-in
 * from TERM saying so, not a default.
 */
Capabilities detect_capabilities(const char *colorterm, const char *term,
                                 bool force_ascii = false) noexcept;

/* The same, reading COLORTERM and TERM from the environment. */
Capabilities detect_capabilities_from_env(bool force_ascii = false) noexcept;

/* --- quantisers ---------------------------------------------------------- */

/* 0x00RRGGBB -> an xterm-256 palette index.
 *
 * Considers the 6x6x6 colour cube and the 24-step grey ramp and returns
 * whichever is closer, because the cube's grey diagonal is coarse: its steps
 * are 40 apart where the ramp's are 10, so quantising a grey UI to the cube
 * alone visibly banded the panel background. */
std::uint8_t rgb_to_cube256(Rgb c) noexcept;

/* 0x00RRGGBB -> 0..15, nearest of the standard ANSI palette by squared
 * distance in RGB. A user's theme may have redefined those sixteen to anything
 * at all, which is unknowable from here; the standard values are the only
 * defensible reference point. */
std::uint8_t rgb_to_ansi16(Rgb c) noexcept;

/* --- minimum geometry ---------------------------------------------------- */

/* Below this the heap map is not squeezed, it is illegible: the gutter alone is
 * ~12 columns and the header, legend and status bars claim 6 rows. */
constexpr int kMinUsableWidth  = 80;
constexpr int kMinUsableHeight = 24;

bool size_is_usable(int w, int h) noexcept;

/* Writes a readable refusal to `fd`, naming both the size found and the size
 * needed. Takes an fd rather than printing to stderr so a test can read the
 * message back; it runs before the alternate screen is entered, so what it
 * writes stays on the user's scrollback where they can read it. */
void report_too_small(int fd, int w, int h) noexcept;

} // namespace hv

#endif /* HEAPVIZ_TUI_CAPABILITIES_H */
