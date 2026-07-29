/* heapviz - virtual memory segment scanner (M2.1).
 *
 * Copyright (C) 2026 Parth Sinha
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The interceptor reports allocations but not the shape of the address space
 * they land in: it watches `malloc` return 0x55a4c8e05010 and has no way to
 * know whether that is the brk heap, a thread arena, or a plain mmap. The
 * kernel knows, and publishes it as text in /proc/<pid>/maps. This turns that
 * text into regions.
 *
 * TWO CALLERS WANTING DIFFERENT THINGS
 * ------------------------------------
 * The header bar wants one range it can print, so `bounds` is the union of
 * every region an allocation could land in. The grid wants the opposite: a
 * union stretching from a brk heap at 0x55... to an mmap arena at 0x7f... is a
 * 47-bit span that is almost entirely hole, and `Grid::covers_whole_span` goes
 * false trying to bucket it (see the comment there, which names M2 as the fix).
 * So the region list is kept as a list rather than being collapsed into the
 * bounds, and a later task can grid the regions separately. Both views come
 * from one scan; neither is derived from the other after the fact.
 *
 * WHY THE PARSER IS HAND-ROLLED
 * -----------------------------
 * `sscanf` on every line of a map with a few thousand entries, twice a second,
 * for the lifetime of the session, to extract fields that are plain hex runs.
 * The format is fixed by the kernel and is not going to surprise us, so the
 * scanning is direct.
 *
 * WHAT A SCAN CANNOT PROMISE
 * --------------------------
 * /proc/<pid>/maps is a seq_file generated on demand, and the target keeps
 * running while it is read. A map that changes mid-read yields a snapshot that
 * was never simultaneously true. Nothing here can prevent that, so malformed
 * lines are counted and skipped rather than failing the scan: a torn read must
 * cost at most one region for 500 ms, never a blank display.
 */

#ifndef HEAPVIZ_TUI_PROC_MAPS_H
#define HEAPVIZ_TUI_PROC_MAPS_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace hv {

/* Re-scan cadence. Fast enough that a heap growing by brk is visible within a
 * few frames, slow enough that parsing a large map is not a per-frame cost.
 * The other trigger is `note_address`, which is what actually keeps the display
 * correct: a heap that grows mid-frame is noticed by the event that lands
 * outside the bounds, not by the timer that would have caught it 400 ms later. */
constexpr std::uint32_t kRescanIntervalMs = 500;

/* glibc reserves a thread arena with a 64 MiB-aligned mmap of HEAP_MAX_SIZE and
 * commits a prefix of it, so an anonymous region starting on this boundary is a
 * candidate arena. It is a heuristic, not a fact: an application mmapping 64 MiB
 * on the same alignment is indistinguishable from the outside. It labels the
 * header bar, and nothing is computed from it. */
constexpr std::uint64_t kThreadArenaAlign = std::uint64_t{64} << 20;

/* Enough for "Main + 4294967295 threads" and its terminator. */
constexpr std::size_t kArenaLabelMax = 32;

/* Permission bits, in the order /proc prints them. */
constexpr std::uint8_t kPermRead    = 1u << 0;
constexpr std::uint8_t kPermWrite   = 1u << 1;
constexpr std::uint8_t kPermExec    = 1u << 2;
constexpr std::uint8_t kPermPrivate = 1u << 3; /* 'p'; clear means 's', shared */

enum class RegionKind : std::uint8_t {
    Heap,  /* [heap]: the main arena, grown by brk                        */
    Anon,  /* anonymous rw-p: thread arenas and large mmap'd chunks       */
    Stack, /* [stack]                                                     */
    File,  /* file-backed: the binary, its libraries, mapped data         */
    Other, /* [vdso], [vvar], guard pages, anything not allocatable       */
};

/* Why a scan produced nothing useful. `TargetGone` is the one the UI acts on;
 * the rest are reported and retried. */
enum class ScanStatus : std::uint8_t {
    Ok,
    TargetGone,  /* ENOENT on open, or ESRCH on read: the pid is finished */
    Denied,      /* EACCES/EPERM: another user's process                  */
    ReadFailed,  /* anything else, kept distinct so it is not read as death */
};

struct Region {
    std::uint64_t start = 0;
    std::uint64_t end   = 0; /* exclusive, as the kernel prints it */
    std::uint64_t file_offset = 0;
    std::uint64_t inode = 0;
    RegionKind    kind  = RegionKind::Other;
    std::uint8_t  perms = 0;
    bool          thread_arena = false;

    std::uint64_t size() const noexcept { return end - start; }

    bool contains(std::uint64_t addr) const noexcept {
        return addr >= start && addr < end;
    }

    /* The regions an allocation can come from, and therefore the ones the map
     * draws and the bounds cover. A file mapping is not one of them even when
     * it is writable: the target's .data segment is not its heap. */
    bool allocatable() const noexcept {
        return kind == RegionKind::Heap || kind == RegionKind::Anon;
    }
};

/* A half-open address range. Empty until the first successful scan, which the
 * header bar has to render as something other than 0x0 - 0x0. */
struct AddrRange {
    std::uint64_t base = 0;
    std::uint64_t end  = 0;

    bool empty() const noexcept { return end <= base; }
    std::uint64_t span() const noexcept { return empty() ? 0 : end - base; }
};

/* One line of /proc/<pid>/maps:
 *
 *   55a4c8e00000-55a4c8e21000 rw-p 00000000 00:00 0     [heap]
 *
 * Returns false for a line that does not parse, which during a torn read is
 * expected rather than exceptional. `out` is untouched on failure.
 *
 * A trailing newline is tolerated. Paths keep their internal spaces, because
 * the kernel pads the column with spaces and a mapped file is allowed to have
 * one in its name. */
bool parse_maps_line(std::string_view line, Region &out) noexcept;

const char *region_kind_str(RegionKind k) noexcept;
const char *scan_status_str(ScanStatus s) noexcept;

/* Formats the arena summary for the header bar: "Main", "Main + 3 threads",
 * "3 threads", or "None" when a scan has found neither. `buf` must hold
 * kArenaLabelMax bytes. Returns the length written. */
std::size_t format_arena_label(char *buf, std::size_t n, bool main_arena,
                               unsigned thread_arenas) noexcept;

class MapsScanner {
public:
    explicit MapsScanner(int pid);

    int pid() const noexcept { return pid_; }

    /* Reads and parses /proc/<pid>/maps. On TargetGone the previous regions are
     * deliberately kept: M2.3 freezes the last frame and leaves the UI
     * interactive, which cannot be done from an empty region list. */
    ScanStatus scan(std::uint32_t now_ms);

    /* The same parse over text supplied by the caller. The file read is the
     * only part of this class that needs a live process, so keeping it on one
     * side of a seam is what lets the parsing, the classification, the bounds
     * and the arena count all be tested against fixed input. */
    ScanStatus scan_text(std::string_view text, std::uint32_t now_ms);

    /* Whether a scan is owed: the 500 ms timer has elapsed, an address arrived
     * from outside the known bounds, or nothing has been scanned yet. */
    bool due(std::uint32_t now_ms) const noexcept;

    /* Feed every drained event's pointer through this. An address outside the
     * current bounds means the map moved under us, and the next `due` is true
     * immediately rather than up to 500 ms later. Cheap enough for the drain
     * loop: two compares against members already in cache. */
    void note_address(std::uint64_t addr) noexcept;

    ScanStatus status() const noexcept { return status_; }
    bool target_gone() const noexcept { return status_ == ScanStatus::TargetGone; }
    bool scanned() const noexcept { return scans_ != 0; }

    /* Ascending by address, as the kernel emits them. */
    const std::vector<Region> &regions() const noexcept { return regions_; }

    /* Union of the allocatable regions. Mostly hole in a real process; see the
     * header comment for why it is not the grid's input. */
    AddrRange bounds() const noexcept { return bounds_; }

    /* The region covering an address, or null. Binary search over the ascending
     * region list. */
    const Region *find(std::uint64_t addr) const noexcept;

    bool     main_arena()    const noexcept { return main_arena_; }
    unsigned thread_arenas() const noexcept { return thread_arenas_; }
    std::size_t arena_label(char *buf, std::size_t n) const noexcept {
        return format_arena_label(buf, n, main_arena_, thread_arenas_);
    }

    std::uint64_t scans() const noexcept { return scans_; }
    std::uint64_t malformed_lines() const noexcept { return malformed_; }

private:
    void rebuild(std::uint32_t now_ms) noexcept;

    int         pid_;
    std::string path_; /* built once; a scan must not allocate to name its file */
    std::string text_; /* reused across scans, so the steady state does not grow */

    std::vector<Region> regions_;
    AddrRange           bounds_{};
    ScanStatus          status_ = ScanStatus::Ok;

    std::uint32_t last_scan_ms_ = 0;
    bool          forced_       = true; /* the first scan is always owed */
    bool          main_arena_   = false;
    unsigned      thread_arenas_ = 0;
    std::uint64_t scans_        = 0;
    std::uint64_t malformed_    = 0;
};

} // namespace hv

#endif /* HEAPVIZ_TUI_PROC_MAPS_H */
