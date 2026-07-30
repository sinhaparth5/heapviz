/* heapviz - semantic colour themes (M6.1).
 *
 * Copyright (C) 2026 Parth Sinha
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef HEAPVIZ_TUI_THEME_H
#define HEAPVIZ_TUI_THEME_H

#include "tui/framebuffer.h"

#include <string_view>

namespace hv {

struct HeatPalette;

enum class ThemeKind { Dark, Light };

struct Theme {
    Rgb frame;
    Rgb title;
    Rgb accent;
    Rgb malloc;
    Rgb freed;
    Rgb active;
    Rgb overhead;
    Rgb unalloc;
    Rgb cursor;
    Rgb text;
    Rgb dim;
    Rgb bg;

    /* `freed` is a saturated map fill. This companion is deliberately brighter
     * when the fill itself cannot meet WCAG contrast as foreground text. */
    Rgb danger_text;
    Rgb leak;
};

constexpr Theme dark_theme() noexcept {
    return Theme{
        0x00C87828, 0x007FE08A, 0x00F5A623, 0x004EC94E,
        0x00E01B24, 0x003584E4, 0x00F6D32D, 0x002A2A2A,
        0x0033D7E8, 0x00D8D8D8, 0x007A7A7A, 0x000C0C0C,
        0x00F36A72, 0x00E57BFF};
}

constexpr Theme light_theme() noexcept {
    return Theme{
        0x00834D10, 0x001E6B2C, 0x00805A00, 0x001F7A31,
        0x00B51620, 0x001E5FAF, 0x00805A00, 0x00E2E5E7,
        0x0000717B, 0x001C2228, 0x00545D66, 0x00FAFAF8,
        0x00A40812, 0x008A2387};
}

const Theme &theme_for(ThemeKind kind) noexcept;
bool parse_theme(std::string_view name, ThemeKind &out) noexcept;
const char *theme_name(ThemeKind kind) noexcept;
HeatPalette heat_palette(const Theme &theme) noexcept;

/* WCAG 2 relative luminance and contrast, useful both to tests and to future
 * user-supplied themes. */
double contrast_ratio(Rgb foreground, Rgb background) noexcept;
bool readable_text(Rgb foreground, Rgb background) noexcept;

} // namespace hv

#endif /* HEAPVIZ_TUI_THEME_H */
