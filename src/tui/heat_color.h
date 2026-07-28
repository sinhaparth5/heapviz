/* heapviz - heatmap aging (M3.4).
 *
 * Copyright (C) 2026 Parth Sinha
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Turns a cell aggregate and the current time into the colour to draw it with.
 *
 * A PURE FUNCTION, NOT AN ANIMATION SYSTEM
 * ----------------------------------------
 * There is no per-cell animation state, no timer, no list of cells currently
 * fading, and nothing to advance per frame. `HeatRamp::color(aggregate, now)`
 * is total: every colour in the session is derived from the two timestamps
 * M3.3 already stores and the clock. A cell stops being red because time
 * passed, not because something updated it.
 *
 * That is worth more than the code it saves. An animation list is a second
 * model of the heap that has to be kept consistent with the first, and it is
 * exactly the kind of thing that leaks entries, or holds a stale index into a
 * grid that a SIGWINCH just resized. Here, a resize throws the aggregates away
 * and the colours are simply recomputed from whatever the rebuild produced.
 *
 * WHY OKLAB
 * ---------
 * The two long fades cross most of the colour wheel: green to blue, and red to
 * grey. Interpolating those channel-by-channel in sRGB goes wrong twice over.
 * The encoding is non-linear, so a fade between two bright colours sags darker
 * in the middle than either end; and even in linear light, RGB's axes are not
 * perceptual, so the path from green to blue slumps through a desaturated
 * slate. Both artefacts land in the middle of the fade, which is precisely
 * where the eye is watching. Oklab's straight lines look straight.
 *
 * WHY A CLASS AND NOT A FREE FUNCTION
 * -----------------------------------
 * Colour is computed per cell per frame -- ten thousand times at 60 Hz -- so
 * where the conversions happen decides whether this fits in the frame budget.
 * The first version took the palette as sRGB and converted inside: three cube
 * roots and three pow() calls per cell, 206 ns, 2.1 ms for a 200x50 map. That
 * is twice the entire frame budget spent on arithmetic whose inputs are five
 * constants.
 *
 * `HeatRamp` converts the palette once, at construction, and every ramp is
 * computed in Oklab, so at most one conversion back to sRGB happens per cell --
 * and a settled cell does not even pay that, because the settled colours are
 * tabulated by density at construction too. The result is 6.7 ns for a settled
 * cell and 46 ns for an animating one: 67 us and 460 us respectively for a
 * whole 200x50 map, against a 1 ms frame. That is why this is a small object
 * you build once rather than a function you call with a palette.
 */

#ifndef HEAPVIZ_TUI_HEAT_COLOR_H
#define HEAPVIZ_TUI_HEAT_COLOR_H

#include "tui/framebuffer.h"
#include "tui/heatmap.h"

#include <array>
#include <cstdint>

namespace hv {

/* --- Oklab --------------------------------------------------------------- */

/* Björn Ottosson's Oklab. L is perceptual lightness in [0,1]; a and b are the
 * green-red and blue-yellow axes, roughly +/-0.4. */
struct Oklab {
    float L = 0.0f;
    float a = 0.0f;
    float b = 0.0f;
};

Oklab srgb_to_oklab(Rgb c) noexcept;

/* Out-of-gamut results are clamped per channel, which is the standard cheap
 * answer and is correct for the ramps here: they interpolate between two
 * in-gamut colours, so any excursion is small. */
Rgb oklab_to_srgb(const Oklab &c) noexcept;

/* `t` is clamped to [0,1], so callers may hand these a raw ratio. */
Oklab lerp(const Oklab &from, const Oklab &to, float t) noexcept;
Rgb   lerp_oklab(Rgb from, Rgb to, float t) noexcept;

/* --- the palette --------------------------------------------------------- */

/* The five colours the map is drawn from, at ROADMAP M6.1's token values. They
 * are a struct rather than constants so M6.1's `--theme` can supply a different
 * set without any of the aging logic knowing. */
struct HeatPalette {
    Rgb fresh    = 0x004EC94E; /* malloc   - a new allocation             */
    Rgb freed    = 0x00E01B24; /* freed    - memory that just went away   */
    Rgb settled  = 0x003584E4; /* active   - a long-lived allocation      */
    Rgb overhead = 0x00F6D32D; /* overhead - chunk headers, no payload    */
    Rgb empty    = 0x002A2A2A; /* unalloc  - address space nobody owns    */
};

constexpr HeatPalette kDefaultPalette{};

/* How much brighter the peak of the malloc pulse is than settled green, and how
 * dim an almost-empty cell gets.
 *
 * The floor is set by a number rather than by taste: the unallocated grey sits
 * at Oklab L=0.285 and settled blue at L=0.614, so a floor of 0.65 puts the
 * dimmest live cell at L=0.40 -- a tenth of the lightness scale clear of empty,
 * which is what "something is here" has to survive being. A floor of 0.45,
 * tried first, lands the sliver at L=0.285: exactly the empty colour, so a cell
 * holding a single small chunk vanished. */
constexpr float kPulsePeak    = 0.35f;
constexpr float kDensityFloor = 0.65f;

/* A triangle wave over [0,1]: zero at both ends, peaking in the middle. Ending
 * where it starts is what lets the pulse be joined to the fade that follows
 * without a visible step. */
float pulse_wave(float u) noexcept;

/* Whether this cell's colour is still changing with the clock, i.e. whether it
 * is inside any window. Two callers want it: `LoopApp::animating`, which decides
 * whether a frame needs drawing at all, and the map draw, which can leave a
 * settled cell's colour alone until its aggregate changes. Cheap enough to ask
 * per cell -- two comparisons, no colour arithmetic. */
bool cell_animating(const CellAggregate &a, const HeatTimings &t,
                    std::uint32_t now_ms) noexcept;

/* --- the ramp ------------------------------------------------------------ */

class HeatRamp {
public:
    explicit HeatRamp(const HeatPalette &p = kDefaultPalette,
                      const HeatTimings &t = kDefaultTimings) noexcept;

    void               set_timings(const HeatTimings &t) noexcept { t_ = t; }
    const HeatTimings &timings() const noexcept { return t_; }

    /* The whole aging rule.
     *
     * `cell_bytes` is the grid's granularity, i.e. the address span this cell
     * stands for; `live_bytes / cell_bytes` is the fill density that scales a
     * settled cell's brightness. Zero is accepted and read as "no density
     * information", giving full brightness -- an invalid grid must not blank
     * the display.
     *
     * The result agrees with `cell_state()` by construction: the flash windows
     * here are the same two fields, so a cell reporting `RecentFree` is red. */
    Rgb color(const CellAggregate &a, std::uint64_t cell_bytes,
              std::uint32_t now_ms) const noexcept;

    /* The colour a cell settles at once nothing is flashing: blue scaled by
     * density, or the overhead/empty colours. Exposed because both fades
     * interpolate towards it and a test needs to be able to name the target. */
    Rgb settled(const CellAggregate &a, std::uint64_t cell_bytes) const noexcept;

private:
    /* Density is quantised to this many steps so a settled cell's colour is a
     * table lookup rather than a colour-space conversion. See the .cpp for why
     * 128 steps is the point past which more would buy nothing. */
    static constexpr unsigned kDensitySteps = 128;

    static unsigned density_step(const CellAggregate &a,
                                 std::uint64_t cell_bytes) noexcept;
    Oklab lab_for_step(unsigned step) const noexcept;
    Oklab settled_lab(const CellAggregate &a,
                      std::uint64_t cell_bytes) const noexcept;

    Oklab fresh_;
    Oklab freed_;
    Oklab settled_;
    Oklab overhead_;
    Oklab empty_;

    /* Kept in sRGB as well, so the three states that are a palette colour
     * exactly -- the flash, and the two non-live settled colours -- return it
     * bit-for-bit rather than by round-tripping through Oklab. */
    HeatPalette srgb_;
    HeatTimings t_;

    std::array<Rgb, kDensitySteps + 1> settled_lut_{};
};

} // namespace hv

#endif /* HEAPVIZ_TUI_HEAT_COLOR_H */
