/* heapviz - constraint-solved application layout (M6.2).
 *
 * Copyright (C) 2026 Parth Sinha
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef HEAPVIZ_TUI_LAYOUT_H
#define HEAPVIZ_TUI_LAYOUT_H

#include "tui/framebuffer.h"

namespace hv {

struct AppLayout {
    Rect title{};
    Rect address{};
    Rect section{};
    Rect map{};
    Rect inspector{};
    Rect metrics{};
    Rect footer{};
    bool legend = true;
    bool panels_stacked = false;
};

/* Wide terminals use the mockup's 60/40 lower band. Below 100 columns the
 * panels stack; below 30 rows the legend is removed before map rows are lost. */
AppLayout solve_layout(int width, int height) noexcept;

} // namespace hv

#endif /* HEAPVIZ_TUI_LAYOUT_H */
