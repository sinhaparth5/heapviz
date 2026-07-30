/* heapviz - semantic colour themes (M6.1).
 *
 * Copyright (C) 2026 Parth Sinha
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "tui/theme.h"

#include "tui/heat_color.h"

#include <algorithm>
#include <cmath>

namespace hv {
namespace {

double channel(unsigned value) noexcept {
    const double s = static_cast<double>(value) / 255.0;
    return s <= 0.04045 ? s / 12.92 : std::pow((s + 0.055) / 1.055, 2.4);
}

double luminance(Rgb c) noexcept {
    return 0.2126 * channel((c >> 16) & 0xffu) +
           0.7152 * channel((c >> 8) & 0xffu) +
           0.0722 * channel(c & 0xffu);
}

} // namespace

const Theme &theme_for(ThemeKind kind) noexcept {
    static constexpr Theme dark = dark_theme();
    static constexpr Theme light = light_theme();
    return kind == ThemeKind::Light ? light : dark;
}

bool parse_theme(std::string_view name, ThemeKind &out) noexcept {
    if (name == "dark") { out = ThemeKind::Dark; return true; }
    if (name == "light") { out = ThemeKind::Light; return true; }
    return false;
}

const char *theme_name(ThemeKind kind) noexcept {
    return kind == ThemeKind::Light ? "light" : "dark";
}

HeatPalette heat_palette(const Theme &t) noexcept {
    return HeatPalette{t.malloc, t.freed, t.active, t.overhead, t.unalloc};
}

double contrast_ratio(Rgb foreground, Rgb background) noexcept {
    const double a = luminance(foreground);
    const double b = luminance(background);
    const double hi = std::max(a, b);
    const double lo = std::min(a, b);
    return (hi + 0.05) / (lo + 0.05);
}

bool readable_text(Rgb foreground, Rgb background) noexcept {
    return contrast_ratio(foreground, background) >= 4.5;
}

} // namespace hv
