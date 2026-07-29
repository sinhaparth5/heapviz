/* heapviz - telemetry metrics panel checks (M5.3).
 *
 * Copyright (C) 2026 Parth Sinha
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Four properties, and each is a way the panel could be quietly wrong rather
 * than visibly broken.
 *
 * **The drop count is never the thing that gets truncated.** Every other figure
 * here is wrong by an unknown amount once the ring has overflowed, so a narrow
 * panel that cut the caveat off while keeping the numbers it invalidates would
 * be the worst possible degradation. The narrow case is checked by drawing it.
 *
 * **A non-empty ring never reads as empty.** `queued * 100 / capacity` is zero
 * for anything under 1% of a 1 Mi-slot ring, which is most of a busy session --
 * and `0%` next to a queue holding ten thousand events reads as "the telemetry
 * path is idle", which is the opposite of true.
 *
 * **An idle frame changes nothing.** `sample` runs every frame and its return
 * value is what puts bytes on the wire. A version that returned true because
 * the ring's raw depth wobbled by one event would break ground rule #4 for the
 * whole application, not just for this panel, so it is checked directly.
 *
 * **Neither panel writes into the other.** The bottom block is split between
 * two panels that each format text without knowing the other exists;
 * `Framebuffer::text` clips to the screen, not to the rect, so the only thing
 * standing between a long value and the neighbour's labels is `panel_text`.
 * The check draws both into a real framebuffer and reads the boundary column.
 */

#include "tui/metrics.h"

#include "tui/inspector.h"
#include "tui/panel.h"

#include <cstdio>
#include <cstring>
#include <string>

namespace {

int g_failures = 0;

void check(bool ok, const char *what) {
    if (!ok) { std::fprintf(stderr, "  FAIL %s\n", what); ++g_failures; }
}

std::string row_text(const hv::Framebuffer &fb, int y) {
    std::string s;
    for (int x = 0; x < fb.width(); ++x) {
        const char32_t g = fb.at_back(x, y).glyph;
        s.push_back(g < 128 ? static_cast<char>(g) : '?');
    }
    return s;
}

std::string rect_text(const hv::Framebuffer &fb, hv::Rect r) {
    std::string s;
    for (int y = r.y; y < r.y + r.h; ++y) {
        for (int x = r.x; x < r.x + r.w; ++x) {
            const char32_t g = fb.at_back(x, y).glyph;
            s.push_back(g < 128 ? static_cast<char>(g) : '?');
        }
        s.push_back('\n');
    }
    return s;
}

/* The colour the first cell of `needle` was drawn in, or 0 if it is not there.
 * The panel says as much with colour as with words -- an amber ring reading and
 * a blue one are the same characters -- so the checks that matter here have to
 * look at more than the glyphs. */
hv::Rgb colour_of(const hv::Framebuffer &fb, hv::Rect r, const char *needle) {
    for (int y = r.y; y < r.y + r.h; ++y) {
        const std::string row = row_text(fb, y);
        const std::size_t at = row.find(needle);
        if (at == std::string::npos) continue;
        if (static_cast<int>(at) < r.x || static_cast<int>(at) >= r.x + r.w)
            continue;
        return fb.at_back(static_cast<int>(at), y).fg;
    }
    return 0;
}

hv::MetricsSample sample_of(std::uint64_t chunks, std::uint64_t bytes,
                            std::uint64_t queued, std::uint64_t cap,
                            std::uint64_t dropped, std::uint32_t now_ms) {
    hv::MetricsSample s;
    s.live_chunks   = chunks;
    s.live_bytes    = bytes;
    s.ring_queued   = queued;
    s.ring_capacity = cap;
    s.dropped       = dropped;
    s.now_ms        = now_ms;
    return s;
}

/* --- the model ---------------------------------------------------------- */

void test_the_total_is_cumulative() {
    hv::Metrics m;
    check(m.total_bytes() == 0 && m.total_allocs() == 0,
          "total: a fresh session has allocated nothing");

    m.on_alloc(1000);
    m.on_alloc(24);
    m.on_alloc(1000);
    check(m.total_bytes() == 2024, "total: every allocation counts once");
    check(m.total_allocs() == 3, "total: including the ones at the same size");

    /* Freeing does not reduce it, which is the whole point of the figure: it is
     * how much churn there has been, not how much is held. The live set is the
     * other number on the row below. */
    m.sample(sample_of(0, 0, 0, 1024, 0, 100));
    check(m.total_bytes() == 2024, "total: a free does not take it back");
}

void test_the_total_saturates() {
    hv::Metrics m;
    m.on_alloc(UINT64_MAX - 8);
    m.on_alloc(64);
    check(m.total_bytes() == UINT64_MAX,
          "total: it pins at the maximum rather than wrapping to a small "
          "number that would be believed");
}

void test_the_peak_is_a_high_water_mark() {
    hv::Metrics m;

    m.sample(sample_of(10, 4096, 0, 1024, 0, 100));
    check(m.peak_bytes() == 4096, "peak: the first sample sets it");
    check(m.peak_ms() == 100, "peak: with the moment it happened");

    m.sample(sample_of(30, 16384, 0, 1024, 0, 200));
    check(m.peak_bytes() == 16384 && m.peak_ms() == 200, "peak: it rises");

    m.sample(sample_of(2, 512, 0, 1024, 0, 300));
    check(m.peak_bytes() == 16384, "peak: and does not come back down");
    check(m.peak_ms() == 200, "peak: nor does the moment move with it");
    check(m.live_bytes() == 512, "peak: while the live figure does");
}

void test_the_peak_is_per_frame_not_per_event() {
    /* A burst that allocates and frees between two frames never held the memory
     * from the display's point of view. A peak folded per event would report a
     * high-water mark for bytes the process may never have had mapped at once. */
    hv::Metrics m;
    m.on_alloc(1u << 30);
    m.on_alloc(1u << 30);
    check(m.peak_bytes() == 0, "peak: an allocation alone does not move it");

    m.sample(sample_of(1, 4096, 0, 1024, 0, 10));
    check(m.peak_bytes() == 4096,
          "peak: only what the live set held when a frame looked");
}

void test_an_idle_frame_reports_no_change() {
    hv::Metrics m;
    const hv::MetricsSample s = sample_of(100, 65536, 1000, 1u << 20, 0, 1000);

    check(m.sample(s), "idle: the first sample is a change");
    check(!m.sample(s), "idle: an identical one is not");

    /* Time passing is not a change. Nothing on this panel is a function of the
     * clock -- unlike the inspector's Lifetime, which is why that one ticks. */
    hv::MetricsSample later = s;
    later.now_ms = 5000;
    check(!m.sample(later), "idle: nor is the clock moving");

    /* Nor is a ring depth that does not move the percentage. On a 1 Mi ring the
     * raw depth changes almost every frame and the figure on screen does not. */
    hv::MetricsSample nudged = s;
    nudged.ring_queued = 1037;
    check(!m.sample(nudged),
          "idle: nor a ring depth that still reads the same percentage");

    hv::MetricsSample grown = s;
    grown.live_chunks = 101;
    check(m.sample(grown), "idle: a chunk arriving is");

    hv::MetricsSample lost = grown;
    lost.dropped = 1;
    check(m.sample(lost), "idle: and so is the first dropped event");
}

/* --- the derived figures ------------------------------------------------ */

void test_the_ring_percentage() {
    check(hv::ring_percent(0, 0) == 0, "ring: an unattached session is 0%");
    check(hv::ring_percent(512, 0) == 0, "ring: and so is a zero capacity");
    check(hv::ring_percent(0, 1024) == 0, "ring: an empty ring is 0%");
    check(hv::ring_percent(512, 1024) == 50, "ring: half is 50%");
    check(hv::ring_percent(1024, 1024) == 100, "ring: full is 100%");
    check(hv::ring_percent(9999, 1024) == 100,
          "ring: and it cannot read past full");

    /* The one that matters. A thousand events in a 1 Mi ring is 0.09%, and
     * truncating that to `0%` beside a queue that is not empty tells the reader
     * the telemetry path is idle. */
    check(hv::ring_percent(1000, 1u << 20) == 1,
          "ring: a non-empty ring never reads as empty");
    check(hv::ring_percent(1, 1u << 20) == 1,
          "ring: not even with one event in it");
}

void test_the_ring_pressure_thresholds() {
    check(hv::ring_pressure(0, 1024) == hv::Pressure::Calm, "press: empty");
    check(hv::ring_pressure(499, 1024) == hv::Pressure::Calm,
          "press: just under half is still calm");
    check(hv::ring_pressure(512, 1024) == hv::Pressure::Warn,
          "press: half is the warning");
    check(hv::ring_pressure(820, 1024) == hv::Pressure::Bad,
          "press: four fifths is the alarm");
    check(hv::ring_pressure(1024, 1024) == hv::Pressure::Bad, "press: full");
}

void test_the_fragmentation_badge() {
    check(hv::frag_badge(-1) == hv::FragBadge::Unknown,
          "badge: no analysis is not zero percent");
    check(hv::frag_badge(0) == hv::FragBadge::Low, "badge: 0% is Low");
    check(hv::frag_badge(14) == hv::FragBadge::Low, "badge: 14% is Low");
    check(hv::frag_badge(15) == hv::FragBadge::Med, "badge: 15% is Med");
    check(hv::frag_badge(39) == hv::FragBadge::Med, "badge: 39% is Med");
    check(hv::frag_badge(40) == hv::FragBadge::High, "badge: 40% is High");
    check(hv::frag_badge(100) == hv::FragBadge::High, "badge: 100% is High");
    check(std::strlen(hv::frag_badge_str(hv::FragBadge::Unknown)) == 0,
          "badge: the unknown one draws nothing rather than a word");
}

/* --- the split ---------------------------------------------------------- */

void test_the_split_never_starves_either_panel() {
    /* Wide: the ratio applies. */
    const int wide = hv::metrics_split(120, hv::kInspectorMinCols);
    check(wide == 48, "split: 120 columns gives the metrics two fifths");
    check(120 - wide >= hv::kInspectorMinCols,
          "split: leaving the inspector its minimum");

    /* Narrow: the minimum applies, and both still fit. */
    const int narrow = hv::metrics_split(80, hv::kInspectorMinCols);
    check(narrow >= hv::kMetricsMinCols,
          "split: 80 columns still gives the metrics a usable width");
    check(80 - narrow >= hv::kInspectorMinCols,
          "split: without taking the inspector below its own");

    /* Too narrow for both: the metrics panel goes rather than both being
     * squeezed into widths where every value is truncated. */
    check(hv::metrics_split(60, hv::kInspectorMinCols) == 0,
          "split: 60 columns is not enough for two panels");
    check(hv::metrics_split(0, hv::kInspectorMinCols) == 0,
          "split: nor is no terminal at all");

    /* The boundary, walked. There must be no width at which both panels are
     * given room and one of them gets less than it said it needed. */
    for (int w = 1; w <= 400; ++w) {
        const int mw = hv::metrics_split(w, hv::kInspectorMinCols);
        if (mw == 0) continue;
        if (mw < hv::kMetricsMinCols || w - mw < hv::kInspectorMinCols) {
            std::fprintf(stderr, "  FAIL split: %d columns split %d/%d\n", w,
                         w - mw, mw);
            ++g_failures;
            break;
        }
    }
}

/* --- the panel ---------------------------------------------------------- */

void test_the_panel_draws_the_metrics() {
    hv::Metrics m;
    m.on_alloc(4u << 20);
    m.on_alloc(8u << 20);
    m.sample(sample_of(4096, 12u << 20, 0, 65536, 0, 1000));
    m.sample(sample_of(1024, 3u << 20, 6553, 65536, 0, 2000));

    hv::Framebuffer fb;
    fb.resize(48, hv::kMetricsRows);
    fb.clear();
    const hv::Rect area{0, 0, 48, hv::kMetricsRows};
    m.draw(fb, area);
    const std::string out = rect_text(fb, area);

    check(out.find("TELEMETRY METRICS") != std::string::npos,
          "panel: it names itself");
    check(out.find("12 MB") != std::string::npos,
          "panel: the cumulative total, human-readable");
    check(out.find("12,582,912 B") != std::string::npos,
          "panel: with the exact count beside it at this width");
    check(out.find("1,024 chunks") != std::string::npos,
          "panel: the live count, thousands-separated");
    check(out.find("3 MB") != std::string::npos,
          "panel: and the bytes they hold");
    check(out.find("Peak") != std::string::npos &&
              out.find("12 MB (now -9 MB)") != std::string::npos,
          "panel: the peak, with how far below it the target now sits");
    check(out.find("9% of 65,536") != std::string::npos,
          "panel: the ring depth as a percentage of its capacity");
    check(out.find("0 dropped") != std::string::npos,
          "panel: and the drop count, present even at zero");
}

void test_the_panel_says_when_fragmentation_is_unknown() {
    hv::Metrics m;
    m.sample(sample_of(10, 4096, 0, 1024, 0, 100));

    hv::Framebuffer fb;
    fb.resize(48, hv::kMetricsRows);
    fb.clear();
    const hv::Rect area{0, 0, 48, hv::kMetricsRows};

    m.draw(fb, area);
    std::string out = rect_text(fb, area);
    check(out.find("Fragmented") != std::string::npos,
          "frag: the row is there before the analysis is");
    check(out.find("--") != std::string::npos,
          "frag: saying it is not known rather than claiming 0%");
    check(out.find("[Low]") == std::string::npos,
          "frag: and no badge, since there is nothing to badge");

    /* M5.4 supplies the number; everything above this line is what the panel
     * does until it does. */
    fb.clear();
    m.set_fragmentation(22);
    m.draw(fb, area);
    out = rect_text(fb, area);
    check(out.find("22%") != std::string::npos, "frag: then the figure");
    check(out.find("[Med]") != std::string::npos, "frag: and its badge");
    check(colour_of(fb, area, "22%") == m.style().warn,
          "frag: colour-coded to match the badge");

    fb.clear();
    m.set_fragmentation(70);
    m.draw(fb, area);
    check(colour_of(fb, area, "70%") == m.style().bad,
          "frag: a high figure is drawn in the alarm colour");
}

void test_the_ring_row_is_colour_coded() {
    hv::Metrics m;
    hv::Framebuffer fb;
    fb.resize(48, hv::kMetricsRows);
    const hv::Rect area{0, 0, 48, hv::kMetricsRows};

    m.sample(sample_of(1, 64, 1024, 65536, 0, 10));
    fb.clear();
    m.draw(fb, area);
    check(colour_of(fb, area, "1% of") == m.style().good,
          "ring: a quiet ring is drawn in the calm colour");

    m.sample(sample_of(1, 64, 40000, 65536, 0, 20));
    fb.clear();
    m.draw(fb, area);
    check(colour_of(fb, area, "61% of") == m.style().warn,
          "ring: past half it turns amber");

    m.sample(sample_of(1, 64, 60000, 65536, 0, 30));
    fb.clear();
    m.draw(fb, area);
    check(colour_of(fb, area, "91% of") == m.style().bad,
          "ring: past four fifths it turns red");
}

void test_dropped_events_are_impossible_to_miss() {
    hv::Metrics m;
    m.sample(sample_of(1000, 1u << 20, 100, 65536, 4271, 500));

    hv::Framebuffer fb;
    fb.resize(48, hv::kMetricsRows);
    fb.clear();
    const hv::Rect area{0, 0, 48, hv::kMetricsRows};
    m.draw(fb, area);
    const std::string out = rect_text(fb, area);

    check(out.find("4,271 DROPPED") != std::string::npos,
          "drop: the count is on the panel, in the word that means it");
    check(colour_of(fb, area, "4,271") == m.style().bad,
          "drop: in the alarm colour");

    /* Bold as well as red: M4.4 degrades a 16-colour terminal's palette, and
     * the one line on the panel that must survive that is this one. */
    for (int y = area.y; y < area.y + area.h; ++y) {
        const std::string row = row_text(fb, y);
        const std::size_t at = row.find("4,271");
        if (at == std::string::npos) continue;
        check((fb.at_back(static_cast<int>(at), y).attrs & hv::kAttrBold) != 0,
              "drop: and in bold, so it survives a 16-colour terminal");
        break;
    }
}

void test_a_narrow_panel_keeps_the_drop_count() {
    /* The narrowest the panel is ever drawn at. The ring's depth is a
     * curiosity; the drop count is the reason not to believe the four figures
     * above it, so it is the one that keeps the row. */
    hv::Metrics m;
    m.on_alloc(1u << 20);
    m.sample(sample_of(12, 4096, 60000, 65536, 9, 100));

    hv::Framebuffer fb;
    fb.resize(hv::kMetricsMinCols, hv::kMetricsRows);
    fb.clear();
    const hv::Rect area{0, 0, hv::kMetricsMinCols, hv::kMetricsRows};
    m.draw(fb, area);
    const std::string out = rect_text(fb, area);

    check(out.find("9 DROPPED") != std::string::npos,
          "narrow: the drop count survives the narrowest panel");
    check(out.find("91%") == std::string::npos,
          "narrow: it takes the row from the ring rather than sharing it");
    check(out.find("1,048,576 B") == std::string::npos,
          "narrow: exact byte counts are dropped whole, not cut mid-number");
    check(out.find("1 MB") != std::string::npos,
          "narrow: the human figure stays");
}

void test_a_panel_with_no_room_draws_nothing() {
    hv::Metrics m;
    m.sample(sample_of(12, 4096, 0, 65536, 0, 100));

    hv::Framebuffer fb;
    fb.resize(80, hv::kMetricsRows);
    fb.clear();

    /* One column under the minimum. Half a panel is not a smaller panel, it is
     * a column of truncated numbers next to a map that could have had the
     * space. */
    const hv::Rect area{0, 0, hv::kMetricsMinCols - 1, hv::kMetricsRows};
    m.draw(fb, area);
    check(rect_text(fb, area).find("TELEMETRY") == std::string::npos,
          "tiny: below its minimum the panel draws nothing at all");
}

void test_neither_panel_writes_into_the_other() {
    /* 80 columns, which is the width where the two are tightest, and the
     * inspector holding the value that prompted this check. `Real Size` starts
     * at column 12 and a megabyte chunk with inferred overhead runs 43
     * characters past it -- eight columns into the metrics panel. */
    constexpr int kW = 80;
    const int mw = hv::metrics_split(kW, hv::kInspectorMinCols);
    check(mw > 0, "split: 80 columns does carry both panels");

    const hv::Rect left{0, 0, kW - mw, hv::kInspectorRows};
    const hv::Rect right{kW - mw, 0, mw, hv::kMetricsRows};

    constexpr std::uint64_t kBase = 0x600000000000ull;
    constexpr std::uint64_t kCell = 4096;
    hv::Grid g;
    g.configure(kBase, kBase + kCell * 400, 40, 10);
    hv::HeatMap map;
    map.configure(g);

    hv::ChunkTable table;
    const std::uint64_t addr = kBase + kCell * 7;
    table.insert_live(addr, 1048576, 1052672, 100, 1);
    map.on_alloc(addr, 1048576, 1052672, 100);

    hv::MapCursor cur;
    cur.set_coord(g, addr);
    hv::ChunkInspector in;
    in.refresh(table, map, cur, 200, true);
    check(in.total() == 1, "bleed: the inspector has the long value to draw");

    hv::Metrics m;
    m.on_alloc(123456789);
    m.sample(sample_of(987654, 123456789, 60000, 65536, 4271, 100));

    hv::Framebuffer fb;
    fb.resize(kW, hv::kInspectorRows);
    fb.clear(hv::Cell{U'.', 0, 0, 0});

    in.draw(fb, left, g, cur, nullptr, 0);

    /* Every column the metrics panel owns must still be untouched. This is the
     * assertion `Framebuffer::text` cannot make for itself: it clips to the
     * screen, so an over-long inspector value is a perfectly legal write that
     * lands in the neighbour. */
    bool clean = true;
    for (int y = right.y; y < right.y + right.h; ++y)
        for (int x = right.x; x < right.x + right.w; ++x)
            if (fb.at_back(x, y).glyph != U'.') clean = false;
    check(clean, "bleed: the inspector stays inside its own columns");

    m.draw(fb, right);
    clean = true;
    for (int y = left.y; y < left.y + left.h; ++y)
        if (fb.at_back(left.x + left.w - 1, y).glyph == U'?') clean = false;
    check(clean, "bleed: and the metrics panel inside its");

    /* And the two together do actually fill the row, so this is not passing
     * because one of them drew nothing. */
    const std::string row = row_text(fb, 0);
    check(row.find("CHUNK INSPECTOR") != std::string::npos &&
              row.find("TELEMETRY METRICS") != std::string::npos,
          "bleed: with both panels present on the same row");
}

void test_panel_text_clips_to_its_rect() {
    hv::Framebuffer fb;
    fb.resize(40, 4);
    fb.clear(hv::Cell{U'.', 0, 0, 0});

    const hv::Rect area{10, 1, 10, 2};

    const int n = hv::panel_text(fb, area, 2, 0, "0123456789ABCDEF", 1, 2);
    check(n == 8, "clip: a long string is cut at the panel's edge");
    check(fb.at_back(19, 1).glyph == U'7', "clip: writing up to the last column");
    check(fb.at_back(20, 1).glyph == U'.', "clip: and not one past it");

    check(hv::panel_text(fb, area, 0, 5, "x", 1, 2) == 0,
          "clip: a row below the panel writes nothing");
    check(hv::panel_text(fb, area, 12, 0, "x", 1, 2) == 0,
          "clip: and so does a column past its right edge");

    /* Right-aligned text is dropped whole rather than losing its start, which
     * is where its meaning is: " 3 of 40 " cut to "of 40 " says nothing. */
    check(hv::panel_text_right(fb, area, 0, "0123456789AB", 1, 2) == 0,
          "clip: right-aligned text too long to fit is not drawn at all");
    check(hv::panel_text_right(fb, area, 0, "abcd", 1, 2) == 4,
          "clip: one that fits lands against the right edge");
    check(fb.at_back(19, 1).glyph == U'd', "clip: with its last character there");
}

} // namespace

int main() {
    test_the_total_is_cumulative();
    test_the_total_saturates();
    test_the_peak_is_a_high_water_mark();
    test_the_peak_is_per_frame_not_per_event();
    test_an_idle_frame_reports_no_change();
    test_the_ring_percentage();
    test_the_ring_pressure_thresholds();
    test_the_fragmentation_badge();
    test_the_split_never_starves_either_panel();
    test_the_panel_draws_the_metrics();
    test_the_panel_says_when_fragmentation_is_unknown();
    test_the_ring_row_is_colour_coded();
    test_dropped_events_are_impossible_to_miss();
    test_a_narrow_panel_keeps_the_drop_count();
    test_a_panel_with_no_room_draws_nothing();
    test_neither_panel_writes_into_the_other();
    test_panel_text_clips_to_its_rect();

    if (g_failures != 0) {
        std::fprintf(stderr, "metrics: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("metrics: all checks passed\n");
    return 0;
}
