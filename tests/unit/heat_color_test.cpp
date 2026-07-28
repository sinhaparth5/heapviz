/* heapviz - heatmap aging (M3.4).
 *
 * Copyright (C) 2026 Parth Sinha
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * What is hard to test here is not "is the colour right" -- nobody can assert
 * that a fade looks good -- but the two properties that make it look wrong when
 * they break, and neither is visible in a single sampled colour:
 *
 *   1. Continuity. A seam at a window boundary is one frame wide and reads as a
 *      flicker. The whole timeline is therefore walked a millisecond at a time
 *      and every consecutive pair is required to be perceptually adjacent.
 *   2. That the interpolation is perceptual at all. A naive sRGB lerp produces
 *      a colour at every t and asserts nothing; it just looks cheap. The test
 *      pins the artefact it causes -- lightness sagging off the straight line
 *      between the endpoints -- and requires the naive version to exhibit it,
 *      so the check cannot pass vacuously.
 *
 * The third thing is the encoder LUT, which is an optimisation standing where a
 * closed-form transfer function used to be. It is checked against an
 * independent implementation of that closed form, written out here so the two
 * cannot drift into agreeing by sharing a bug.
 */

#include "tui/heat_color.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

int g_failures = 0;

void check(bool ok, const std::string &what) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", what.c_str());
        ++g_failures;
    }
}

int r_of(hv::Rgb c) { return static_cast<int>((c >> 16) & 0xFF); }
int g_of(hv::Rgb c) { return static_cast<int>((c >> 8) & 0xFF); }
int b_of(hv::Rgb c) { return static_cast<int>(c & 0xFF); }

/* Perceptual distance, which is the only distance worth measuring here: two
 * colours 3 sRGB units apart in the dark are a visible step, and 3 units apart
 * in a bright cyan are invisible. */
float delta_e(hv::Rgb x, hv::Rgb y) {
    const hv::Oklab a = hv::srgb_to_oklab(x);
    const hv::Oklab b = hv::srgb_to_oklab(y);
    const float dL = a.L - b.L;
    const float da = a.a - b.a;
    const float db = a.b - b.b;
    return std::sqrt(dL * dL + da * da + db * db);
}

hv::Rgb lerp_naive_srgb(hv::Rgb from, hv::Rgb to, float t) {
    const auto mix = [t](int a, int b) {
        return static_cast<hv::Rgb>(
            static_cast<int>(static_cast<float>(a) +
                             (static_cast<float>(b - a)) * t + 0.5f));
    };
    return (mix(r_of(from), r_of(to)) << 16) | (mix(g_of(from), g_of(to)) << 8) |
           mix(b_of(from), b_of(to));
}

std::string hex(hv::Rgb c) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "#%06X", c & 0xFFFFFFu);
    return buf;
}

constexpr std::uint64_t kCellBytes = 4096;

/* The ramp converts its palette once, at construction, which is the whole point
 * of it being an object. Tests build one per call and do not care. */
hv::Rgb color_of(const hv::CellAggregate &a, const hv::HeatTimings &t,
                 const hv::HeatPalette &p, std::uint64_t cell_bytes,
                 std::uint32_t now_ms) {
    return hv::HeatRamp{p, t}.color(a, cell_bytes, now_ms);
}

hv::Rgb settled_of(const hv::CellAggregate &a, const hv::HeatPalette &p,
                   std::uint64_t cell_bytes) {
    return hv::HeatRamp{p}.settled(a, cell_bytes);
}

hv::CellAggregate live_cell(std::uint64_t bytes, std::uint32_t alloc_ms) {
    hv::CellAggregate a{};
    a.n_live        = 1;
    a.live_bytes    = bytes;
    a.last_alloc_ms = alloc_ms;
    return a;
}

/* --- the colour space ----------------------------------------------------- */

void test_oklab_round_trip() {
    /* Every grey, because the sRGB transfer function's linear toe is a separate
     * branch and the empty colour (0x2A2A2A) and the tail of the free fade both
     * live near it. */
    int worst = 0;
    for (int v = 0; v < 256; ++v) {
        const auto c = static_cast<hv::Rgb>((v << 16) | (v << 8) | v);
        const hv::Rgb back = hv::oklab_to_srgb(hv::srgb_to_oklab(c));
        worst = std::max(worst, std::abs(r_of(back) - v));
    }
    check(worst <= 1, "oklab: greys round-trip within one unit (worst " +
                          std::to_string(worst) + ")");

    const hv::HeatPalette p{};
    for (hv::Rgb c : {p.fresh, p.freed, p.settled, p.overhead, p.empty}) {
        const hv::Rgb back = hv::oklab_to_srgb(hv::srgb_to_oklab(c));
        check(std::abs(r_of(back) - r_of(c)) <= 1 &&
                  std::abs(g_of(back) - g_of(c)) <= 1 &&
                  std::abs(b_of(back) - b_of(c)) <= 1,
              "oklab: " + hex(c) + " round-trips to " + hex(back));
    }

    /* Lightness has to be ordered, or scaling it is meaningless. */
    check(hv::srgb_to_oklab(0x00000000).L < hv::srgb_to_oklab(0x00808080).L &&
              hv::srgb_to_oklab(0x00808080).L < hv::srgb_to_oklab(0x00FFFFFF).L,
          "oklab: L increases from black through mid-grey to white");
}

/* The closed form `oklab_to_srgb` replaced with a table, written independently
 * so a shared mistake cannot make the two agree. If this and the shipped
 * encoder ever disagree, the table is wrong: the arithmetic is the definition.
 */
hv::Rgb reference_oklab_to_srgb(const hv::Oklab &c) {
    const float l_ = c.L + 0.3963377774f * c.a + 0.2158037573f * c.b;
    const float m_ = c.L - 0.1055613458f * c.a - 0.0638541728f * c.b;
    const float s_ = c.L - 0.0894841775f * c.a - 1.2914855480f * c.b;

    const float l = l_ * l_ * l_;
    const float m = m_ * m_ * m_;
    const float s = s_ * s_ * s_;

    const float lin[3] = {
         4.0767416621f * l - 3.3077115913f * m + 0.2309699292f * s,
        -1.2684380046f * l + 2.6097574011f * m - 0.3413193965f * s,
        -0.0041960863f * l - 0.7034186147f * m + 1.7076147010f * s};

    hv::Rgb out = 0;
    for (int i = 0; i < 3; ++i) {
        float v = lin[i] <= 0.0031308f
                      ? lin[i] * 12.92f
                      : 1.055f * std::pow(lin[i], 1.0f / 2.4f) - 0.055f;
        v = std::min(1.0f, std::max(0.0f, v));
        out |= static_cast<hv::Rgb>(static_cast<int>(v * 255.0f + 0.5f))
               << (16 - 8 * i);
    }
    return out;
}

/* The encoder is a guessed index plus a correction, standing in for three pow()
 * calls on the per-cell path. It claims to be the same rounding, not an
 * approximation of it, so the bar is exact agreement -- across the sRGB cube
 * and, more importantly, across the out-of-gamut and near-black values the
 * ramps actually pass through, which is where a table's edges are wrong and
 * where a correction step that runs at most once would be caught short. */
void test_encoder_matches_the_closed_form() {
    int mismatches = 0;
    hv::Rgb first_bad = 0;

    const auto compare = [&](const hv::Oklab &lab) {
        const hv::Rgb ours = hv::oklab_to_srgb(lab);
        const hv::Rgb ref  = reference_oklab_to_srgb(lab);
        if (ours != ref && mismatches++ == 0) first_bad = ours;
    };

    /* Every 8-bit value on each axis, which is where the thresholds live. */
    for (int v = 0; v < 256; ++v) {
        for (unsigned sh = 0; sh <= 16; sh += 8)
            compare(hv::srgb_to_oklab(static_cast<hv::Rgb>(v) << sh));
        compare(hv::srgb_to_oklab(
            static_cast<hv::Rgb>((v << 16) | (v << 8) | v)));
    }

    /* And a sweep of Oklab itself, including values no sRGB colour maps to:
     * the fades produce intermediate labs, not round-tripped ones. */
    for (int li = 0; li <= 40; ++li) {
        for (int ai = -10; ai <= 10; ++ai) {
            for (int bi = -10; bi <= 10; ++bi) {
                compare(hv::Oklab{static_cast<float>(li) / 40.0f,
                                  static_cast<float>(ai) / 25.0f,
                                  static_cast<float>(bi) / 25.0f});
            }
        }
    }

    check(mismatches == 0,
          "encoder: the threshold table agrees with the closed form everywhere ("
              + std::to_string(mismatches) + " disagreements, first " +
              hex(first_bad) + ")");
}

/* A fade has to arrive at exactly the colour it is joining, or there is a
 * one-unit seam where it meets the flat region beyond -- which on a large area
 * of a single colour is visible. Nothing special-cases the endpoints; the
 * property comes from the round trip being exact, which is why it is worth
 * asserting rather than assuming. */
void test_lerp_endpoints_are_exact() {
    const hv::HeatPalette p{};
    check(hv::lerp_oklab(p.fresh, p.settled, 0.0f) == p.fresh,
          "lerp: t=0 is the source colour exactly");
    check(hv::lerp_oklab(p.fresh, p.settled, 1.0f) == p.settled,
          "lerp: t=1 is the destination colour exactly");
    check(hv::lerp_oklab(p.fresh, p.settled, -3.0f) == p.fresh &&
              hv::lerp_oklab(p.fresh, p.settled, 42.0f) == p.settled,
          "lerp: t outside [0,1] is clamped, not extrapolated");
}

/* The point of Oklab, stated as a property rather than as a colour table.
 *
 * A correct perceptual lerp moves lightness *linearly* from one endpoint to the
 * other: at t the result's L is L(from) + t*(L(to) - L(from)), exactly. A
 * per-channel sRGB lerp cannot, because the encoding is non-linear -- it sags
 * below that line, most in the middle, which is the fade reading as mud a
 * second in.
 *
 * Deviation from the line is therefore the measurement, and asserting that the
 * naive version fails it is what keeps this test from being vacuous: an
 * implementation could pass a "midpoint is between the endpoints" check while
 * doing nothing perceptual at all. */
void test_lerp_is_perceptual_not_naive() {
    const hv::HeatPalette p{};

    struct Ramp { const char *what; hv::Rgb from, to; };
    const Ramp ramps[] = {
        {"green -> blue (the malloc fade)", p.fresh, p.settled},
        {"red -> grey (the free fade)",     p.freed, p.empty},
    };

    for (const Ramp &r : ramps) {
        const float l_from = hv::srgb_to_oklab(r.from).L;
        const float l_to   = hv::srgb_to_oklab(r.to).L;

        float worst_ours  = 0.0f;
        float worst_naive = 0.0f;
        for (int i = 0; i <= 100; ++i) {
            const float t        = static_cast<float>(i) / 100.0f;
            const float expected = l_from + (l_to - l_from) * t;
            worst_ours  = std::max(worst_ours,
                                   std::fabs(hv::srgb_to_oklab(
                                       hv::lerp_oklab(r.from, r.to, t)).L - expected));
            worst_naive = std::max(worst_naive,
                                   std::fabs(hv::srgb_to_oklab(
                                       lerp_naive_srgb(r.from, r.to, t)).L - expected));
        }

        char msg[192];
        std::snprintf(msg, sizeof msg,
                      "lerp: %s is linear in lightness (off by %.4f)",
                      r.what, static_cast<double>(worst_ours));
        check(worst_ours < 0.004f, msg);

        std::snprintf(msg, sizeof msg,
                      "lerp: a naive sRGB %s measurably is not (off by %.4f), "
                      "so the check above is not vacuous",
                      r.what, static_cast<double>(worst_naive));
        check(worst_naive > 0.008f, msg);
    }
}

/* --- the ramps ------------------------------------------------------------ */

void test_pulse_wave_starts_and_ends_where_the_fade_does() {
    check(hv::pulse_wave(0.0f) == 0.0f && hv::pulse_wave(1.0f) == 0.0f,
          "pulse: no boost at either end, so it joins what surrounds it");
    check(std::fabs(hv::pulse_wave(0.5f) - 1.0f) < 1e-6f,
          "pulse: peaks in the middle");
    check(hv::pulse_wave(0.25f) > 0.0f && hv::pulse_wave(0.25f) < 1.0f &&
              std::fabs(hv::pulse_wave(0.25f) - hv::pulse_wave(0.75f)) < 1e-6f,
          "pulse: a triangle, symmetric about the peak");
}

void test_states_get_the_colours_they_are_named_for() {
    const hv::HeatTimings  t{};
    const hv::HeatPalette  p{};
    const hv::CellAggregate fresh = live_cell(kCellBytes, 1000);

    /* Mid-pulse: green, and brighter than plain green. */
    const hv::Rgb peak = color_of(fresh, t, p, kCellBytes, 1100);
    check(g_of(peak) > r_of(peak) && g_of(peak) > b_of(peak),
          "malloc: the pulse is green");
    check(hv::srgb_to_oklab(peak).L > hv::srgb_to_oklab(p.fresh).L + 0.02f,
          "malloc: the middle of the pulse is brighter than settled green");

    /* Long after every window: settled blue. */
    const hv::Rgb old = color_of(fresh, t, p, kCellBytes, 60000);
    check(b_of(old) > r_of(old) && b_of(old) > g_of(old),
          "live: a long-lived allocation is blue");
    check(old == settled_of(fresh, p, kCellBytes),
          "live: past every window the colour is exactly the settled colour");

    hv::CellAggregate freed{};
    freed.last_free_ms = 1000;
    check(color_of(freed, t, p, kCellBytes, 1100) == p.freed,
          "free: the flash is the palette's red, held flat");
    check(color_of(freed, t, p, kCellBytes, 1299) == p.freed,
          "free: still red one millisecond before the flash ends");

    const hv::Rgb faded = color_of(freed, t, p, kCellBytes, 1000 + 5000);
    check(faded == p.empty, "free: the fade ends at the unallocated colour");

    hv::CellAggregate empty{};
    check(color_of(empty, t, p, kCellBytes, 1234) == p.empty,
          "empty: a cell nothing ever touched is the unallocated colour");

    hv::CellAggregate overhead{};
    overhead.overhead_bytes = 16;
    check(color_of(overhead, t, p, kCellBytes, 1234) == p.overhead,
          "overhead: header bytes with no payload get the overhead colour");
}

void test_free_outranks_a_newer_malloc() {
    const hv::HeatTimings t{};
    const hv::HeatPalette p{};

    /* The awkward case M3.3 fixes: the malloc is the more recent event and the
     * free still wins. Pinned here too, because the colour rule could
     * plausibly be written to take the newest timestamp instead. */
    hv::CellAggregate a = live_cell(kCellBytes, 950);
    a.last_free_ms      = 900;
    check(color_of(a, t, p, kCellBytes, 1000) == p.freed,
          "precedence: a free 100 ms ago beats a malloc 50 ms ago");
}

/* The settled colour is tabulated against a quantised density, which is what
 * makes the common case a lookup. Quantisation is only free if the steps cannot
 * be seen: a map is a field of adjacent cells at slightly different densities,
 * so a step above the threshold of perception shows up as contour banding
 * across the whole display, not as one cell being slightly off. */
void test_density_steps_are_invisible() {
    const hv::HeatPalette p{};
    const hv::HeatRamp    ramp{p};

    float   worst    = 0.0f;
    hv::Rgb worst_a  = 0;
    hv::Rgb worst_b  = 0;
    hv::Rgb prev     = ramp.settled(live_cell(0, 0), kCellBytes);

    for (std::uint64_t bytes = 1; bytes <= kCellBytes; ++bytes) {
        const hv::Rgb cur = ramp.settled(live_cell(bytes, 0), kCellBytes);
        const float   d   = delta_e(prev, cur);
        if (d > worst) { worst = d; worst_a = prev; worst_b = cur; }
        prev = cur;
    }

    char msg[192];
    std::snprintf(msg, sizeof msg,
                  "density: the biggest step between adjacent densities is "
                  "%.4f, under the ~0.01 that becomes visible (%s -> %s)",
                  static_cast<double>(worst), hex(worst_a).c_str(),
                  hex(worst_b).c_str());
    check(worst < 0.005f, msg);
}

void test_density_scales_brightness() {
    const hv::HeatPalette p{};

    const hv::Rgb full  = settled_of(live_cell(kCellBytes, 0), p, kCellBytes);
    const hv::Rgb half  = settled_of(live_cell(kCellBytes / 2, 0), p, kCellBytes);
    const hv::Rgb sliver = settled_of(live_cell(64, 0), p, kCellBytes);

    const float lf = hv::srgb_to_oklab(full).L;
    const float lh = hv::srgb_to_oklab(half).L;
    const float ls = hv::srgb_to_oklab(sliver).L;

    check(lf > lh && lh > ls, "density: a fuller cell is brighter");
    check(ls > hv::srgb_to_oklab(p.empty).L + 0.05f,
          "density: even one small chunk stays clearly brighter than empty");

    /* Monotone the whole way, not merely at three points. */
    bool monotone = true;
    float prev = 0.0f;
    for (int i = 0; i <= 64; ++i) {
        const auto bytes = static_cast<std::uint64_t>(i) * kCellBytes / 64;
        const float l = hv::srgb_to_oklab(
            settled_of(live_cell(bytes, 0), p, kCellBytes)).L;
        if (i > 0 && l < prev - 0.001f) monotone = false;
        prev = l;
    }
    check(monotone, "density: brightness never goes backwards as a cell fills");

    /* Over-full is possible: usable bytes exceed the span when a chunk starts
     * inside this cell and ends in the next one. It must clamp, not overflow
     * into something brighter than full. */
    check(settled_of(live_cell(kCellBytes * 9, 0), p, kCellBytes) == full,
          "density: a cell holding more than it spans clamps to full");

    /* A degenerate grid must not blank the map. */
    check(settled_of(live_cell(4096, 0), p, 0) == full,
          "density: cell_bytes of zero reads as full brightness, not black");
}

/* The DoD, mechanised: "colours age smoothly with no popping".
 *
 * Every window boundary is a branch, and a branch that starts its output where
 * the previous one stopped is the difference between a fade and a flicker.
 * Walking the timeline catches all of them at once, including boundaries a
 * future edit adds. */
void test_no_popping_anywhere_on_the_timeline() {
    const hv::HeatTimings t{};
    const hv::HeatPalette p{};

    /* `from` is the timestamp of the cell's most recent event. Walking from
     * before it would be walking a timeline where the aggregate already knows
     * about something that has not happened yet, and the step at the moment it
     * "arrives" is an event, not a rendering seam. */
    struct Case {
        const char       *what;
        std::uint32_t     from;
        hv::CellAggregate cell;
    };

    hv::CellAggregate freed_empty{};
    freed_empty.last_free_ms = 1000;

    hv::CellAggregate freed_but_live = live_cell(kCellBytes / 2, 1000);
    freed_but_live.last_free_ms      = 1000;

    hv::CellAggregate reallocated = live_cell(kCellBytes, 1400);
    reallocated.last_free_ms      = 1000;

    const std::vector<Case> cases{
        {"a live cell aging from its malloc", 1000, live_cell(kCellBytes / 2, 1000)},
        {"a cell emptied by a free",          1000, freed_empty},
        {"a cell freed that still holds live chunks", 1000, freed_but_live},
        {"a cell freed and then reallocated", 1400, reallocated},
    };

    /* One millisecond is finer than any frame at 60 fps, so a step small here
     * is smaller still on screen. The bound is a perceptual distance: 0.01 in
     * Oklab is around the threshold of a just-noticeable difference for a large
     * flat patch, and every legitimate step in these ramps is far below it. */
    constexpr float kMaxStep = 0.01f;

    for (const Case &c : cases) {
        float         worst    = 0.0f;
        std::uint32_t worst_at = 0;
        hv::Rgb       prev     = color_of(c.cell, t, p, kCellBytes, c.from);

        for (std::uint32_t now = c.from; now <= c.from + 4000; ++now) {
            const hv::Rgb cur = color_of(c.cell, t, p, kCellBytes, now);
            const float   d   = delta_e(prev, cur);
            if (d > worst) { worst = d; worst_at = now - c.from; }
            prev = cur;
        }

        char msg[192];
        std::snprintf(msg, sizeof msg,
                      "smooth: %s -- worst step %.4f at %u ms (limit %.2f)",
                      c.what, static_cast<double>(worst), worst_at,
                      static_cast<double>(kMaxStep));
        check(worst < kMaxStep, msg);
    }
}

/* Pure in (aggregate, now): the same inputs give the same colour however many
 * times they are asked, and nothing accumulates between calls. If any state
 * were being kept, walking the timeline backwards would disagree with walking
 * it forwards. */
void test_colour_is_a_pure_function_of_time() {
    const hv::HeatTimings   t{};
    const hv::HeatPalette   p{};
    const hv::CellAggregate a = live_cell(kCellBytes / 3, 1000);

    std::vector<hv::Rgb> forward;
    for (std::uint32_t now = 1000; now < 3000; now += 7)
        forward.push_back(color_of(a, t, p, kCellBytes, now));

    std::size_t i = forward.size();
    bool        same = true;
    for (std::uint32_t now = 1000 + 7 * static_cast<std::uint32_t>(i - 1);
         i-- > 0; now -= 7) {
        if (color_of(a, t, p, kCellBytes, now) != forward[i]) same = false;
    }
    check(same, "purity: replaying the timeline backwards gives identical colours");

    /* And it does change with time alone -- a constant function would pass
     * every assertion above. */
    check(color_of(a, t, p, kCellBytes, 1100) !=
              color_of(a, t, p, kCellBytes, 1600),
          "purity: the colour does move as the clock does, with no events");
}

/* Tuning is meant to happen by editing HeatTimings and nothing else. */
void test_durations_come_only_from_the_timings_struct() {
    const hv::HeatPalette p{};

    hv::HeatTimings slow;
    slow.malloc_pulse_ms = 1000;
    slow.malloc_fade_ms  = 4000;
    slow.free_flash_ms   = 2000;
    slow.free_fade_ms    = 8000;

    const hv::CellAggregate a = live_cell(kCellBytes, 0);

    /* At 900 ms the default timings are long past the pulse; the slow ones are
     * still inside it. */
    check(color_of(a, hv::kDefaultTimings, p, kCellBytes, 900) !=
              color_of(a, slow, p, kCellBytes, 900),
          "timings: stretching the windows stretches the ramp");

    hv::CellAggregate freed{};
    freed.last_free_ms = 0;
    check(color_of(freed, slow, p, kCellBytes, 1900) == p.freed,
          "timings: the flash length is read from the struct, not hardcoded");

    /* Zeroed durations are a legitimate configuration (a --no-animation mode is
     * ROADMAP M6.4) and must not divide by zero or leave a colour stuck. */
    hv::HeatTimings none{};
    none.malloc_pulse_ms = 0;
    none.malloc_fade_ms  = 0;
    none.free_flash_ms   = 0;
    none.free_fade_ms    = 0;
    check(color_of(a, none, p, kCellBytes, 0) ==
              settled_of(a, p, kCellBytes),
          "timings: all-zero durations mean no animation, not a hang");
    check(color_of(freed, none, p, kCellBytes, 0) == p.empty,
          "timings: a zero flash window shows no flash");
}

/* A stamp the map has never set, and one from the future (a clock the caller
 * computed backwards). Neither may be treated as "just happened". */
void test_absent_and_future_stamps() {
    const hv::HeatTimings t{};
    const hv::HeatPalette p{};

    hv::CellAggregate a = live_cell(kCellBytes, 0);
    a.last_alloc_ms     = hv::kNoTime;
    a.last_free_ms      = hv::kNoTime;
    check(color_of(a, t, p, kCellBytes, 0) ==
              settled_of(a, p, kCellBytes),
          "sentinel: kNoTime is not an event at time 4294967295");

    hv::CellAggregate future = live_cell(kCellBytes, 5000);
    future.last_free_ms      = 5000;
    check(color_of(future, t, p, kCellBytes, 4000) ==
              settled_of(future, p, kCellBytes),
          "sentinel: a stamp in the future ages out rather than flashing");
}

/* `animating` is what lets the map leave settled cells alone, and what
 * `LoopApp::animating` will report. It has to agree with `color` exactly: a
 * cell reported settled whose colour is still moving is a cell that freezes
 * mid-fade until something else forces a repaint. */
void test_animating_agrees_with_the_colour() {
    const hv::HeatTimings t{};
    const hv::HeatRamp    ramp{};

    hv::CellAggregate a = live_cell(kCellBytes / 2, 1000);
    a.last_free_ms      = 1000;

    bool          agreed  = true;
    std::uint32_t disagreed_at = 0;
    hv::Rgb       prev    = ramp.color(a, kCellBytes, 1000);

    /* Visibly changing, not bit-changing: the last millisecond of a fade lands
     * a rounding step away from the colour it settles at, and calling that
     * "still animating" would keep a whole screen of cells redrawing forever. */
    for (std::uint32_t now = 1001; now <= 1000 + 4000; ++now) {
        const hv::Rgb cur     = ramp.color(a, kCellBytes, now);
        const bool    moving  = delta_e(prev, cur) > 0.002f;
        if (moving && !hv::cell_animating(a, t, now)) {
            agreed = false;
            if (disagreed_at == 0) disagreed_at = now - 1000;
        }
        prev = cur;
    }
    check(agreed, "animating: never reports settled while the colour is still "
                  "changing (first at " + std::to_string(disagreed_at) + " ms)");

    hv::CellAggregate quiet{};
    check(!hv::cell_animating(quiet, t, 12345),
          "animating: a cell nothing ever touched is not animating");
    check(hv::cell_animating(live_cell(64, 1000), t, 1050),
          "animating: a cell mid-pulse is");
    check(!hv::cell_animating(live_cell(64, 1000), t, 9000),
          "animating: and stops once every window has passed");
}

/* Colour is computed per cell per frame, so its cost is a frame-budget item and
 * not an afterthought: 10000 cells at 60 Hz against a 1 ms budget leaves about
 * 100 ns per cell for *everything*, colour included.
 *
 * The first implementation took the palette as sRGB and converted inside,
 * costing 206 ns per cell -- 2.1 ms for a 200x50 map, twice the whole budget.
 * The bound below is deliberately loose: it is not a target, it is the line
 * past which the per-cell path has grown a conversion or an allocation again,
 * and it wants to survive a shared runner rather than teach people to re-run
 * the suite. The measured figure is printed either way. */
void test_cost_per_cell() {
    constexpr int kCells  = 200 * 50; /* the roadmap's reference terminal */
    constexpr int kFrames = 50;

    /* Two workloads, because they are two different code paths and the honest
     * answer is different for each. "Storm" is every cell inside a window at
     * once, which is what heavy churn across the whole address space looks
     * like. "Steady" is every cell past every window, the common case, and the
     * one the density table exists for. */
    std::vector<hv::CellAggregate> storm(kCells);
    std::vector<hv::CellAggregate> steady(kCells);
    for (int i = 0; i < kCells; ++i) {
        const auto j = static_cast<std::size_t>(i);
        storm[j] = live_cell(static_cast<std::uint64_t>(i % 4096),
                             static_cast<std::uint32_t>(3000 - i % 1000));
        if (i % 3 == 0)
            storm[j].last_free_ms = static_cast<std::uint32_t>(3000 - i % 2300);
        if (i % 7 == 0) storm[j].n_live = 0;

        steady[j] = live_cell(static_cast<std::uint64_t>(i % 4096), 0);
        if (i % 7 == 0) steady[j].n_live = 0;
    }

    const hv::HeatRamp ramp{};

    const auto measure = [&](const std::vector<hv::CellAggregate> &cells) {
        hv::Rgb sink = 0;

        /* One untimed pass first. Without it the first workload measured pays
         * for cold caches and a CPU still at its idle clock, and reads 40%
         * slower than the same code measured second. */
        for (const hv::CellAggregate &c : cells) sink ^= ramp.color(c, kCellBytes, 3000);

        const auto t0 = std::chrono::steady_clock::now();
        for (int f = 0; f < kFrames; ++f) {
            const auto now = static_cast<std::uint32_t>(3000 + f);
            for (const hv::CellAggregate &c : cells)
                sink ^= ramp.color(c, kCellBytes, now);
        }
        const auto t1 = std::chrono::steady_clock::now();
        check(sink != 0xFFFFFFFFu, "cost: the loop was not optimised away");
        return static_cast<double>(
                   std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0)
                       .count()) /
               static_cast<double>(kFrames * kCells);
    };

    const double ns_storm  = measure(storm);
    const double ns_steady = measure(steady);

    std::printf("heat_color: %.1f ns/cell storm (%.0f us/frame), "
                "%.1f ns/cell steady (%.0f us/frame)\n",
                ns_storm, ns_storm * kCells / 1000.0, ns_steady,
                ns_steady * kCells / 1000.0);

#ifdef NDEBUG
    /* Deliberately loose -- roughly 2x the measured figures. These are not
     * targets, they are the lines past which the per-cell path has grown a
     * colour-space conversion or an allocation again. An absolute timing
     * assertion with a 20% margin is the one this repo already learned not to
     * write: `chunk_table` measured the same unchanged code at 25 ns idle and
     * 36 ns under load, and a test that fails for reasons unrelated to the
     * change under test teaches people to re-run it. The measured figures are
     * printed either way. */
    char msg[160];
    std::snprintf(msg, sizeof msg,
                  "cost: %.1f ns/cell in a storm is within the 150 ns ceiling",
                  ns_storm);
    check(ns_storm < 150.0, msg);

    std::snprintf(msg, sizeof msg,
                  "cost: %.1f ns/cell settled is within the 25 ns ceiling",
                  ns_steady);
    check(ns_steady < 25.0, msg);
#endif
}

} // namespace

int main() {
    test_oklab_round_trip();
    test_encoder_matches_the_closed_form();
    test_lerp_endpoints_are_exact();
    test_lerp_is_perceptual_not_naive();
    test_pulse_wave_starts_and_ends_where_the_fade_does();
    test_states_get_the_colours_they_are_named_for();
    test_free_outranks_a_newer_malloc();
    test_density_steps_are_invisible();
    test_density_scales_brightness();
    test_no_popping_anywhere_on_the_timeline();
    test_colour_is_a_pure_function_of_time();
    test_durations_come_only_from_the_timings_struct();
    test_absent_and_future_stamps();
    test_animating_agrees_with_the_colour();
    test_cost_per_cell();

    if (g_failures != 0) {
        std::fprintf(stderr, "heat_color: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("heat_color: all checks passed\n");
    return 0;
}
