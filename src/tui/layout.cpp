/* heapviz - constraint-solved application layout (M6.2).
 *
 * Copyright (C) 2026 Parth Sinha
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "tui/layout.h"

#include <algorithm>

namespace hv {

AppLayout solve_layout(int w, int h) noexcept {
    AppLayout out;
    if (w <= 0 || h <= 0) return out;

    out.footer = Rect{0, h - 1, w, 1};
    int top = 0;
    int bottom = h - 1;

    if (top < bottom) out.title = Rect{0, top++, w, 1};
    if (top < bottom) out.address = Rect{0, top++, w, 1};
    if (top < bottom) out.section = Rect{0, top++, w, 1};

    out.legend = h >= 30;
    out.panels_stacked = w < 100;

    constexpr int panel_h = 7; /* complete border plus five content rows */
    const int available = bottom - top;
    if (!out.panels_stacked && available >= panel_h + 4) {
        const int inspector_w = w * 3 / 5;
        out.inspector = Rect{0, bottom - panel_h, inspector_w, panel_h};
        out.metrics = Rect{inspector_w, bottom - panel_h,
                           w - inspector_w, panel_h};
        bottom -= panel_h;
    } else if (out.panels_stacked && available >= panel_h * 2 + 4) {
        out.metrics = Rect{0, bottom - panel_h, w, panel_h};
        bottom -= panel_h;
        out.inspector = Rect{0, bottom - panel_h, w, panel_h};
        bottom -= panel_h;
    } else if (available >= panel_h + 4) {
        /* The inspector answers the active cursor question, so it survives
         * when only one lower panel can fit. */
        out.inspector = Rect{0, bottom - panel_h, w, panel_h};
        bottom -= panel_h;
    }

    out.map = Rect{0, top, w, std::max(0, bottom - top)};
    return out;
}

} // namespace hv
