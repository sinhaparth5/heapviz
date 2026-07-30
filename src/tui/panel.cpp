/* heapviz - drawing primitives shared by the bottom panels (M5.3).
 *
 * Copyright (C) 2026 Parth Sinha
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "tui/panel.h"

#include <cctype>
#include <cstdio>

namespace hv {

int panel_text(Framebuffer &fb, Rect area, int dx, int dy, std::string_view s,
               Rgb fg, Rgb bg, std::uint8_t attrs) noexcept {
    if (area.w <= 0 || area.h <= 0) return 0;
    if (dx < 0 || dy < 0 || dx >= area.w || dy >= area.h) return 0;

    const auto room = static_cast<std::size_t>(area.w - dx);
    if (s.size() > room) s = s.substr(0, room);
    return fb.text(area.x + dx, area.y + dy, s, fg, bg, attrs);
}

int panel_text_right(Framebuffer &fb, Rect area, int dy, std::string_view s,
                     Rgb fg, Rgb bg, std::uint8_t attrs) noexcept {
    if (area.w <= 0 || area.h <= 0) return 0;
    if (dy < 0 || dy >= area.h) return 0;
    if (s.size() > static_cast<std::size_t>(area.w)) return 0;

    const int dx = area.w - static_cast<int>(s.size());
    return fb.text(area.x + dx, area.y + dy, s, fg, bg, attrs);
}

int panel_numeric_right(Framebuffer &fb, Rect area, int dy, std::string_view s,
                        Rgb number, Rgb dim, Rgb bg) noexcept {
    if (area.w <= 4 || dy < 0 || dy >= area.h) return 0;
    const int room = area.w - 4;
    if (static_cast<int>(s.size()) > room) return 0;

    const int x = area.x + area.w - 2 - static_cast<int>(s.size());
    const int written = fb.text(x, area.y + dy, s, dim, bg);
    const bool hexadecimal = s.size() > 2 && s[0] == '0' && s[1] == 'x';
    for (std::size_t i = 0; i < s.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        const bool numeric = std::isdigit(c) != 0 || c == ',' || c == '.' ||
                             c == '~' || (hexadecimal &&
                             (c == 'x' || (c >= 'a' && c <= 'f') ||
                              (c >= 'A' && c <= 'F')));
        if (numeric)
            fb.put(x + static_cast<int>(i), area.y + dy,
                   Cell{static_cast<char32_t>(c), number, bg, kAttrNone});
    }
    return written;
}

void panel_rule(Framebuffer &fb, Rect area, std::string_view label, Rgb frame,
                Rgb accent, Rgb bg) noexcept {
    if (area.w <= 0 || area.h <= 0) return;

    fb.box(area, BoxStyle::Light, frame, bg);

    if (label.empty()) return;
    if (static_cast<int>(label.size()) + 6 > area.w) return;

    char title[96];
    const int written = std::snprintf(title, sizeof title, "[%.*s]",
                                      static_cast<int>(label.size()),
                                      label.data());
    if (written <= 0) return;
    panel_text(fb, area, 2, 0, title, accent, bg, kAttrBold);
}

std::size_t format_count(char *buf, std::size_t n, std::uint64_t v) noexcept {
    if (buf == nullptr || n == 0) return 0;

    char digits[24];
    const int len = std::snprintf(digits, sizeof digits, "%llu",
                                  static_cast<unsigned long long>(v));
    if (len <= 0) { buf[0] = '\0'; return 0; }

    const auto d = static_cast<std::size_t>(len);
    std::size_t out = 0;
    for (std::size_t i = 0; i < d; ++i) {
        /* A separator goes before every digit whose distance from the end is a
         * multiple of three, except the first -- which is what stops "100" from
         * coming out as ",100". */
        if (i != 0 && (d - i) % 3 == 0) {
            if (out + 1 >= n) break;
            buf[out++] = ',';
        }
        if (out + 1 >= n) break;
        buf[out++] = digits[i];
    }
    buf[out] = '\0';
    return out;
}

} // namespace hv
