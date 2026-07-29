/* heapviz - the telemetry metrics panel (M5.3).
 *
 * Copyright (C) 2026 Parth Sinha
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "tui/metrics.h"

#include "tui/grid.h"
#include "tui/panel.h"

#include <cstdio>
#include <cstring>

namespace hv {
namespace {

/* Where the values start. Two columns short of the inspector's, because these
 * labels are longer and these values are shorter -- and because this panel is
 * the narrower of the two by design. */
constexpr int kValueCol = 14;

} // namespace

FragBadge frag_badge(int pct) noexcept {
    if (pct < 0) return FragBadge::Unknown;
    if (pct < kFragMedPct) return FragBadge::Low;
    if (pct < kFragHighPct) return FragBadge::Med;
    return FragBadge::High;
}

const char *frag_badge_str(FragBadge b) noexcept {
    switch (b) {
    case FragBadge::Unknown: return "";
    case FragBadge::Low:     return "[Low]";
    case FragBadge::Med:     return "[Med]";
    case FragBadge::High:    return "[High]";
    }
    return "";
}

int metrics_split(int total_w, int other_min_cols) noexcept {
    if (total_w <= 0 || other_min_cols < 0) return 0;

    int mw = total_w * 2 / 5;
    if (mw < kMetricsMinCols) mw = kMetricsMinCols;
    if (total_w - mw < other_min_cols) return 0;
    return mw;
}

unsigned ring_percent(std::uint64_t queued, std::uint64_t capacity) noexcept {
    if (capacity == 0 || queued == 0) return 0;
    if (queued >= capacity) return 100;

    const std::uint64_t pct = queued * 100 / capacity;
    /* Never round a non-empty ring down to nothing. `0%` beside a queue that is
     * not empty invites the reader to conclude the telemetry path is idle, when
     * what it actually means is that the ring is very large. */
    return pct == 0 ? 1u : static_cast<unsigned>(pct);
}

Pressure ring_pressure(std::uint64_t queued, std::uint64_t capacity) noexcept {
    const unsigned pct = ring_percent(queued, capacity);
    if (pct >= kRingBadPct) return Pressure::Bad;
    if (pct >= kRingWarnPct) return Pressure::Warn;
    return Pressure::Calm;
}

void Metrics::on_alloc(std::uint64_t usable) noexcept {
    /* Saturating. A session cannot really allocate 2^64 bytes, but a total that
     * wrapped would read as a small number and be believed, where a pinned one
     * reads as broken and is not. */
    if (total_bytes_ > UINT64_MAX - usable) total_bytes_ = UINT64_MAX;
    else total_bytes_ += usable;
    if (total_allocs_ != UINT64_MAX) ++total_allocs_;
}

bool Metrics::sample(const MetricsSample &s) noexcept {
    bool changed = false;

    /* The high-water mark, and the moment it happened. Recorded here rather
     * than in `HeapApp::apply` because a peak is a property of the frame, not
     * of an event: a burst that allocates and frees a hundred megabytes between
     * two frames never held them at once from the display's point of view, and
     * a peak that counted it would be reporting memory the process may never
     * have had mapped simultaneously. */
    if (s.live_bytes > peak_bytes_) {
        peak_bytes_ = s.live_bytes;
        peak_ms_    = s.now_ms;
        changed     = true;
    }

    /* Compared field by field rather than by memcmp: `MetricsSample` has
     * padding after `now_ms`, and indeterminate padding bytes would make an
     * idle session report a change on some frames and not others -- which is
     * exactly the ground rule about a frame that changes nothing. */
    if (s.live_chunks != live_.live_chunks || s.live_bytes != live_.live_bytes ||
        s.dropped != live_.dropped || s.ring_capacity != live_.ring_capacity)
        changed = true;

    /* The ring's depth only counts when it moves the figure on screen. It
     * changes almost every frame on a busy target and would otherwise force a
     * repaint for a number that still reads `3%`. */
    if (ring_percent(s.ring_queued, s.ring_capacity) !=
        ring_percent(live_.ring_queued, live_.ring_capacity))
        changed = true;

    live_ = s;
    return changed;
}

void Metrics::draw(Framebuffer &fb, Rect area) const noexcept {
    if (area.w < kMetricsMinCols || area.h <= 0) return;

    char line[192];
    char num[48];
    char human[32];

    panel_rule(fb, area, " TELEMETRY METRICS ", style_.frame, style_.accent,
               style_.bg);

    const bool wide = area.w >= kMetricsWideCols;

    int dy = 1;
    const auto field = [&](const char *label, const char *value, Rgb colour) {
        panel_text(fb, area, 2, dy, label, style_.dim, style_.bg);
        panel_text(fb, area, kValueCol, dy, value, colour, style_.bg);
        ++dy;
    };

    /* Total allocated: everything the target has asked for since heapviz
     * attached, whether or not it still holds it. The human figure leads and
     * the exact count follows, because the question this answers is "how much
     * churn has there been" and nobody reads that in bytes. */
    format_byte_size(human, sizeof human, total_bytes_);
    if (wide) {
        format_count(num, sizeof num, total_bytes_);
        std::snprintf(line, sizeof line, "%s (%s B)", human, num);
    } else {
        std::snprintf(line, sizeof line, "%s", human);
    }
    field("Allocated", line, style_.ink);

    /* Active chunks, with the bytes they hold beside them. The two belong on
     * one row: a count with no size cannot distinguish a million small
     * allocations from a million large ones, and that is the first thing
     * anyone wants to know about a live set. */
    format_count(num, sizeof num, live_.live_chunks);
    format_byte_size(human, sizeof human, live_.live_bytes);
    std::snprintf(line, sizeof line, "%s chunks / %s", num, human);
    field("Active", line, style_.ink);

    /* Peak, and how far below it the target is sitting now. The delta is the
     * useful half: a peak on its own says a number was reached once, while a
     * peak next to a live figure says whether the program came back down. */
    format_byte_size(human, sizeof human, peak_bytes_);
    if (wide && peak_bytes_ > live_.live_bytes) {
        char down[32];
        format_byte_size(down, sizeof down, peak_bytes_ - live_.live_bytes);
        std::snprintf(line, sizeof line, "%s (now -%s)", human, down);
    } else {
        std::snprintf(line, sizeof line, "%s", human);
    }
    field("Peak", line, style_.ink);

    /* Fragmentation. Until M5.4's analysis tick runs there is no figure, and
     * the panel says so with a dash rather than printing `0%` -- which would be
     * a specific claim about a heap nothing has walked, and the most flattering
     * possible one. */
    const FragBadge badge = frag_badge(frag_pct_);
    if (badge == FragBadge::Unknown) {
        field("Fragmented", "--", style_.dim);
    } else {
        Rgb colour = style_.good;
        if (badge == FragBadge::Med) colour = style_.warn;
        if (badge == FragBadge::High) colour = style_.bad;
        std::snprintf(line, sizeof line, "%d%%  %s", frag_pct_,
                      frag_badge_str(badge));
        field("Fragmented", line, colour);
    }

    /* The ring, and the caveat. */
    const unsigned pct = ring_pct();
    Rgb ring_colour = style_.good;
    switch (pressure()) {
    case Pressure::Calm: ring_colour = style_.good; break;
    case Pressure::Warn: ring_colour = style_.warn; break;
    case Pressure::Bad:  ring_colour = style_.bad;  break;
    }

    format_count(num, sizeof num, live_.ring_capacity);
    if (wide) std::snprintf(line, sizeof line, "%u%% of %s", pct, num);
    else      std::snprintf(line, sizeof line, "%u%%", pct);
    const auto ring_len = static_cast<int>(std::strlen(line));

    char drops[80];
    if (live_.dropped == 0) {
        std::snprintf(drops, sizeof drops, "0 dropped");
    } else {
        format_count(num, sizeof num, live_.dropped);
        std::snprintf(drops, sizeof drops, " %s DROPPED ", num);
    }
    const auto drop_len = static_cast<int>(std::strlen(drops));
    const Rgb drop_colour = live_.dropped == 0 ? style_.dim : style_.bad;
    const auto drop_attrs =
        static_cast<std::uint8_t>(live_.dropped == 0 ? kAttrNone : kAttrBold);

    /* Both on one row where they fit. Where they do not, the drop count takes
     * the row: the ring's depth is a curiosity, and the drop count is the
     * reason not to trust the four figures above it. */
    if (kValueCol + ring_len + 2 + drop_len <= area.w) {
        panel_text(fb, area, kValueCol, dy, line, ring_colour, style_.bg);
        panel_text_right(fb, area, dy, drops, drop_colour, style_.bg,
                         drop_attrs);
        panel_text(fb, area, 2, dy, "Ring", style_.dim, style_.bg);
    } else if (live_.dropped != 0) {
        panel_text(fb, area, 2, dy, "Ring", style_.dim, style_.bg);
        panel_text(fb, area, kValueCol, dy, drops, drop_colour, style_.bg,
                   drop_attrs);
    } else {
        panel_text(fb, area, 2, dy, "Ring", style_.dim, style_.bg);
        panel_text(fb, area, kValueCol, dy, line, ring_colour, style_.bg);
    }
}

} // namespace hv
