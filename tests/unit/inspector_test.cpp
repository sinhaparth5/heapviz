/* heapviz - chunk inspector checks (M5.2).
 *
 * Copyright (C) 2026 Parth Sinha
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The panel is the one place in heapviz that puts a *number* in front of a
 * person and says it describes a specific allocation. Everything else on screen
 * is an aggregate, and an aggregate that is slightly wrong is slightly wrong;
 * `Real Size: 1,040 bytes` for a chunk that is 1,032 is simply a lie about a
 * thing the user could go and check in a debugger. So the fields are checked
 * against records built by hand rather than against each other.
 *
 * Three properties beyond the field values:
 *
 * **The scan stays off the frame budget.** The reverse lookup is O(table), and
 * `scans()` exists so a test can prove it does not run per frame. A cursor
 * parked on a dark cell must never reach it at all, because on a real heap most
 * of the screen is dark and that is where a cursor spends most of its time.
 *
 * **The headline chunk does not depend on probe order.** A cell holding more
 * than `kInspectorCandidates` chunks keeps the largest, not the first sixty-four
 * the table walk happened to reach -- otherwise an unrelated rehash renames the
 * chunk on screen with nothing in the target having changed.
 *
 * **`Tab` survives a rescan.** The selection refreshes four times a second, and
 * a selection that reset to the largest each time would make cycling past the
 * second chunk impossible on a live target.
 */

#include "tui/inspector.h"

#include "tui/capabilities.h"
#include "tui/map_view.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

int g_failures = 0;

void check(bool ok, const char *what) {
    if (!ok) { std::fprintf(stderr, "  FAIL %s\n", what); ++g_failures; }
}

constexpr std::uint64_t kBase = 0x600000000000ull;
constexpr int           kCols = 40;
constexpr int           kRows = 10;
constexpr std::uint64_t kCell = 4096;

/* No RegionMap, so the coordinate is the address and a test can name a cell by
 * arithmetic. `HeapApp` supplies one in production; `region_map_test` is what
 * checks that translation, and duplicating it here would be testing the same
 * conversion twice and this class once. */
struct Fixture {
    hv::Grid       grid;
    hv::HeatMap    map;
    hv::ChunkTable table;

    Fixture() {
        grid.configure(kBase, kBase + kCell * (kCols * kRows), kCols, kRows);
        map.configure(grid);
    }

    /* Puts a chunk in both halves of the model, exactly as `HeapApp::apply`
     * does -- the inspector reads the table but gates on the map, so a fixture
     * that touched only one would test a state that cannot occur. */
    void alloc(std::uint64_t addr, std::uint32_t size, std::uint32_t usable,
               std::uint32_t ms) {
        map.on_alloc(addr, size, usable, ms);
        table.insert_live(addr, size, usable, ms, 1234);
    }

    void free(std::uint64_t addr, std::uint32_t size, std::uint32_t usable,
              std::uint32_t ms) {
        map.on_free(addr, size, usable, ms);
        table.mark_freed(addr, ms);
    }

    std::uint64_t addr_of_cell(std::size_t cell, std::uint64_t within = 0) {
        return kBase + kCell * cell + within;
    }
};

hv::MapCursor at_cell(const hv::Grid &g, std::size_t cell) {
    hv::MapCursor c;
    c.set_coord(g, kBase + kCell * cell);
    return c;
}

/* --- formatting ------------------------------------------------------------- */

void test_counts_carry_thousands_separators() {
    char buf[32];
    const struct { std::uint64_t v; const char *want; } cases[] = {
        {0, "0"},         {7, "7"},           {100, "100"},
        {999, "999"},     {1000, "1,000"},    {1040, "1,040"},
        {12345, "12,345"}, {1048576, "1,048,576"},
        {18446744073709551615ull, "18,446,744,073,709,551,615"},
    };
    for (const auto &c : cases) {
        hv::format_count(buf, sizeof buf, c.v);
        check(std::strcmp(buf, c.want) == 0, "count: separators land every three digits");
    }

    /* A buffer too small truncates rather than overruns. The panel builds this
     * into a fixed line[] and a size field is not worth a stack smash. */
    char tiny[4];
    hv::format_count(tiny, sizeof tiny, 1048576);
    check(std::strlen(tiny) < sizeof tiny, "count: a short buffer is not overrun");
}

/* --- the empty state -------------------------------------------------------- */

void test_an_empty_cell_reports_unallocated() {
    Fixture f;
    hv::ChunkInspector in;
    hv::MapCursor c = at_cell(f.grid, 12);

    in.refresh(f.table, f.map, c, 0, true);
    check(in.total() == 0, "empty: nothing found");
    check(in.current().status == hv::ChunkStatus::Unallocated,
          "empty: the status is UNALLOCATED, which is an answer");
    check(std::strcmp(hv::chunk_status_str(hv::ChunkStatus::Unallocated),
                      "UNALLOCATED") == 0, "empty: and it has a name");
}

void test_a_dark_cell_never_costs_a_scan() {
    Fixture f;
    f.alloc(f.addr_of_cell(5), 128, 136, 10);

    hv::ChunkInspector in;
    hv::MapCursor c = at_cell(f.grid, 300); /* nothing has ever been here */

    in.refresh(f.table, f.map, c, 0, true);
    const std::uint64_t after_first = in.scans();
    check(after_first == 0,
          "budget: a cell the map says is empty is answered without a scan");

    /* And it stays free across a second of refreshes. This is the property the
     * whole design rests on: the lookup is O(table), and on a real heap the
     * cursor is over a dark cell almost all of the time. */
    for (std::uint32_t ms = 0; ms < 1000; ms += 16)
        in.refresh(f.table, f.map, c, ms, false);
    check(in.scans() == 0, "budget: and still none a second later");
}

void test_the_scan_does_not_run_every_frame() {
    Fixture f;
    f.alloc(f.addr_of_cell(5), 128, 136, 10);

    hv::ChunkInspector in;
    hv::MapCursor c = at_cell(f.grid, 5);

    in.refresh(f.table, f.map, c, 0, true);
    check(in.scans() == 1, "budget: an occupied cell is scanned once");

    /* One second of 60 fps frames over a cell that holds something. At 4 Hz the
     * refresh should fire four times, not sixty -- and certainly not once per
     * frame, which at 65,536 slots would be most of the frame budget. */
    for (std::uint32_t ms = 1; ms <= 1000; ms += 16)
        in.refresh(f.table, f.map, c, ms, false);
    check(in.scans() <= 6, "budget: about four scans a second, not sixty");
    check(in.scans() >= 3, "budget: but it does keep up with the target");
}

/* --- the fields ------------------------------------------------------------- */

void test_a_live_chunk_reports_every_field() {
    Fixture f;
    const std::uint64_t addr = f.addr_of_cell(7, 64);
    f.alloc(addr, 1000, 1016, 500);

    hv::ChunkInspector in;
    hv::MapCursor c = at_cell(f.grid, 7);
    in.refresh(f.table, f.map, c, 500, true);

    const hv::ChunkDetail &d = in.current();
    check(in.total() == 1, "fields: exactly the one chunk");
    check(d.addr == addr, "fields: the address is the pointer malloc returned");
    check(d.size == 1000, "fields: user size is what the caller asked for");
    check(d.usable == 1016, "fields: usable is what the allocator gave");
    check(d.status == hv::ChunkStatus::Active, "fields: ACTIVE while it is live");

    /* Unrefined: overhead can only be `usable - size`, which cannot see the
     * chunk header and is therefore an underestimate. The flag is what lets the
     * panel say so instead of presenting it as measured. */
    check(!d.exact, "fields: an unenriched chunk is marked inferred");
    check(d.overhead == 16, "fields: inferred overhead is usable - size");
    check(d.chunk_bytes() == 1016, "fields: real size is size + overhead");
}

void test_an_enriched_chunk_reports_the_measured_overhead() {
    Fixture f;
    const std::uint64_t addr = f.addr_of_cell(7, 64);
    f.alloc(addr, 1000, 1016, 500);

    /* What M2.2's enrichment pass does: the header said the chunk really cost
     * 40 bytes beyond the request, not the 16 the interceptor's figures imply. */
    check(f.table.mark_refined(addr, 40), "fields: the record took the figure");

    hv::ChunkInspector in;
    hv::MapCursor c = at_cell(f.grid, 7);
    in.refresh(f.table, f.map, c, 500, true);

    const hv::ChunkDetail &d = in.current();
    check(d.exact, "fields: a refined chunk is marked measured");
    check(d.overhead == 40, "fields: and reports the header's figure");
    check(d.chunk_bytes() == 1040, "fields: 1,040 bytes, as the mockup has it");
}

void test_an_mmapped_chunk_is_named_as_one() {
    Fixture f;
    const std::uint64_t addr = f.addr_of_cell(9);
    f.alloc(addr, 200000, 200016, 100);

    hv::ChunkInspector in;
    hv::MapCursor c = at_cell(f.grid, 9);

    /* Before the header has been read, the honest answer is the common one.
     * Guessing MMAPPED from the size would be encoding glibc's mmap threshold,
     * which is tunable at runtime by the target itself. */
    in.refresh(f.table, f.map, c, 100, true);
    check(in.current().status == hv::ChunkStatus::Active,
          "mmap: an unread chunk is ACTIVE, not guessed at");

    f.table.mark_refined(addr, 24, hv::kChunkFlagMmapped);
    in.refresh(f.table, f.map, c, 400, true);
    check(in.current().status == hv::ChunkStatus::Mmapped,
          "mmap: the header's IS_MMAPPED bit is what names it");

    /* A rebuild clears the refinement, and the header bits have to go with it --
     * otherwise a stale MMAPPED outlives the figure it arrived with. */
    f.table.clear_refined();
    in.refresh(f.table, f.map, c, 700, true);
    check(in.current().status == hv::ChunkStatus::Active,
          "mmap: clear_refined takes the header bits back too");
    check(!in.current().exact, "mmap: and the overhead with them");
}

void test_a_freed_chunk_reports_how_long_it_lived() {
    Fixture f;
    const std::uint64_t addr = f.addr_of_cell(3);
    f.alloc(addr, 512, 520, 1000);
    f.free(addr, 512, 520, 3500);

    hv::ChunkInspector in;
    hv::MapCursor c = at_cell(f.grid, 3);
    in.refresh(f.table, f.map, c, 3500, true);

    const hv::ChunkDetail &d = in.current();
    check(d.status == hv::ChunkStatus::Freed, "freed: named FREED");
    check(d.alloc_ms == 1000 && d.free_ms == 3500,
          "freed: both timestamps survive into the panel");
    check(d.free_ms - d.alloc_ms == 2500,
          "freed: lifetime is alloc to free, not alloc to now");
}

/* --- several chunks in one cell --------------------------------------------- */

void test_the_largest_leads_and_tab_cycles() {
    Fixture f;
    /* Four chunks inside one 4 KiB cell, inserted smallest-first so that
     * "largest" cannot come out right by accident of insertion order. */
    f.alloc(f.addr_of_cell(20, 0),    64,   72, 10);
    f.alloc(f.addr_of_cell(20, 256),  512,  520, 10);
    f.alloc(f.addr_of_cell(20, 1024), 2048, 2056, 10);
    f.alloc(f.addr_of_cell(20, 3200), 128,  136, 10);

    hv::ChunkInspector in;
    hv::MapCursor c = at_cell(f.grid, 20);
    in.refresh(f.table, f.map, c, 10, true);

    check(in.total() == 4, "crowd: all four are counted");
    check(in.reachable() == 4, "crowd: and all four are reachable");
    check(in.current().size == 2048, "crowd: the largest leads");
    check(in.position() == 0, "crowd: at position 0");

    check(in.cycle(), "crowd: Tab moves");
    check(in.current().size == 512, "crowd: to the next largest");
    check(in.cycle() && in.current().size == 128, "crowd: and the next");
    check(in.cycle() && in.current().size == 64, "crowd: and the next");
    check(in.cycle(), "crowd: Tab wraps");
    check(in.current().size == 2048, "crowd: back to the largest");
}

void test_tab_does_nothing_with_one_chunk() {
    Fixture f;
    f.alloc(f.addr_of_cell(2), 64, 72, 10);

    hv::ChunkInspector in;
    hv::MapCursor c = at_cell(f.grid, 2);
    in.refresh(f.table, f.map, c, 10, true);

    check(in.total() == 1, "solo: one chunk");
    check(!in.cycle(), "solo: Tab reports no change, so no frame is drawn");
}

void test_the_selection_survives_a_rescan() {
    Fixture f;
    f.alloc(f.addr_of_cell(20, 0),    2048, 2056, 10);
    f.alloc(f.addr_of_cell(20, 2100), 512,  520, 10);

    hv::ChunkInspector in;
    hv::MapCursor c = at_cell(f.grid, 20);
    in.refresh(f.table, f.map, c, 10, true);

    in.cycle();
    const std::uint64_t picked = in.current().addr;
    check(in.current().size == 512, "hold: cycled to the smaller one");

    /* Four refreshes later -- a second of wall clock on a live target. Without
     * the addressed selection this snaps back to the largest and cycling past
     * the first chunk becomes impossible. */
    for (std::uint32_t ms = 300; ms <= 1200; ms += 300)
        in.refresh(f.table, f.map, c, ms, true);

    check(in.scans() >= 4, "hold: the rescans really happened");
    check(in.current().addr == picked, "hold: and the selection is still Tab's");
}

void test_more_chunks_than_can_be_reached_still_lead_with_the_largest() {
    Fixture f;
    /* A 4 KiB cell packed with 128 small chunks, plus one large one placed last
     * so a first-K list would miss it -- and, crucially, sized so that it is the
     * largest by a margin no ordering accident could produce. */
    const std::size_t cell = 30;
    for (std::size_t i = 0; i < 128; ++i)
        f.alloc(f.addr_of_cell(cell, i * 32), 16, 24, 10);
    const std::uint64_t big = f.addr_of_cell(cell, 4090);
    f.alloc(big, 1024, 1032, 10);

    hv::ChunkInspector in;
    hv::MapCursor c = at_cell(f.grid, cell);
    in.refresh(f.table, f.map, c, 10, true);

    check(in.total() == 129, "topk: the true total is reported");
    check(in.reachable() == hv::kInspectorCandidates,
          "topk: the reachable list is capped");
    check(in.reachable() < in.total(),
          "topk: and the panel can therefore say so");
    check(in.current().addr == big,
          "topk: the largest survives the cap, whatever the probe order was");
}

/* --- drawing ---------------------------------------------------------------- */

std::string row_text(const hv::Framebuffer &fb, int y) {
    std::string s;
    for (int x = 0; x < fb.width(); ++x) {
        const char32_t g = fb.at_back(x, y).glyph;
        s.push_back(g < 128 ? static_cast<char>(g) : '?');
    }
    return s;
}

std::string panel_text(const hv::Framebuffer &fb, hv::Rect area) {
    std::string s;
    for (int y = area.y; y < area.y + area.h; ++y) s += row_text(fb, y) + "\n";
    return s;
}

void test_the_panel_draws_the_fields() {
    Fixture f;
    const std::uint64_t addr = f.addr_of_cell(7, 64);
    f.alloc(addr, 1000, 1016, 500);
    f.table.mark_refined(addr, 40);

    hv::ChunkInspector in;
    hv::MapCursor c = at_cell(f.grid, 7);
    in.refresh(f.table, f.map, c, 2500, true);

    hv::Framebuffer fb;
    fb.resize(100, hv::kInspectorRows);
    fb.clear();

    const hv::Rect area{0, 0, 100, hv::kInspectorRows};
    in.draw(fb, area, f.grid, c, nullptr, 2500);
    const std::string out = panel_text(fb, area);

    check(out.find("CHUNK INSPECTOR") != std::string::npos,
          "panel: it names itself");
    check(out.find("0x0000600000007040") != std::string::npos,
          "panel: the address is the full 64-bit pointer, zero-padded");
    check(out.find("1,000 bytes") != std::string::npos,
          "panel: user size, with separators");
    check(out.find("1,040 bytes") != std::string::npos,
          "panel: real size is size plus the measured overhead");
    check(out.find("measured") != std::string::npos,
          "panel: and is marked as measured rather than inferred");
    check(out.find("ACTIVE (ptmalloc)") != std::string::npos,
          "panel: the status, in the mockup's words");
    check(out.find("2.0 s") != std::string::npos,
          "panel: alive for two seconds of the session");
}

void test_the_panel_marks_an_inferred_overhead() {
    Fixture f;
    f.alloc(f.addr_of_cell(7), 1000, 1016, 0);

    hv::ChunkInspector in;
    hv::MapCursor c = at_cell(f.grid, 7);
    in.refresh(f.table, f.map, c, 0, true);

    hv::Framebuffer fb;
    fb.resize(100, hv::kInspectorRows);
    fb.clear();
    const hv::Rect area{0, 0, 100, hv::kInspectorRows};
    in.draw(fb, area, f.grid, c, nullptr, 0);
    const std::string out = panel_text(fb, area);

    check(out.find("inferred") != std::string::npos,
          "panel: an unenriched overhead is not presented as measured");
    check(out.find("~16 B") != std::string::npos,
          "panel: and the figure itself carries the approximation mark");
}

void test_the_empty_panel_reads_as_intentional() {
    Fixture f;
    hv::ChunkInspector in;
    hv::MapCursor c = at_cell(f.grid, 100);
    in.refresh(f.table, f.map, c, 0, true);

    hv::Framebuffer fb;
    fb.resize(100, hv::kInspectorRows);
    fb.clear();
    const hv::Rect area{0, 0, 100, hv::kInspectorRows};
    in.draw(fb, area, f.grid, c, nullptr, 0);
    const std::string out = panel_text(fb, area);

    check(out.find("UNALLOCATED") != std::string::npos,
          "empty panel: the status says what it is");
    check(out.find("address space the target has not used") != std::string::npos,
          "empty panel: in words, not just a blank");
    check(out.find("n / N") != std::string::npos,
          "empty panel: and says which key gets somewhere useful");

    /* The address is still real: it is a cell of the target's address space
     * that happens to hold nothing, which is different from having no answer. */
    check(out.find("0x0000600000064000") != std::string::npos,
          "empty panel: with the address of the cell the cursor is on");
}

void test_the_panel_says_when_a_cell_is_crowded() {
    Fixture f;
    f.alloc(f.addr_of_cell(20, 0),    2048, 2056, 10);
    f.alloc(f.addr_of_cell(20, 2100), 512,  520, 10);
    f.alloc(f.addr_of_cell(20, 3000), 256,  264, 10);

    hv::ChunkInspector in;
    hv::MapCursor c = at_cell(f.grid, 20);
    in.refresh(f.table, f.map, c, 10, true);

    hv::Framebuffer fb;
    fb.resize(100, hv::kInspectorRows);
    fb.clear();
    const hv::Rect area{0, 0, 100, hv::kInspectorRows};
    in.draw(fb, area, f.grid, c, nullptr, 10);
    const std::string out = panel_text(fb, area);

    check(out.find("1 of 3 in this cell") != std::string::npos,
          "crowded panel: a cell is a span of addresses, and it says how many");
    check(out.find("Tab") != std::string::npos,
          "crowded panel: and which key reaches the others");
}

void test_the_panel_says_when_there_are_no_bounds_yet() {
    const hv::Grid g;   /* never configured: before the first /proc scan */
    hv::ChunkInspector in;
    const hv::MapCursor c;

    hv::Framebuffer fb;
    fb.resize(100, hv::kInspectorRows);
    fb.clear();
    const hv::Rect area{0, 0, 100, hv::kInspectorRows};
    in.draw(fb, area, g, c, nullptr, 0);
    const std::string out = panel_text(fb, area);

    check(out.find("waiting for the target's memory map") != std::string::npos,
          "no bounds: said out loud rather than shown as address zero");
    check(out.find("0x0000000000000000") == std::string::npos,
          "no bounds: which would be a real-looking answer to nothing");
}

} // namespace

int main() {
    test_counts_carry_thousands_separators();
    test_an_empty_cell_reports_unallocated();
    test_a_dark_cell_never_costs_a_scan();
    test_the_scan_does_not_run_every_frame();
    test_a_live_chunk_reports_every_field();
    test_an_enriched_chunk_reports_the_measured_overhead();
    test_an_mmapped_chunk_is_named_as_one();
    test_a_freed_chunk_reports_how_long_it_lived();
    test_the_largest_leads_and_tab_cycles();
    test_tab_does_nothing_with_one_chunk();
    test_the_selection_survives_a_rescan();
    test_more_chunks_than_can_be_reached_still_lead_with_the_largest();
    test_the_panel_draws_the_fields();
    test_the_panel_marks_an_inferred_overhead();
    test_the_empty_panel_reads_as_intentional();
    test_the_panel_says_when_a_cell_is_crowded();
    test_the_panel_says_when_there_are_no_bounds_yet();

    if (g_failures != 0) {
        std::fprintf(stderr, "inspector: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("inspector: all checks passed\n");
    return 0;
}
