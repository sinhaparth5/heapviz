/* heapviz - virtual memory segment scanner (M2.1).
 *
 * Copyright (C) 2026 Parth Sinha
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "tui/proc_maps.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>

#include <fcntl.h>
#include <unistd.h>

namespace hv {

namespace {

/* The kernel's format, from fs/proc/task_mmu.c:
 *
 *   start-end perms offset dev inode<pad>path
 *
 * Every numeric field is a fixed-radix run of characters with no sign and no
 * prefix, so each one is a loop rather than a call. `end` moves to one past the
 * last digit consumed; a field that consumed nothing is a parse failure, which
 * is what distinguishes a torn line from a short one. */

bool parse_hex(std::string_view s, std::size_t &i, std::uint64_t &out) noexcept {
    const std::size_t start = i;
    std::uint64_t v = 0;
    for (; i < s.size(); ++i) {
        const char c = s[i];
        unsigned d;
        if      (c >= '0' && c <= '9') d = static_cast<unsigned>(c - '0');
        else if (c >= 'a' && c <= 'f') d = static_cast<unsigned>(c - 'a') + 10;
        else if (c >= 'A' && c <= 'F') d = static_cast<unsigned>(c - 'A') + 10;
        else break;

        /* A 64-bit address is 16 hex digits. More than that is not a long
         * address, it is a line that is not an address at all. */
        if (i - start >= 16) return false;
        v = (v << 4) | d;
    }
    if (i == start) return false;
    out = v;
    return true;
}

bool parse_dec(std::string_view s, std::size_t &i, std::uint64_t &out) noexcept {
    const std::size_t start = i;
    std::uint64_t v = 0;
    for (; i < s.size() && s[i] >= '0' && s[i] <= '9'; ++i) {
        if (v > (UINT64_MAX - 9) / 10) return false;
        v = v * 10 + static_cast<std::uint64_t>(s[i] - '0');
    }
    if (i == start) return false;
    out = v;
    return true;
}

bool skip_spaces(std::string_view s, std::size_t &i) noexcept {
    const std::size_t start = i;
    while (i < s.size() && s[i] == ' ') ++i;
    return i != start;
}

/* Classification is by path first, because the bracketed names are exact and
 * unambiguous, and only then by permissions. The order matters: [heap] is
 * anonymous rw-p as well, and answering "anonymous" for it would lose the one
 * region we can name with certainty. */
RegionKind classify(std::string_view path, std::uint64_t inode,
                    std::uint8_t perms) noexcept {
    if (path == "[heap]")  return RegionKind::Heap;
    if (path == "[stack]") return RegionKind::Stack;

    /* [vdso], [vvar], [vsyscall], and the kernel's named anonymous mappings.
     * None are allocatable, and guessing at [anon:...] names would be reading
     * meaning into a string the application chose. */
    if (!path.empty() && path.front() == '[') return RegionKind::Other;

    if (!path.empty() || inode != 0) return RegionKind::File;

    /* Anonymous. Only private and writable counts: a PROT_NONE reservation is
     * the 64 MiB an arena has asked for and not yet committed, and treating it
     * as heap would put tens of megabytes of address space on the map that
     * nothing can be allocated in. */
    constexpr std::uint8_t kWant = kPermRead | kPermWrite | kPermPrivate;
    return (perms & kWant) == kWant ? RegionKind::Anon : RegionKind::Other;
}

/* Reads a file whose size stat(2) reports as zero, which is every file in
 * /proc: the content is generated as it is read, so the only way to learn the
 * length is to read until EOF. `out` keeps its capacity between calls. */
ScanStatus read_all(const char *path, std::string &out) {
    const int fd = ::open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        if (errno == ENOENT || errno == ESRCH) return ScanStatus::TargetGone;
        if (errno == EACCES || errno == EPERM) return ScanStatus::Denied;
        return ScanStatus::ReadFailed;
    }

    out.clear();
    ScanStatus st = ScanStatus::Ok;
    constexpr std::size_t kChunk = 16 * 1024;

    for (;;) {
        const std::size_t used = out.size();
        out.resize(used + kChunk);

        const ssize_t n = ::read(fd, out.data() + used, kChunk);
        if (n < 0) {
            out.resize(used);
            if (errno == EINTR) continue;
            /* The target died between the open and the read. The kernel reports
             * that as ESRCH here rather than as a short read, so it has to be
             * caught in both places. */
            st = (errno == ESRCH || errno == ENOENT) ? ScanStatus::TargetGone
               : (errno == EACCES || errno == EPERM) ? ScanStatus::Denied
                                                     : ScanStatus::ReadFailed;
            break;
        }

        out.resize(used + static_cast<std::size_t>(n));
        if (n == 0) break;
    }

    ::close(fd);
    return st;
}

} // namespace

bool parse_maps_line(std::string_view line, Region &out) noexcept {
    while (!line.empty() && (line.back() == '\n' || line.back() == '\r'))
        line.remove_suffix(1);

    std::size_t i = 0;
    Region r;

    if (!parse_hex(line, i, r.start)) return false;
    if (i >= line.size() || line[i] != '-') return false;
    ++i;
    if (!parse_hex(line, i, r.end)) return false;
    if (r.end < r.start) return false; /* an equal pair is legal; inverted is not */

    if (!skip_spaces(line, i)) return false;

    /* Exactly four permission characters, each either its letter or '-'. Any
     * other shape means the columns are not where we think they are, so the
     * rest of the line cannot be trusted either. */
    if (i + 4 > line.size()) return false;
    constexpr char kLetters[4] = {'r', 'w', 'x', 'p'};
    constexpr std::uint8_t kBits[4] = {kPermRead, kPermWrite, kPermExec,
                                       kPermPrivate};
    for (int b = 0; b < 4; ++b, ++i) {
        if (line[i] == kLetters[b])
            r.perms = static_cast<std::uint8_t>(r.perms | kBits[b]);
        else if (line[i] == '-')         { /* absent */ }
        else if (b == 3 && line[i] == 's') { /* shared, the other value of 'p' */ }
        else return false;
    }

    if (!skip_spaces(line, i)) return false;
    if (!parse_hex(line, i, r.file_offset)) return false;

    if (!skip_spaces(line, i)) return false;
    std::uint64_t dev_major = 0, dev_minor = 0;
    if (!parse_hex(line, i, dev_major)) return false;
    if (i >= line.size() || line[i] != ':') return false;
    ++i;
    if (!parse_hex(line, i, dev_minor)) return false;

    if (!skip_spaces(line, i)) return false;
    if (!parse_dec(line, i, r.inode)) return false;

    /* Everything after the padding is the path, spaces and all. The kernel pads
     * this column to a fixed width, so the separator is a run of spaces, but a
     * mapped file may legitimately contain one in its name -- and a deleted one
     * arrives as "/path/to/lib.so (deleted)". Taking the remainder verbatim is
     * the only reading that keeps both intact. */
    std::string_view path;
    if (i < line.size()) {
        skip_spaces(line, i);
        path = line.substr(i);
    }

    r.kind = classify(path, r.inode, r.perms);
    r.thread_arena = r.kind == RegionKind::Anon &&
                     (r.start & (kThreadArenaAlign - 1)) == 0 &&
                     r.size() <= kThreadArenaAlign;

    out = r;
    return true;
}

const char *region_kind_str(RegionKind k) noexcept {
    switch (k) {
    case RegionKind::Heap:  return "heap";
    case RegionKind::Anon:  return "anon";
    case RegionKind::Stack: return "stack";
    case RegionKind::File:  return "file";
    case RegionKind::Other: return "other";
    }
    return "other";
}

const char *scan_status_str(ScanStatus s) noexcept {
    switch (s) {
    case ScanStatus::Ok:         return "ok";
    case ScanStatus::TargetGone: return "target exited";
    case ScanStatus::Denied:     return "permission denied";
    case ScanStatus::ReadFailed: return "read failed";
    }
    return "read failed";
}

std::size_t format_arena_label(char *buf, std::size_t n, bool main_arena,
                               unsigned thread_arenas) noexcept {
    if (buf == nullptr || n == 0) return 0;

    int len;
    if (main_arena && thread_arenas == 0) {
        len = std::snprintf(buf, n, "Main");
    } else if (main_arena) {
        len = std::snprintf(buf, n, "Main + %u thread%s", thread_arenas,
                            thread_arenas == 1 ? "" : "s");
    } else if (thread_arenas != 0) {
        len = std::snprintf(buf, n, "%u thread%s", thread_arenas,
                            thread_arenas == 1 ? "" : "s");
    } else {
        /* Before the first scan, and for a target that has not allocated
         * enough to need a heap yet. Both are real states, and neither is
         * "Main". */
        len = std::snprintf(buf, n, "None");
    }

    if (len < 0 || static_cast<std::size_t>(len) >= n) {
        buf[0] = '\0';
        return 0;
    }
    return static_cast<std::size_t>(len);
}

MapsScanner::MapsScanner(int pid) : pid_(pid) {
    char path[64];
    std::snprintf(path, sizeof path, "/proc/%d/maps", pid);
    path_ = path;
    regions_.reserve(256); /* a typical process maps a few hundred regions */
}

ScanStatus MapsScanner::scan(std::uint32_t now_ms) {
    ScanStatus st = read_all(path_.c_str(), text_);

    /* A process that has exited but not yet been reaped still has a /proc entry,
     * and its maps file opens and reads cleanly as zero bytes. Parsed at face
     * value that is a successful scan of a process with no memory, which clears
     * the region list -- so the display of a target that just died would go
     * blank a fraction of a second after it died, which is precisely the frame
     * worth keeping. A live process always has mappings, so empty means gone. */
    if (st == ScanStatus::Ok && text_.empty()) st = ScanStatus::TargetGone;

    if (st != ScanStatus::Ok) {
        status_ = st;
        /* The timer is still reset. A dead target that is polled every 500 ms
         * is one open(2) per half second; one polled every frame because the
         * failure left the scan owed is sixty. */
        last_scan_ms_ = now_ms;
        forced_ = false;
        return st;
    }
    return scan_text(text_, now_ms);
}

ScanStatus MapsScanner::scan_text(std::string_view text, std::uint32_t now_ms) {
    regions_.clear();
    main_arena_    = false;
    thread_arenas_ = 0;

    std::size_t i = 0;
    while (i < text.size()) {
        std::size_t nl = text.find('\n', i);
        if (nl == std::string_view::npos) nl = text.size();

        const std::string_view line = text.substr(i, nl - i);
        i = nl + 1;

        if (line.empty()) continue;

        Region r;
        if (!parse_maps_line(line, r)) { ++malformed_; continue; }

        if (r.kind == RegionKind::Heap) main_arena_ = true;
        if (r.thread_arena) ++thread_arenas_;
        regions_.push_back(r);
    }

    status_ = ScanStatus::Ok;
    rebuild(now_ms);
    return ScanStatus::Ok;
}

void MapsScanner::rebuild(std::uint32_t now_ms) noexcept {
    bounds_ = AddrRange{};
    for (const Region &r : regions_) {
        if (!r.allocatable() || r.size() == 0) continue;
        if (bounds_.empty()) {
            bounds_.base = r.start;
            bounds_.end  = r.end;
        } else {
            bounds_.base = std::min(bounds_.base, r.start);
            bounds_.end  = std::max(bounds_.end, r.end);
        }
    }

    last_scan_ms_ = now_ms;
    forced_ = false;
    ++scans_;
}

bool MapsScanner::due(std::uint32_t now_ms) const noexcept {
    /* `forced_` starts true, which is the whole of "the first scan is owed".
     * Asking `scans_ == 0` as well would look like the same statement but is
     * not: a failed scan leaves it at zero, so a target that is already gone
     * would come due every frame forever, which is the case the backoff in
     * `scan` exists to prevent. */
    if (forced_) return true;
    /* Unsigned subtraction, so the 49-day wrap of a 32-bit millisecond clock
     * gives the right delta rather than a scan that never comes due again. */
    return static_cast<std::uint32_t>(now_ms - last_scan_ms_) >= kRescanIntervalMs;
}

void MapsScanner::note_address(std::uint64_t addr) noexcept {
    if (forced_) return;
    /* A finished process cannot map anything else, so events still draining out
     * of its ring have nothing to tell us about its address space. Without this
     * the backoff above is defeated from the other side: every leftover event
     * outside the last known bounds would force another open(2). */
    if (status_ == ScanStatus::TargetGone) return;
    if (bounds_.empty() || addr < bounds_.base || addr >= bounds_.end)
        forced_ = true;
}

const Region *MapsScanner::find(std::uint64_t addr) const noexcept {
    /* The kernel emits the map in ascending address order and the regions are
     * disjoint, so the list arrives sorted and needs no sorting pass. */
    std::size_t lo = 0, hi = regions_.size();
    while (lo < hi) {
        const std::size_t mid = lo + (hi - lo) / 2;
        const Region &r = regions_[mid];
        if (addr < r.start)     hi = mid;
        else if (addr >= r.end) lo = mid + 1;
        else                    return &r;
    }
    return nullptr;
}

} // namespace hv
