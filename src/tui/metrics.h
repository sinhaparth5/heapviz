/* heapviz - the telemetry metrics panel (M5.3).
 *
 * Copyright (C) 2026 Parth Sinha
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The five numbers that describe the session as a whole, next to the one that
 * says whether to believe them.
 *
 * WHY THE DROPPED COUNT SITS ON THE RING ROW
 * ------------------------------------------
 * Every other figure on this panel is wrong by an unknown amount once the ring
 * has overflowed. A dropped `malloc` leaves a chunk the table never learns
 * about; a dropped `free` leaves one it never retires, which reads on screen as
 * a leak that is not there. Nothing resyncs -- the consumer has no way to ask
 * the target what it missed -- so the count is not a health statistic to be
 * folded into a percentage, it is a caveat attached to the rest of the panel.
 * It goes on the ring's row because the ring filling is what causes it, and it
 * takes that row over entirely rather than being truncated away when the panel
 * is too narrow for both.
 *
 * WHY THE COUNTERS ARE FED RATHER THAN COMPUTED
 * ---------------------------------------------
 * `HeapApp::apply` already gets the live set right, and getting it right is
 * more delicate than it looks: an allocation at an address still marked live is
 * a `free` that was never seen rather than a second chunk, and every decrement
 * saturates because heapviz attaches to a heap that already exists. A second
 * implementation of that reasoning here would be a second thing to keep
 * correct, and the two would disagree on exactly the targets where it matters.
 * So this owns only what nobody else does: the cumulative total and the peak.
 *
 * WHAT "BYTES" MEANS HERE
 * -----------------------
 * Usable bytes, the same figure the header's `Live:` counts, not the bytes the
 * program requested. The difference is the allocator's rounding, and it is
 * small -- but the panel puts a running total, a live figure and a high-water
 * mark next to each other, and three numbers meant to be compared have to be
 * measured the same way. Overhead is the inspector's subject, per chunk, where
 * the distinction is the point rather than a footnote.
 */

#ifndef HEAPVIZ_TUI_METRICS_H
#define HEAPVIZ_TUI_METRICS_H

#include "tui/framebuffer.h"
#include "tui/theme.h"

#include <cstdint>

namespace hv {

/* Rows the panel occupies, including its rule. Deliberately the same as
 * `kInspectorRows`: the two share the bottom block, and a mismatch would leave
 * one of them with a ragged edge for no benefit. */
constexpr int kMetricsRows = 6;

/* Below this the labels and their values have no room to sit side by side and
 * the panel is dropped, leaving the inspector the whole block. Any narrower and
 * what is left is a column of truncated numbers, which is worse than the map
 * having the space. */
constexpr int kMetricsMinCols = 28;

/* Above this the values carry their exact byte counts in parentheses; below it
 * they are the human-readable figure alone. Dropping the parenthetical is a
 * better degradation than letting it be cut off mid-number, which produces a
 * digit string that looks like a real quantity and is not one. */
constexpr int kMetricsWideCols = 44;

/* Ring depth at which the display stops being informational. 50% of a ring that
 * drains every 16 ms means the consumer is roughly one frame from falling
 * behind; 80% means it already has, and is being saved by a lull. */
constexpr unsigned kRingWarnPct = 50;
constexpr unsigned kRingBadPct  = 80;

/* Fragmentation badge boundaries. `FragAnalyzer` computes the number; these live
 * here because the badge is a display decision and this is the only thing that
 * draws it.
 *
 * Measured against every mode `examples/churn` has, mid-run: steady and bursty
 * churn sit at 23%, mixed at 40-50%, and the mode written specifically to
 * fragment the heap at 51-63%. So amber is the resting state of a program that
 * allocates and frees continuously -- glibc's tcache and fastbins hold those
 * freed chunks and they really are unavailable to a larger request -- and red is
 * reached by the workload that is trying. Raising the amber boundary so ordinary
 * churn reads green would also swallow `mixed` at 40%, which is the case worth
 * catching. The full table is in ROADMAP.md's M5.4. */
constexpr int kFragMedPct  = 15;
constexpr int kFragHighPct = 40;

enum class FragBadge : std::uint8_t {
    Unknown, /* no analysis has run yet */
    Low,
    Med,
    High,
};

/* `pct` below zero means "not computed", which is a different statement from
 * "zero percent" and is drawn differently. */
FragBadge   frag_badge(int pct) noexcept;
const char *frag_badge_str(FragBadge b) noexcept;

enum class Pressure : std::uint8_t { Calm, Warn, Bad };

/* Rounded up rather than truncated: a ring holding a thousand events out of a
 * million really is 0.1% full, but printing `0%` next to a queue that is not
 * empty invites the reader to conclude the telemetry path is idle. */
unsigned ring_percent(std::uint64_t queued, std::uint64_t capacity) noexcept;
Pressure ring_pressure(std::uint64_t queued, std::uint64_t capacity) noexcept;

/* How many of `total_w` columns the metrics panel gets when it shares the
 * bottom block with a panel needing at least `other_min_cols`. Zero means it
 * gets none and the other panel takes the block whole.
 *
 * The mockup puts the inspector at roughly 60% and the metrics at 40%, which is
 * right for the width the mockup was drawn at and wrong for 80 columns: two
 * fifths of 80 is 32, which this panel can use, against 48 for the inspector,
 * which is below what its longest value needs. So the ratio is a starting point
 * and the two minimums decide -- and when both cannot be met it is this panel
 * that goes, because the inspector is answering a question the user asked with
 * the cursor and this one is answering a question nobody asked.
 *
 * One function, taking both minimums, for the reason `Grid::configure` is one
 * function: two callers computing a boundary independently disagree at exactly
 * one width each, and the symptom is one panel drawing a column into the other.
 */
int metrics_split(int total_w, int other_min_cols) noexcept;

/* What the model looked like this frame. A struct rather than six positional
 * arguments, all of which are `uint64_t` and two of which are counts of
 * different things. */
struct MetricsSample {
    std::uint64_t live_chunks   = 0;
    std::uint64_t live_bytes    = 0;
    std::uint64_t ring_queued   = 0;
    std::uint64_t ring_capacity = 0;
    std::uint64_t dropped       = 0;
    std::uint32_t now_ms        = 0;
};

/* Same shape and the same defaults as `InspectorStyle`, so M6.1 can hand both
 * the theme's tokens without either knowing what a theme is. */
struct MetricsStyle {
    Rgb ink    = dark_theme().text;
    Rgb dim    = dark_theme().dim;
    Rgb accent = dark_theme().accent;
    Rgb frame  = dark_theme().frame;
    Rgb good   = dark_theme().title;
    Rgb warn   = dark_theme().accent;
    Rgb bad    = dark_theme().danger_text;
    Rgb bg     = dark_theme().bg;
};

class Metrics {
public:
    void set_style(const MetricsStyle &s) noexcept { style_ = s; }
    const MetricsStyle &style() const noexcept { return style_; }

    /* One call per allocation event, from the drain. Saturating rather than
     * wrapping: a session that ran long enough to overflow this should report
     * an impossible-looking total, not a small one. */
    void on_alloc(std::uint64_t usable) noexcept;

    /* One call per frame. Returns true when a redraw would put different
     * characters on screen -- which on an idle session is never, so the panel
     * costs nothing to have. */
    bool sample(const MetricsSample &s) noexcept;

    /* Starts a new measurement window without changing the heap being watched.
     * Cumulative allocation traffic returns to zero, while the current live set
     * becomes the new peak baseline: resetting must not claim that a process
     * already holding memory peaked at zero. */
    void reset(const MetricsSample &baseline) noexcept;

    /* M5.4's analysis tick. Until it runs the panel says the figure is not
     * known rather than showing 0%, which would be a specific and wrong claim
     * about a heap nobody has walked. */
    void set_fragmentation(int pct) noexcept { frag_pct_ = pct; }

    /* The biggest single hole between two live chunks, which is the figure that
     * answers the question people actually ask about a fragmented heap. Two
     * setters rather than one call taking both, because they have different
     * availability: the percentage is exact on every heap, and the largest hole
     * is dropped on one too large to sort. `known` false leaves the field off
     * the panel entirely -- printing zero would read as "no holes", which is the
     * opposite of what a heap that big is likely to be. */
    void set_largest_gap(std::uint64_t bytes, bool known) noexcept {
        largest_gap_ = bytes;
        gap_known_   = known;
    }

    /* False in snapshot mode (M2.5), where there is no event stream to count.
     * A cumulative total can only be accumulated from allocations as they
     * happen; a heap walk sees the live set and nothing about what came before
     * it. Printing 0 B beside a live 500 MB heap would read as "this program
     * has never allocated anything", which is the same class of lie as
     * reporting a zero largest hole -- so the field says so instead. */
    void set_cumulative_known(bool known) noexcept { cumulative_known_ = known; }
    bool cumulative_known() const noexcept { return cumulative_known_; }

    std::uint64_t total_bytes()  const noexcept { return total_bytes_; }
    std::uint64_t total_allocs() const noexcept { return total_allocs_; }
    std::uint64_t peak_bytes()   const noexcept { return peak_bytes_; }
    std::uint32_t peak_ms()      const noexcept { return peak_ms_; }
    std::uint64_t live_chunks()  const noexcept { return live_.live_chunks; }
    std::uint64_t live_bytes()   const noexcept { return live_.live_bytes; }
    std::uint64_t dropped()      const noexcept { return live_.dropped; }
    int           frag_pct()     const noexcept { return frag_pct_; }
    FragBadge     badge()        const noexcept { return frag_badge(frag_pct_); }
    std::uint64_t largest_gap()  const noexcept { return largest_gap_; }
    bool          gap_known()    const noexcept { return gap_known_; }

    unsigned ring_pct() const noexcept {
        return ring_percent(live_.ring_queued, live_.ring_capacity);
    }
    Pressure pressure() const noexcept {
        return ring_pressure(live_.ring_queued, live_.ring_capacity);
    }

    void draw(Framebuffer &fb, Rect area) const noexcept;

private:
    MetricsStyle  style_{};
    MetricsSample live_{};
    std::uint64_t total_bytes_  = 0;
    std::uint64_t total_allocs_ = 0;
    std::uint64_t peak_bytes_   = 0;
    std::uint32_t peak_ms_      = 0;
    std::uint64_t largest_gap_  = 0;
    bool          gap_known_    = false;
    bool          cumulative_known_ = true;
    int           frag_pct_     = -1;
};

} // namespace hv

#endif /* HEAPVIZ_TUI_METRICS_H */
