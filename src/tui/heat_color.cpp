/* heapviz - heatmap aging (M3.4).
 *
 * Copyright (C) 2026 Parth Sinha
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "tui/heat_color.h"

#include <array>
#include <cmath>

namespace hv {

namespace {

float clamp01(float v) noexcept {
    if (v < 0.0f) return 0.0f;
    if (v > 1.0f) return 1.0f;
    return v;
}

/* sRGB's transfer function, not the 2.2 approximation. The linear toe matters
 * here: the empty colour is 0x2A2A2A, which sits inside it, and the tail of the
 * red-to-grey fade spends its last second down there. */
float srgb_to_linear(float v) noexcept {
    return v <= 0.04045f ? v / 12.92f : std::pow((v + 0.055f) / 1.055f, 2.4f);
}

/* Encoding is on the per-cell path and decoding is not, which is why only one
 * of the two directions is written as arithmetic.
 *
 * `kEncode[i]` is the linear value exactly halfway between output codes i and
 * i+1, so a linear value's code is the number of thresholds below it. That
 * definition *is* the rounding the closed form performs, so this is not an
 * approximation of the inverse transfer function and there is no accuracy
 * budget to argue about; `heat_color_test` spells out the closed form it
 * replaces and requires the two to agree exactly, everywhere.
 *
 * Finding the code by binary search over those 255 thresholds cost 10.6 ns per
 * channel, 32 ns of the 84 ns a cell then took -- eight unpredictable branches
 * is a poor way to compute a monotone function. `kGuess` indexes a candidate by
 * the square root of the linear value instead, which spreads the codes evenly
 * enough over 1024 bins that the guess is never more than one code out, and the
 * two comparisons below turn "never more than one out" into "exact". */
const std::array<float, 255> kEncode = [] {
    std::array<float, 255> t{};
    for (std::size_t i = 0; i < t.size(); ++i)
        t[i] = srgb_to_linear((static_cast<float>(i) + 0.5f) / 255.0f);
    return t;
}();

constexpr std::size_t kGuessBins = 1024;

const std::array<std::uint8_t, kGuessBins + 1> kGuess = [] {
    std::array<std::uint8_t, kGuessBins + 1> t{};
    for (std::size_t j = 0; j <= kGuessBins; ++j) {
        const float root   = static_cast<float>(j) / static_cast<float>(kGuessBins);
        const float linear = root * root;
        std::size_t code   = 0;
        while (code < kEncode.size() && linear >= kEncode[code]) ++code;
        t[j] = static_cast<std::uint8_t>(code);
    }
    return t;
}();

/* Correctness here belongs to the two loops, not to the guess: whatever index
 * they start from, they walk to the code the thresholds define. The square root
 * is purely what makes that walk short, and with 1024 bins it is provably at
 * most one step -- which is why replacing the loops with a single `if`, or
 * indexing linearly instead, both leave every correctness test passing. Neither
 * mutation is a behaviour change; only the cost ceiling in `heat_color_test`
 * stands against them, and it is deliberately loose. Said plainly here because
 * the alternative is someone "simplifying" this later and wondering why nothing
 * went red. */
std::uint32_t encode_channel(float linear) noexcept {
    if (!(linear > 0.0f)) return 0;   /* also catches NaN */
    if (linear >= 1.0f)   return 255;

    const auto  bin  = static_cast<std::size_t>(
        std::sqrt(linear) * static_cast<float>(kGuessBins));
    std::uint32_t code = kGuess[bin];

    while (code < 255 && linear >= kEncode[code]) ++code;
    while (code > 0 && linear < kEncode[code - 1]) --code;
    return code;
}

float channel(Rgb c, unsigned shift) noexcept {
    return static_cast<float>((c >> shift) & 0xFFu) / 255.0f;
}

/* Elapsed milliseconds since a stamp, or `none` if it never happened.
 *
 * Unsigned subtraction is deliberate: a stamp from the future wraps to
 * something near 2^32, which is larger than every window and therefore reads as
 * "long ago" rather than as a negative age needing a branch of its own. */
std::uint32_t age_of(std::uint32_t stamp, std::uint32_t now,
                     std::uint32_t none) noexcept {
    if (stamp == kNoTime) return none;
    return now - stamp;
}

Oklab scaled(const Oklab &c, float factor) noexcept {
    return Oklab{clamp01(c.L * factor), c.a, c.b};
}

} // namespace

Oklab srgb_to_oklab(Rgb c) noexcept {
    const float r = srgb_to_linear(channel(c, 16));
    const float g = srgb_to_linear(channel(c, 8));
    const float b = srgb_to_linear(channel(c, 0));

    const float l = 0.4122214708f * r + 0.5363325363f * g + 0.0514459929f * b;
    const float m = 0.2119034982f * r + 0.6806995451f * g + 0.1073969566f * b;
    const float s = 0.0883024619f * r + 0.2817188376f * g + 0.6299787005f * b;

    const float l_ = std::cbrt(l);
    const float m_ = std::cbrt(m);
    const float s_ = std::cbrt(s);

    return Oklab{0.2104542553f * l_ + 0.7936177850f * m_ - 0.0040720468f * s_,
                 1.9779984951f * l_ - 2.4285922050f * m_ + 0.4505937099f * s_,
                 0.0259040371f * l_ + 0.7827717662f * m_ - 0.8086757660f * s_};
}

Rgb oklab_to_srgb(const Oklab &c) noexcept {
    const float l_ = c.L + 0.3963377774f * c.a + 0.2158037573f * c.b;
    const float m_ = c.L - 0.1055613458f * c.a - 0.0638541728f * c.b;
    const float s_ = c.L - 0.0894841775f * c.a - 1.2914855480f * c.b;

    const float l = l_ * l_ * l_;
    const float m = m_ * m_ * m_;
    const float s = s_ * s_ * s_;

    const float r =  4.0767416621f * l - 3.3077115913f * m + 0.2309699292f * s;
    const float g = -1.2684380046f * l + 2.6097574011f * m - 0.3413193965f * s;
    const float b = -0.0041960863f * l - 0.7034186147f * m + 1.7076147010f * s;

    return (encode_channel(r) << 16) | (encode_channel(g) << 8) |
           encode_channel(b);
}

Oklab lerp(const Oklab &from, const Oklab &to, float t) noexcept {
    const float u = clamp01(t);
    return Oklab{from.L + (to.L - from.L) * u, from.a + (to.a - from.a) * u,
                 from.b + (to.b - from.b) * u};
}

Rgb lerp_oklab(Rgb from, Rgb to, float t) noexcept {
    return oklab_to_srgb(lerp(srgb_to_oklab(from), srgb_to_oklab(to), t));
}

float pulse_wave(float u) noexcept {
    const float x = clamp01(u);
    const float t = x <= 0.5f ? x * 2.0f : (1.0f - x) * 2.0f;
    const float inv = 1.0f - t;
    return 1.0f - inv * inv * inv;
}

bool cell_animating(const CellAggregate &a, const HeatTimings &t,
                    std::uint32_t now_ms) noexcept {
    return age_of(a.last_alloc_ms, now_ms, kNoTime) <
               t.malloc_pulse_ms + t.malloc_fade_ms ||
           age_of(a.last_free_ms, now_ms, kNoTime) <
               t.free_flash_ms + t.free_fade_ms;
}

HeatRamp::HeatRamp(const HeatPalette &p, const HeatTimings &t) noexcept
    : fresh_(srgb_to_oklab(p.fresh)),
      freed_(srgb_to_oklab(p.freed)),
      settled_(srgb_to_oklab(p.settled)),
      overhead_(srgb_to_oklab(p.overhead)),
      empty_(srgb_to_oklab(p.empty)),
      srgb_(p),
      t_(t) {
    for (unsigned i = 0; i <= kDensitySteps; ++i)
        settled_lut_[i] = oklab_to_srgb(lab_for_step(i));
}

/* Fill density, quantised. A cell holding one 64-byte chunk of a 4 KiB span
 * reads dimmer than one that is full, which is the difference between
 * "something is here" and "this is packed" -- and it is the only information
 * the map carries about how much of the address space is actually in use.
 *
 * Quantising is what lets the settled colour be a table lookup, and it is free
 * only if the steps cannot be seen -- a map is a field of adjacent cells at
 * slightly different densities, so a visible step is contour banding across the
 * whole display rather than one cell being slightly wrong. 128 steps over the
 * 0.215 of lightness between the dimmest and the fullest cell puts the largest
 * gap at 0.0031, which is the floor: that is one 8-bit code in each channel,
 * the smallest step sRGB can express at all. Doubling the steps again would
 * change nothing, and halving them to 64 takes it to 0.0060.
 *
 * Both paths quantise here, so the table and the arithmetic cannot disagree. */
unsigned HeatRamp::density_step(const CellAggregate &a,
                                std::uint64_t cell_bytes) noexcept {
    if (cell_bytes == 0) return kDensitySteps;
    const float density = clamp01(static_cast<float>(a.live_bytes) /
                                  static_cast<float>(cell_bytes));
    return static_cast<unsigned>(density * static_cast<float>(kDensitySteps) +
                                 0.5f);
}

Oklab HeatRamp::settled_lab(const CellAggregate &a,
                            std::uint64_t cell_bytes) const noexcept {
    if (a.n_live == 0) {
        /* Only reachable once M2.2 attributes header bytes to the cell that
         * physically holds them; through the event path alone, overhead always
         * arrives with a live allocation. See heatmap.h. */
        return a.overhead_bytes > 0 ? overhead_ : empty_;
    }
    return lab_for_step(density_step(a, cell_bytes));
}

Oklab HeatRamp::lab_for_step(unsigned step) const noexcept {
    const float density =
        static_cast<float>(step) / static_cast<float>(kDensitySteps);
    return scaled(settled_, kDensityFloor + (1.0f - kDensityFloor) * density);
}

Rgb HeatRamp::settled(const CellAggregate &a,
                      std::uint64_t cell_bytes) const noexcept {
    /* The common case by a wide margin: most cells, most frames, are not
     * animating, and this is the whole of their cost. The two non-live cases
     * are palette colours untouched, so they are returned as they were given
     * rather than round-tripped. */
    if (a.n_live == 0) return a.overhead_bytes > 0 ? srgb_.overhead : srgb_.empty;
    return settled_lut_[density_step(a, cell_bytes)];
}

Rgb HeatRamp::color(const CellAggregate &a, std::uint64_t cell_bytes,
                    std::uint32_t now_ms) const noexcept {
    /* Each stage below falls through to the one under it once its window has
     * passed, and every stage's last frame equals the next stage's first. That
     * property is what "no popping" means, and `heat_color_test` walks the
     * whole timeline a millisecond at a time to hold it. */

    const std::uint32_t free_span = t_.free_flash_ms + t_.free_fade_ms;
    const std::uint32_t free_age  = age_of(a.last_free_ms, now_ms, free_span);

    /* Free outranks malloc, matching `cell_state`'s precedence. Taken first
     * because the flash is a flat colour, and reaching it means none of the
     * arithmetic below has to happen. */
    if (free_age < t_.free_flash_ms) return srgb_.freed;

    const std::uint32_t alloc_span = t_.malloc_pulse_ms + t_.malloc_fade_ms;
    const std::uint32_t alloc_age  = age_of(a.last_alloc_ms, now_ms, alloc_span);

    /* Nothing is animating: the settled colour, by the shortest path. */
    if (alloc_age >= alloc_span && free_age >= free_span) {
        return settled(a, cell_bytes);
    }

    const Oklab base = settled_lab(a, cell_bytes);

    /* Malloc: a brightness pulse that begins and ends at plain green, then a
     * fade from that green into whatever the cell has settled at. */
    Oklab after_alloc = base;
    if (alloc_age < t_.malloc_pulse_ms) {
        const float u = t_.malloc_pulse_ms > 0
                            ? static_cast<float>(alloc_age) /
                                  static_cast<float>(t_.malloc_pulse_ms)
                            : 1.0f;
        after_alloc = scaled(fresh_, 1.0f + kPulsePeak * pulse_wave(u));
    } else if (alloc_age < alloc_span && t_.malloc_fade_ms > 0) {
        const float u = static_cast<float>(alloc_age - t_.malloc_pulse_ms) /
                        static_cast<float>(t_.malloc_fade_ms);
        after_alloc = lerp(fresh_, base, u);
    }

    /* The flash has passed; fade towards whatever the cell would otherwise
     * have been. On an emptied cell that target is the unallocated grey the
     * roadmap names; on a cell that still holds live chunks it is that cell's
     * own colour, which is the same rule and avoids a jump back to blue the
     * instant the flash ends. */
    if (free_age < free_span && t_.free_fade_ms > 0) {
        const float u = static_cast<float>(free_age - t_.free_flash_ms) /
                        static_cast<float>(t_.free_fade_ms);
        return oklab_to_srgb(lerp(freed_, after_alloc, u));
    }

    return oklab_to_srgb(after_alloc);
}

} // namespace hv
