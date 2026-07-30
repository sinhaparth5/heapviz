/* heapviz - semantic theme and responsive layout checks (M6.1/M6.2).
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "tui/layout.h"
#include "tui/theme.h"

#include <cstdio>

namespace {
int failures = 0;

void check(bool ok, const char *what) {
    if (!ok) { std::fprintf(stderr, "  FAIL %s\n", what); ++failures; }
}
}

int main() {
    const hv::Theme dark = hv::dark_theme();
    check(dark.frame == 0x00C87828 && dark.title == 0x007FE08A &&
              dark.accent == 0x00F5A623 && dark.malloc == 0x004EC94E &&
              dark.freed == 0x00E01B24 && dark.active == 0x003584E4 &&
              dark.overhead == 0x00F6D32D && dark.unalloc == 0x002A2A2A &&
              dark.cursor == 0x0033D7E8 && dark.text == 0x00D8D8D8 &&
              dark.dim == 0x007A7A7A && dark.bg == 0x000C0C0C,
          "dark theme keeps the roadmap's semantic tokens exactly");

    const hv::Rgb dark_text[] = {
        dark.frame, dark.title, dark.accent, dark.malloc, dark.active,
        dark.cursor, dark.text, dark.dim, dark.danger_text, dark.leak};
    for (const hv::Rgb c : dark_text)
        check(hv::readable_text(c, dark.bg),
              "every dark foreground token meets 4.5:1");

    const hv::Theme light = hv::light_theme();
    const hv::Rgb light_text[] = {
        light.frame, light.title, light.accent, light.malloc, light.active,
        light.cursor, light.text, light.dim, light.danger_text, light.leak};
    for (const hv::Rgb c : light_text)
        check(hv::readable_text(c, light.bg),
              "every light foreground token meets 4.5:1");

    hv::ThemeKind kind = hv::ThemeKind::Dark;
    check(hv::parse_theme("light", kind) && kind == hv::ThemeKind::Light,
          "light theme parses");
    check(!hv::parse_theme("neon", kind), "unknown themes are rejected");

    const hv::AppLayout wide = hv::solve_layout(140, 40);
    check(!wide.panels_stacked && wide.inspector.w == 84 &&
              wide.metrics.w == 56,
          "wide lower panels use the 60/40 split");
    check(wide.map.y + wide.map.h <= wide.inspector.y,
          "wide panels never overlap the map");

    const hv::AppLayout narrow = hv::solve_layout(90, 40);
    check(narrow.panels_stacked && narrow.inspector.y < narrow.metrics.y,
          "below 100 columns the lower panels stack");

    const hv::AppLayout short_one = hv::solve_layout(120, 29);
    check(!short_one.legend, "below 30 rows the legend collapses");
    const hv::AppLayout tiny = hv::solve_layout(20, 6);
    check(tiny.map.h >= 0 && tiny.footer.y == 5,
          "minimum running geometry remains valid");

    if (failures != 0) {
        std::fprintf(stderr, "theme_layout_test: %d failure(s)\n", failures);
        return 1;
    }
    std::puts("theme_layout_test: ok");
    return 0;
}
