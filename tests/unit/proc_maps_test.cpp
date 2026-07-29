/* heapviz - virtual memory segment scanner checks (M2.1).
 *
 * Copyright (C) 2026 Parth Sinha
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Two halves, and the split is the point. Most of this drives `scan_text` with
 * literal /proc output, because the interesting inputs -- a thread arena, a
 * torn line, a path with a space in it -- are ones this process cannot be
 * persuaded to produce on demand. The rest reads /proc/self/maps, because a
 * parser tested only against text someone wrote to satisfy it will agree with
 * that text and nothing else.
 *
 * Reading our own map means the expectations have to hold for any process on
 * any kernel: that the regions come back ascending and disjoint, that the code
 * we are executing is inside one of them, that a stack exists. Anything more
 * specific than that would be asserting on the machine rather than on the code.
 */

#include "tui/proc_maps.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include <unistd.h>

namespace {

int g_failures = 0;

void check(bool ok, const char *what) {
    if (!ok) { std::fprintf(stderr, "  FAIL %s\n", what); ++g_failures; }
}

/* A map with one of everything, laid out the way the kernel lays it out: the
 * binary and its libraries file-backed, a brk heap, a 64 MiB-aligned thread
 * arena with its uncommitted tail, and the bracketed regions at the top. */
const char *const kSampleMap =
    "55a4c8e00000-55a4c8e04000 r--p 00000000 08:02 1966081  /usr/bin/churn\n"
    "55a4c8e04000-55a4c8e0c000 r-xp 00004000 08:02 1966081  /usr/bin/churn\n"
    "55a4c8e21000-55a4c8e42000 rw-p 00000000 00:00 0        [heap]\n"
    "7f2c00000000-7f2c00021000 rw-p 00000000 00:00 0 \n"
    "7f2c00021000-7f2c04000000 ---p 00000000 00:00 0 \n"
    "7f2c08000000-7f2c08021000 rw-p 00000000 00:00 0 \n"
    "7f2c0c123000-7f2c0c125000 rw-p 00000000 00:00 0 \n"
    "7f2c0c130000-7f2c0c158000 r--p 00000000 08:02 2097153  /usr/lib/libc.so.6\n"
    "7ffd4a3e1000-7ffd4a402000 rw-p 00000000 00:00 0        [stack]\n"
    "7ffd4a5d0000-7ffd4a5d4000 r--p 00000000 00:00 0        [vvar]\n"
    "7ffd4a5d4000-7ffd4a5d6000 r-xp 00000000 00:00 0        [vdso]\n";

/* --- the line parser ------------------------------------------------------ */

void test_a_line_parses_into_its_fields() {
    hv::Region r;
    check(hv::parse_maps_line(
              "55a4c8e21000-55a4c8e42000 rw-p 00000000 00:00 0    [heap]", r),
          "line: a heap line parses");
    check(r.start == 0x55a4c8e21000ull, "line: start");
    check(r.end == 0x55a4c8e42000ull, "line: end");
    check(r.size() == 0x21000ull, "line: size is end - start");
    check(r.kind == hv::RegionKind::Heap, "line: [heap] is the heap");
    check(r.perms == (hv::kPermRead | hv::kPermWrite | hv::kPermPrivate),
          "line: rw-p sets exactly read, write and private");
    check(r.inode == 0, "line: anonymous inode");
    check(r.allocatable(), "line: the heap is allocatable");

    check(hv::parse_maps_line(
              "7f2c0c130000-7f2c0c158000 r-xp 00012000 08:02 2097153"
              "  /usr/lib/libc.so.6", r),
          "line: a file mapping parses");
    check(r.kind == hv::RegionKind::File, "line: a file mapping is file-backed");
    check(r.file_offset == 0x12000ull, "line: file offset");
    check(r.inode == 2097153, "line: inode");
    check(r.perms == (hv::kPermRead | hv::kPermExec | hv::kPermPrivate),
          "line: r-xp is not writable");
    check(!r.allocatable(), "line: a file mapping is never allocatable");

    /* The kernel pads the path column with spaces and a mapped file is allowed
     * to contain one, so the path is the remainder of the line rather than the
     * next whitespace-delimited token. A parser that stops at the first space
     * turns "/tmp/my data.bin" into "/tmp/my", which still classifies as a file
     * and so goes unnoticed until someone reads it in the inspector. */
    check(hv::parse_maps_line(
              "10000000-10001000 r--p 00000000 08:02 12  /tmp/my data.bin", r),
          "line: a path containing a space parses");
    check(r.kind == hv::RegionKind::File, "line: that path is file-backed");

    /* A deleted mapping keeps its suffix, for the same reason. */
    check(hv::parse_maps_line(
              "10000000-10001000 r-xp 00000000 08:02 12  /tmp/a.so (deleted)", r),
          "line: a deleted mapping parses");
    check(r.kind == hv::RegionKind::File, "line: deleted, still file-backed");

    /* Shared rather than private. Not a heap: another process can write it. */
    check(hv::parse_maps_line("20000000-20001000 rw-s 00000000 00:00 0 ", r),
          "line: a shared anonymous mapping parses");
    check((r.perms & hv::kPermPrivate) == 0, "line: 's' is not 'p'");
    check(r.kind == hv::RegionKind::Other,
          "line: shared anonymous memory is not counted as heap");
}

void test_classification_is_by_name_then_permissions() {
    hv::Region r;

    check(hv::parse_maps_line("7f2c00000000-7f2c00021000 rw-p 0 00:00 0", r),
          "kind: a bare anonymous line parses");
    check(r.kind == hv::RegionKind::Anon, "kind: anonymous rw-p is heap memory");

    /* An arena reserves 64 MiB PROT_NONE and commits a prefix. The reservation
     * is not allocatable, and counting it would put tens of megabytes of
     * unusable address space on the map. */
    check(hv::parse_maps_line("7f2c00021000-7f2c04000000 ---p 0 00:00 0", r),
          "kind: a PROT_NONE reservation parses");
    check(r.kind == hv::RegionKind::Other, "kind: PROT_NONE is not allocatable");

    check(hv::parse_maps_line("7ffd4a3e1000-7ffd4a402000 rw-p 0 00:00 0 [stack]",
                              r),
          "kind: the stack parses");
    check(r.kind == hv::RegionKind::Stack, "kind: [stack] is the stack");
    check(!r.allocatable(), "kind: the stack is not heap memory");

    check(hv::parse_maps_line("7ffd4a5d4000-7ffd4a5d6000 r-xp 0 00:00 0 [vdso]",
                              r),
          "kind: the vdso parses");
    check(r.kind == hv::RegionKind::Other, "kind: [vdso] is not heap memory");

    /* [heap] is anonymous rw-p too. Classifying on permissions first would call
     * it Anon and lose the one region the kernel names for us. */
    check(hv::parse_maps_line("55a4c8e21000-55a4c8e42000 rw-p 0 00:00 0 [heap]",
                              r),
          "kind: the heap parses");
    check(r.kind == hv::RegionKind::Heap,
          "kind: the name wins over the permissions");
}

void test_thread_arenas_are_recognised_by_alignment() {
    hv::Region r;

    check(hv::parse_maps_line("7f2c00000000-7f2c00021000 rw-p 0 00:00 0", r),
          "arena: an aligned anonymous region parses");
    check(r.thread_arena, "arena: 64 MiB-aligned anonymous memory is a candidate");

    check(hv::parse_maps_line("7f2c0c123000-7f2c0c125000 rw-p 0 00:00 0", r),
          "arena: an unaligned anonymous region parses");
    check(!r.thread_arena, "arena: an unaligned mmap is not an arena");

    /* The brk heap is not an arena candidate even when it happens to land on
     * the alignment, because it is already the main one. */
    check(hv::parse_maps_line("7f2c00000000-7f2c00021000 rw-p 0 00:00 0 [heap]",
                              r),
          "arena: an aligned heap parses");
    check(!r.thread_arena, "arena: the brk heap is not a thread arena");
}

void test_malformed_lines_are_rejected() {
    hv::Region r;
    const char *const bad[] = {
        "",
        "\n",
        "not a map line at all",
        "55a4c8e21000 rw-p 00000000 00:00 0",          /* no end address     */
        "55a4c8e21000-",                                /* truncated mid-read */
        "55a4c8e21000-55a4c8e42000",                    /* no permissions     */
        "55a4c8e21000-55a4c8e42000 rw",                 /* short permissions  */
        "55a4c8e21000-55a4c8e42000 rwqp 0 00:00 0",     /* not a perms field  */
        "55a4c8e42000-55a4c8e21000 rw-p 0 00:00 0",     /* inverted range     */
        "55a4c8e21000-55a4c8e42000 rw-p 0 00:00",       /* no inode           */
        "55a4c8e21000-55a4c8e42000 rw-p 0 0000 0",      /* no dev separator   */
        "0123456789abcdef0-1 rw-p 0 00:00 0",           /* 17 hex digits      */
    };
    for (const char *line : bad) {
        char what[128];
        std::snprintf(what, sizeof what, "malformed: rejected \"%s\"", line);
        check(!hv::parse_maps_line(line, r), what);
    }

    /* A zero-length region is something the kernel does not emit but a torn
     * read can appear to: it is well-formed, so it parses, and the bounds pass
     * ignores it rather than the parser refusing the line. */
    check(hv::parse_maps_line("55a4c8e21000-55a4c8e21000 rw-p 0 00:00 0", r),
          "malformed: an empty range is well-formed");
    check(r.size() == 0, "malformed: and is empty");
}

/* --- the scanner over fixed text ------------------------------------------ */

void test_a_scan_summarises_the_map() {
    hv::MapsScanner s(1234);
    check(!s.scanned(), "scan: nothing is known before the first scan");
    check(s.bounds().empty(), "scan: and the bounds are empty, not zero-based");

    check(s.scan_text(kSampleMap, 1000) == hv::ScanStatus::Ok, "scan: parsed");
    check(s.regions().size() == 11, "scan: every line became a region");
    check(s.malformed_lines() == 0, "scan: none of them were malformed");
    check(s.scans() == 1, "scan: one scan counted");

    /* Bounds run from the brk heap to the last allocatable anonymous region.
     * The libc mapping past it is file-backed and must not extend them. */
    check(s.bounds().base == 0x55a4c8e21000ull, "scan: bounds start at the heap");
    check(s.bounds().end == 0x7f2c0c125000ull,
          "scan: bounds end at the last anonymous region, not the last mapping");

    check(s.main_arena(), "scan: the main arena was found");
    check(s.thread_arenas() == 2,
          "scan: two aligned anonymous regions, and not the unaligned one");

    char label[hv::kArenaLabelMax];
    const std::size_t n = s.arena_label(label, sizeof label);
    check(std::strcmp(label, "Main + 2 threads") == 0, "scan: the arena label");
    check(n == std::strlen(label), "scan: which returns its own length");
}

void test_lookup_finds_the_region_holding_an_address() {
    hv::MapsScanner s(1234);
    s.scan_text(kSampleMap, 0);

    const hv::Region *r = s.find(0x55a4c8e30000ull);
    check(r != nullptr && r->kind == hv::RegionKind::Heap,
          "find: an address inside the heap");
    r = s.find(0x7ffd4a3e5000ull);
    check(r != nullptr && r->kind == hv::RegionKind::Stack,
          "find: an address inside the stack");

    /* The boundaries, because a half-open range is where an off-by-one hides. */
    check(s.find(0x55a4c8e21000ull) != nullptr, "find: the first byte is inside");
    check(s.find(0x55a4c8e42000ull) == nullptr,
          "find: the end address is not inside");

    check(s.find(0) == nullptr, "find: a null pointer is in no region");
    check(s.find(0x60000000000ull) == nullptr, "find: a hole is in no region");
    check(s.find(UINT64_MAX) == nullptr, "find: past the top is in no region");

    /* Every region reports itself, which is the property a binary search over
     * a list assumed sorted actually needs. */
    bool all = true;
    for (const hv::Region &reg : s.regions())
        if (s.find(reg.start) != &reg) all = false;
    check(all, "find: every region is found at its own start");
}

void test_a_torn_read_costs_one_region_not_the_scan() {
    hv::MapsScanner s(1234);

    /* The map changed under the reader: one line arrived cut in half. The
     * regions either side of it must still be there, because blanking the
     * display for 500 ms is a worse answer than being short one region. */
    const char *const torn =
        "55a4c8e21000-55a4c8e42000 rw-p 00000000 00:00 0 [heap]\n"
        "7f2c00000000-7f2c000\n"
        "7f2c08000000-7f2c08021000 rw-p 00000000 00:00 0 \n";

    check(s.scan_text(torn, 0) == hv::ScanStatus::Ok, "torn: the scan succeeds");
    check(s.regions().size() == 2, "torn: the good lines survive");
    check(s.malformed_lines() == 1, "torn: and the bad one is counted");
    check(s.main_arena(), "torn: the heap either side of it is still known");

    /* A file with no trailing newline is the other shape of the same problem. */
    hv::MapsScanner t(1234);
    t.scan_text("55a4c8e21000-55a4c8e42000 rw-p 00000000 00:00 0 [heap]", 0);
    check(t.regions().size() == 1, "torn: a last line without a newline parses");
}

void test_rescanning_state_is_reset_not_accumulated() {
    hv::MapsScanner s(1234);
    s.scan_text(kSampleMap, 0);
    check(s.regions().size() == 11 && s.thread_arenas() == 2, "reset: first scan");

    /* A target that freed its arenas back. If the counts accumulated instead of
     * being rebuilt, the header bar would keep claiming arenas that are gone --
     * and it would be believable, which is what makes it worth a test. */
    s.scan_text("55a4c8e21000-55a4c8e42000 rw-p 00000000 00:00 0 [heap]\n", 1);
    check(s.regions().size() == 1, "reset: the region list is rebuilt");
    check(s.thread_arenas() == 0, "reset: the arena count is rebuilt");
    check(s.main_arena(), "reset: the heap is still there");
    check(s.bounds().base == 0x55a4c8e21000ull &&
              s.bounds().end == 0x55a4c8e42000ull,
          "reset: the bounds shrink with it");
    check(s.scans() == 2, "reset: two scans counted");

    /* And a map with nothing allocatable in it leaves empty bounds rather than
     * a stale range from the scan before. */
    s.scan_text("7ffd4a5d4000-7ffd4a5d6000 r-xp 0 00:00 0 [vdso]\n", 2);
    check(s.bounds().empty(), "reset: no allocatable regions, no bounds");
    check(!s.main_arena(), "reset: no heap either");
}

/* --- the rescan policy ---------------------------------------------------- */

void test_the_rescan_timer() {
    hv::MapsScanner s(1234);
    check(s.due(0), "due: the first scan is always owed");

    s.scan_text(kSampleMap, 1000);
    check(!s.due(1000), "due: not immediately after a scan");
    check(!s.due(1000 + hv::kRescanIntervalMs - 1), "due: not just before");
    check(s.due(1000 + hv::kRescanIntervalMs), "due: at the interval");
    check(s.due(1000 + hv::kRescanIntervalMs * 4), "due: and after it");

    /* The millisecond clock is 32 bits and wraps every 49 days. Unsigned
     * subtraction gives the right delta across the wrap; a signed comparison
     * would leave the scan permanently owed or permanently not. */
    hv::MapsScanner w(1234);
    w.scan_text(kSampleMap, UINT32_MAX - 100);
    check(!w.due(UINT32_MAX - 100), "wrap: not due at the scan");
    check(!w.due(UINT32_MAX), "wrap: not due before the interval, across zero");
    check(w.due(static_cast<std::uint32_t>(UINT32_MAX + 401u)),
          "wrap: due at the interval, across zero");
}

void test_an_address_outside_the_bounds_forces_a_rescan() {
    hv::MapsScanner s(1234);
    s.scan_text(kSampleMap, 1000);

    s.note_address(0x55a4c8e30000ull);
    check(!s.due(1000), "note: an address inside the bounds changes nothing");

    /* This is the trigger that keeps the display correct: a heap that grew
     * mid-frame is noticed by the event that landed outside it, not by a timer
     * that would have caught it up to 500 ms later. */
    s.note_address(0x9000000000ull);
    check(s.due(1000), "note: an address outside the bounds is owed now");

    s.scan_text(kSampleMap, 1100);
    check(!s.due(1100), "note: and the scan clears it");

    /* Before the first scan there are no bounds, so everything is outside. */
    hv::MapsScanner fresh(1234);
    fresh.note_address(0x55a4c8e30000ull);
    check(fresh.due(0), "note: with no bounds yet, any address forces a scan");
}

/* --- against a real /proc ------------------------------------------------- */

void test_scanning_our_own_map() {
    hv::MapsScanner s(static_cast<int>(::getpid()));
    const hv::ScanStatus st = s.scan(0);
    check(st == hv::ScanStatus::Ok, "self: /proc/self/maps scanned");
    if (st != hv::ScanStatus::Ok) return;

    check(!s.regions().empty(), "self: it has regions");
    check(s.malformed_lines() == 0,
          "self: every line the running kernel emitted parsed");

    /* Ascending and disjoint. The binary search in `find` depends on it, and
     * nothing sorts the list, so this is the assumption rather than a property
     * of the implementation. */
    bool ordered = true;
    for (std::size_t i = 1; i < s.regions().size(); ++i)
        if (s.regions()[i].start < s.regions()[i - 1].end) ordered = false;
    check(ordered, "self: regions are ascending and disjoint");

    /* We are running inside this map, so the code doing the asserting has to be
     * in one of the file-backed regions, and the stack has to exist. */
    const auto here = reinterpret_cast<std::uint64_t>(&test_scanning_our_own_map);
    const hv::Region *code = s.find(here);
    check(code != nullptr, "self: the running code is inside a region");
    check(code != nullptr && (code->perms & hv::kPermExec) != 0,
          "self: and that region is executable");

    int stacks = 0;
    for (const hv::Region &r : s.regions())
        if (r.kind == hv::RegionKind::Stack) ++stacks;
    check(stacks == 1, "self: exactly one [stack]");

    /* This test binary has certainly called malloc by now -- the region list
     * itself is heap -- so there is somewhere to allocate from, whether that
     * came from brk or from mmap. The arena label is printed rather than
     * asserted: under ASan the allocator is not glibc's, there may be no
     * [heap] at all, and pinning a number here would be asserting on which
     * malloc the test was linked against. */
    check(!s.bounds().empty(), "self: the bounds cover something");

    char label[hv::kArenaLabelMax];
    check(s.arena_label(label, sizeof label) > 0, "self: an arena label");
    std::printf("  self: %zu regions, bounds 0x%llx-0x%llx, arenas %s\n",
                s.regions().size(),
                static_cast<unsigned long long>(s.bounds().base),
                static_cast<unsigned long long>(s.bounds().end), label);
}

void test_a_dead_target_is_reported_as_gone() {
    /* No process has this pid: it is above every pid_max Linux permits, so the
     * open cannot race a live process into existence. */
    hv::MapsScanner s(0x7FFFFFFF);
    check(s.scan(0) == hv::ScanStatus::TargetGone, "gone: ENOENT reads as gone");
    check(s.target_gone(), "gone: and the scanner says so");
    check(std::strcmp(hv::scan_status_str(hv::ScanStatus::TargetGone),
                      "target exited") == 0,
          "gone: with a status the UI can print");

    /* A dead target must stop being polled every frame. If the failure left
     * the scan owed, the loop would open(2) sixty times a second forever. */
    check(!s.due(0), "gone: the failed scan still reset the timer");
    check(s.due(hv::kRescanIntervalMs), "gone: and it retries on the interval");

    /* The same backoff, defended from the other side: a ring still draining
     * events from a process that has exited must not force a scan per event. */
    s.note_address(0xdeadbeef000ull);
    check(!s.due(0), "gone: leftover events do not force a rescan");

    /* The last known map survives, because M2.3 freezes the last frame and
     * leaves the UI interactive. That is impossible from an empty list. */
    hv::MapsScanner t(0x7FFFFFFF);
    t.scan_text(kSampleMap, 0);
    check(t.scan(1) == hv::ScanStatus::TargetGone, "gone: the target dies");
    check(t.regions().size() == 11, "gone: its last map is kept");
    check(!t.bounds().empty(), "gone: and so are its bounds");
}

/* --- the label ------------------------------------------------------------ */

void test_the_arena_label() {
    char buf[hv::kArenaLabelMax];

    hv::format_arena_label(buf, sizeof buf, true, 0);
    check(std::strcmp(buf, "Main") == 0, "label: main arena only");

    hv::format_arena_label(buf, sizeof buf, true, 1);
    check(std::strcmp(buf, "Main + 1 thread") == 0, "label: one thread arena");

    hv::format_arena_label(buf, sizeof buf, true, 7);
    check(std::strcmp(buf, "Main + 7 threads") == 0, "label: several");

    /* A target whose main arena has not been created yet, or which allocates
     * only from threads. "Main" would be a plausible-looking lie. */
    hv::format_arena_label(buf, sizeof buf, false, 3);
    check(std::strcmp(buf, "3 threads") == 0, "label: no main arena");

    hv::format_arena_label(buf, sizeof buf, false, 0);
    check(std::strcmp(buf, "None") == 0, "label: nothing found");

    /* An undersized buffer produces an empty string, not a partial label. */
    char tiny[4] = {'x', 'x', 'x', 'x'};
    check(hv::format_arena_label(tiny, sizeof tiny, true, 12) == 0 &&
              tiny[0] == '\0',
          "label: an undersized buffer is refused, not overrun");
    check(hv::format_arena_label(nullptr, 0, true, 0) == 0,
          "label: a null buffer is refused");
}

} // namespace

int main() {
    test_a_line_parses_into_its_fields();
    test_classification_is_by_name_then_permissions();
    test_thread_arenas_are_recognised_by_alignment();
    test_malformed_lines_are_rejected();
    test_a_scan_summarises_the_map();
    test_lookup_finds_the_region_holding_an_address();
    test_a_torn_read_costs_one_region_not_the_scan();
    test_rescanning_state_is_reset_not_accumulated();
    test_the_rescan_timer();
    test_an_address_outside_the_bounds_forces_a_rescan();
    test_scanning_our_own_map();
    test_a_dead_target_is_reported_as_gone();
    test_the_arena_label();

    if (g_failures != 0) {
        std::fprintf(stderr, "proc_maps_test: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("proc_maps_test: the line parser, region classification, arena "
                "detection, bounds, torn reads, the rescan policy and a dead "
                "target all hold\n");
    return 0;
}
