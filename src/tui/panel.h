/* heapviz - drawing primitives shared by the bottom panels (M5.3).
 *
 * Copyright (C) 2026 Parth Sinha
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * `Framebuffer::text` clips to the screen, which is the right rule for a
 * full-width status line and the wrong one the moment two panels share a row.
 * A value longer than its panel would then run on under the neighbour's labels
 * -- not a crash, and not even a visibly corrupt frame, just one panel quietly
 * printing another panel's numbers. These clip to the panel instead.
 *
 * The offsets are relative to the rect rather than absolute, because that is
 * the form in which the bug disappears: a panel that never sees a screen
 * coordinate cannot compute one that lands outside itself.
 */

#ifndef HEAPVIZ_TUI_PANEL_H
#define HEAPVIZ_TUI_PANEL_H

#include "tui/framebuffer.h"

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace hv {

/* Text at (area.x + dx, area.y + dy), truncated at the panel's right edge.
 * Returns the columns written, which is short of the string when it was cut.
 *
 * Truncation is by byte, which is truncation by column only because every
 * panel string is ASCII -- the box-drawing glyphs go through `panel_rule`. A
 * multi-byte value here would be cut mid-sequence and drawn as U+FFFD, which
 * is wrong on screen but still bounded. */
int panel_text(Framebuffer &fb, Rect area, int dx, int dy, std::string_view s,
               Rgb fg, Rgb bg, std::uint8_t attrs = kAttrNone) noexcept;

/* The same, right-aligned against the panel's right edge. Drawn only if it
 * fits: a right-aligned string that had to be truncated would lose its start,
 * which is where its meaning is (" 3 of 40 " cut to "of 40 "). */
int panel_text_right(Framebuffer &fb, Rect area, int dy, std::string_view s,
                     Rgb fg, Rgb bg, std::uint8_t attrs = kAttrNone) noexcept;

/* Right-aligns a numeric value with a two-column inset. Digits, separators and
 * hexadecimal digits use `number`; words and units use `dim`. */
int panel_numeric_right(Framebuffer &fb, Rect area, int dy, std::string_view s,
                        Rgb number, Rgb dim, Rgb bg) noexcept;

/* The panel's top rule with its name inset, `── PANEL NAME ─────`. M6.3 owns
 * the chrome proper and will replace the rule with a box; this is the shape it
 * takes, drawn with what M4.2 already provides. */
void panel_rule(Framebuffer &fb, Rect area, std::string_view label, Rgb frame,
                Rgb accent, Rgb bg) noexcept;

/* "1,024" -- a decimal count with thousands separators, which is how the
 * mockup's size and count fields read. Returns the length written, and always
 * NUL-terminates within `n`.
 *
 * It lives with the panel chrome rather than with either panel because both
 * print counts, and two implementations of a separator rule end up disagreeing
 * about where the commas go on exactly one boundary each. */
std::size_t format_count(char *buf, std::size_t n, std::uint64_t v) noexcept;

} // namespace hv

#endif /* HEAPVIZ_TUI_PANEL_H */
