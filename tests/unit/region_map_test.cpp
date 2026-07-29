/* heapviz - region packing checks (M2.4).
 *
 * Copyright (C) 2026 Parth Sinha
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The contract is a bijection: every byte of every allocatable region has
 * exactly one packed offset, every packed offset has exactly one real address,
 * and the two conversions undo each other. Most of what follows checks that
 * across whole regions rather than at sampled points, because an off-by-one in
 * a cumulative offset produces a mapping that is wrong from the second region
 * onwards and entirely plausible in any example taken from the first.
 *
 * The other half is the change signal. Returning true when nothing moved costs
 * a full HeatMap rebuild every scan; returning false when something did move
 * leaves cells describing an arena that is no longer at that offset, which is
 * worse -- the display stays confident and becomes wrong.
 */

#include "tui/region_map.h"

#include <cstdio>
#include <string>
#include <vector>

namespace {

int g_failures = 0;

void check(bool ok, const char *what) {
    if (!ok) { std::fprintf(stderr, "  FAIL %s\n", what); ++g_failures; }
}

hv::Region reg(std::uint64_t start, std::uint64_t end, hv::RegionKind k) {
    hv::Region r;
    r.start = start;
    r.end   = end;
    r.kind  = k;
    r.perms = hv::kPermRead | hv::kPermWrite | hv::kPermPrivate;
    return r;
}

/* A brk heap low down and two thread arenas far away, which is the layout that
 * makes the union useless and this class necessary. */
std::vector<hv::Region> scattered() {
    return {
        reg(0x5b0000000000ull, 0x5b0000021000ull, hv::RegionKind::Heap),  /* 132 KB */
        reg(0x5b0000021000ull, 0x5b0000030000ull, hv::RegionKind::File),  /* ignored */
        reg(0x7f0000000000ull, 0x7f0000100000ull, hv::RegionKind::Anon),  /* 1 MiB  */
        reg(0x7f4000000000ull, 0x7f4000040000ull, hv::RegionKind::Anon),  /* 256 KB */
        reg(0x7ffd00000000ull, 0x7ffd00021000ull, hv::RegionKind::Stack), /* ignored */
    };
}

/* --- packing --------------------------------------------------------------- */

void test_only_allocatable_regions_are_packed() {
    hv::RegionMap m;
    check(m.rebuild(scattered()), "pack: the first build is a change");

    check(m.count() == 3, "pack: the file and stack mappings are left out");
    check(m.total_bytes() == 0x21000 + 0x100000 + 0x40000,
          "pack: the total is the memory, not the address range");

    /* The whole point, stated as a number: the union of these regions is over
     * 140 TiB, and packing them is under 1.5 MiB. */
    const std::uint64_t union_span = 0x7ffd00021000ull - 0x5b0000000000ull;
    check(m.total_bytes() * 1000 < union_span,
          "pack: packing is orders of magnitude smaller than spanning");

    check(m.spans()[0].flat == 0, "pack: the first region starts at zero");
    check(m.spans()[1].flat == 0x21000, "pack: the second follows the first");
    check(m.spans()[2].flat == 0x21000 + 0x100000, "pack: and the third, the second");
    check(m.spans()[0].kind == hv::RegionKind::Heap, "pack: kinds are carried");
}

void test_the_round_trip_holds_everywhere() {
    hv::RegionMap m;
    m.rebuild(scattered());

    /* Every packed offset maps to an address that maps back to it. Walked at a
     * page stride rather than byte by byte -- 1.4 MiB of single steps proves
     * nothing more than 350 of them -- plus the exact edges below, which is
     * where the arithmetic actually breaks. */
    bool ok = true;
    for (std::uint64_t f = 0; f < m.total_bytes(); f += 4096) {
        std::uint64_t addr = 0, back = 0;
        if (!m.to_addr(f, addr) || !m.to_flat(addr, back) || back != f) ok = false;
    }
    check(ok, "trip: flat -> addr -> flat is the identity");

    /* And the other direction, from real addresses. */
    ok = true;
    for (const hv::Span &s : m.spans()) {
        for (std::uint64_t a = s.start; a < s.end; a += 4096) {
            std::uint64_t f = 0, back = 0;
            if (!m.to_flat(a, f) || !m.to_addr(f, back) || back != a) ok = false;
        }
    }
    check(ok, "trip: addr -> flat -> addr is the identity");

    /* The edges. A half-open range is where an off-by-one lives, and the last
     * byte of one region is adjacent in packed space to the first byte of the
     * next while being 35 TiB away in reality. */
    for (const hv::Span &s : m.spans()) {
        std::uint64_t f = 0;
        check(m.to_flat(s.start, f) && f == s.flat, "trip: first byte of a region");
        check(m.to_flat(s.end - 1, f) && f == s.flat + s.size() - 1,
              "trip: last byte of a region");
        check(!m.to_flat(s.end, f) || f != s.flat + s.size(),
              "trip: the end address belongs to no region, or to the next one");
    }

    std::uint64_t f = 0;
    check(!m.to_flat(0x6000000000ull, f), "trip: an address in the gap is nowhere");
    check(!m.to_flat(0, f), "trip: and neither is null");

    std::uint64_t a = 0;
    check(!m.to_addr(m.total_bytes(), a), "trip: one past the end is nowhere");
    check(!m.to_addr(UINT64_MAX, a), "trip: nor is anything beyond it");
}

void test_regions_stay_distinguishable() {
    hv::RegionMap m;
    m.rebuild(scattered());

    /* Adjacency in the packed space is not adjacency in memory, and the display
     * has to be able to say so or two arenas read as one heap. */
    const std::uint64_t seam = m.spans()[1].flat;
    check(m.is_boundary(seam), "seam: a region start is a boundary");
    check(!m.is_boundary(seam - 1), "seam: the byte before it is not");
    check(!m.is_boundary(seam + 1), "seam: nor the byte after");
    check(!m.is_boundary(0),
          "seam: offset zero is the top of the map, not a seam");

    std::uint64_t before = 0, after = 0;
    check(m.to_addr(seam - 1, before) && m.to_addr(seam, after),
          "seam: both sides resolve");
    check(after - before > (1ull << 40),
          "seam: and they are terabytes apart in reality");

    const hv::Span *s = m.span_at_flat(seam);
    check(s != nullptr && s->start == 0x7f0000000000ull,
          "seam: the span at a boundary is the one starting there");
}

/* --- the change signal ----------------------------------------------------- */

void test_an_unchanged_scan_is_not_a_change() {
    hv::RegionMap m;
    check(m.rebuild(scattered()), "same: the first build changes everything");
    const std::uint64_t after_first = m.repacks();

    /* The scan runs twice a second for the life of the session and usually
     * finds exactly what it found last time. Reporting that as a change would
     * mean a full rebuild of every cell, twice a second, forever. */
    check(!m.rebuild(scattered()), "same: an identical scan is not a change");
    check(!m.rebuild(scattered()), "same: still not, on the third");
    check(m.repacks() == after_first, "same: and nothing was repacked");
    check(m.count() == 3, "same: the layout survived");
}

void test_a_growing_region_moves_everything_above_it() {
    hv::RegionMap m;
    m.rebuild(scattered());
    const std::uint64_t arena_flat = m.spans()[1].flat;

    /* The brk heap grows, which is the single most common thing a heap does.
     * Every region above it shifts, so every cached cell is now describing
     * memory that is no longer there. */
    auto grown = scattered();
    grown[0].end += 0x10000;
    check(m.rebuild(grown), "grow: a region growing is a change");
    check(m.spans()[1].flat == arena_flat + 0x10000,
          "grow: and it moved the regions above it");
    check(m.total_bytes() == 0x31000 + 0x100000 + 0x40000, "grow: total follows");

    /* Addresses in the grown region still resolve, including the new bytes. */
    std::uint64_t f = 0;
    check(m.to_flat(0x5b0000030000ull, f) && f == 0x30000,
          "grow: the new bytes are mapped");
}

void test_regions_appearing_and_disappearing() {
    hv::RegionMap m;
    m.rebuild(scattered());

    /* A thread starts and glibc gives it an arena. */
    auto more = scattered();
    more.push_back(reg(0x7f8000000000ull, 0x7f8000080000ull, hv::RegionKind::Anon));
    check(m.rebuild(more), "churn: a new arena is a change");
    check(m.count() == 4, "churn: and is packed on the end");
    check(m.spans()[3].flat == 0x21000 + 0x100000 + 0x40000,
          "churn: after everything already there");

    /* The thread exits and the arena is unmapped. The stale span must go: an
     * offset that still resolves into a region the target no longer has is a
     * lookup that succeeds with an answer about nothing. */
    check(m.rebuild(scattered()), "churn: losing it is a change too");
    check(m.count() == 3, "churn: and it is gone");
    check(m.total_bytes() == 0x21000 + 0x100000 + 0x40000, "churn: total shrank");

    std::uint64_t f = 0;
    check(!m.to_flat(0x7f8000000000ull, f),
          "churn: addresses in the unmapped arena resolve nowhere");

    /* A region vanishing from the middle is the case that catches a rebuild
     * which only ever appends: the ones after it have to shift down. */
    auto without_first = scattered();
    without_first.erase(without_first.begin());
    check(m.rebuild(without_first), "churn: losing the first region is a change");
    check(m.spans()[0].start == 0x7f0000000000ull, "churn: the rest moved down");
    check(m.spans()[0].flat == 0, "churn: to the top of the map");
}

/* --- degenerate input ------------------------------------------------------ */

void test_nothing_to_pack() {
    hv::RegionMap m;

    check(!m.rebuild({}), "empty: nothing in, no change");
    check(m.empty() && m.total_bytes() == 0, "empty: and nothing packed");

    std::uint64_t x = 0;
    check(!m.to_flat(0x1000, x), "empty: no address resolves");
    check(!m.to_addr(0, x), "empty: no offset resolves");
    check(m.span_at_addr(0x1000) == nullptr, "empty: and no span is found");
    check(!m.is_boundary(0), "empty: with no boundaries in it");

    /* A map with only file and stack mappings in it: a process that has not
     * allocated yet. Nothing allocatable, so nothing to draw. */
    const std::vector<hv::Region> none = {
        reg(0x400000, 0x401000, hv::RegionKind::File),
        reg(0x7ffd00000000ull, 0x7ffd00021000ull, hv::RegionKind::Stack),
    };
    check(!m.rebuild(none), "empty: nothing allocatable is still nothing");
    check(m.empty(), "empty: so the map stays empty");

    /* Then it allocates. */
    check(m.rebuild(scattered()), "empty: and filling it is a change");
    check(m.count() == 3, "empty: with the regions in it");

    m.clear();
    check(m.empty() && m.total_bytes() == 0, "empty: clear empties it");
}

void test_a_single_region_is_the_identity() {
    /* The unthreaded case, which has to keep working: one region packed is the
     * same coordinates it started with, offset to zero. */
    hv::RegionMap m;
    m.rebuild({reg(0x555500000000ull, 0x555500100000ull, hv::RegionKind::Heap)});

    check(m.count() == 1, "single: one region");
    check(m.total_bytes() == 0x100000, "single: its own size");

    std::uint64_t f = 0;
    check(m.to_flat(0x555500000000ull, f) && f == 0, "single: base maps to zero");
    check(m.to_flat(0x555500080000ull, f) && f == 0x80000,
          "single: and the rest is a plain offset");
    check(!m.is_boundary(0), "single: with no seams anywhere");
}

} // namespace

int main() {
    test_only_allocatable_regions_are_packed();
    test_the_round_trip_holds_everywhere();
    test_regions_stay_distinguishable();
    test_an_unchanged_scan_is_not_a_change();
    test_a_growing_region_moves_everything_above_it();
    test_regions_appearing_and_disappearing();
    test_nothing_to_pack();
    test_a_single_region_is_the_identity();

    if (g_failures != 0) {
        std::fprintf(stderr, "region_map_test: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("region_map_test: packing, the address round trip, region seams, "
                "the change signal and arenas coming and going all hold\n");
    return 0;
}
