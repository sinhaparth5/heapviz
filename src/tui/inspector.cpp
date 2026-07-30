/* heapviz - the chunk inspector panel (M5.2).
 *
 * Copyright (C) 2026 Parth Sinha
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "tui/inspector.h"

#include "tui/heat_color.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace hv {
namespace {

/* Column the values start in. Wide enough for the longest label plus a gap, and
 * fixed so the figures line up down the panel -- a column of numbers that
 * stepped left and right with the label beside it is measurably harder to
 * read than one that does not. */
constexpr int kValueCol = 12;

/* One past the last column `Address` writes into: `0x` plus sixteen hex digits.
 * The only thing that shares a row with it needs to know where it ends. */
constexpr int kAddressEndCol = kValueCol + 18;

const ChunkDetail kNothing{};

} // namespace

const char *chunk_status_str(ChunkStatus s) noexcept {
    switch (s) {
    case ChunkStatus::Unallocated: return "UNALLOCATED";
    case ChunkStatus::Active:      return "ACTIVE (ptmalloc)";
    case ChunkStatus::Mmapped:     return "MMAPPED";
    case ChunkStatus::Freed:       return "FREED";
    }
    return "UNALLOCATED";
}

const ChunkDetail &ChunkInspector::current() const noexcept {
    if (pos_ >= found_.size()) return kNothing;
    return found_[pos_];
}

void ChunkInspector::rescan(const ChunkTable &table, const HeatMap &map,
                            std::size_t cell) {
    found_.clear();
    total_ = 0;
    ++scans_;

    const Grid &g = map.grid();
    const Chunk *slots = table.slots();
    const std::size_t n = table.slot_count();

    for (std::size_t i = 0; i < n; ++i) {
        const Chunk &c = slots[i];
        if (c.state == kChunkEmpty || c.key == 0) continue;

        /* Through the map's own conversion, never a raw shift: with a RegionMap
         * in play the grid buckets packed offsets, and an address bucketed
         * directly would land in whatever region happens to sit at that
         * distance from the lowest one. */
        std::uint64_t coord = 0;
        if (!map.coordinate_of(c.key, coord)) continue;

        std::size_t idx = 0;
        if (!g.index_of(coord, idx) || idx != cell) continue;

        ++total_;

        ChunkDetail d;
        d.addr     = c.key;
        d.size     = c.size;
        d.usable   = c.usable;
        d.alloc_ms = c.alloc_ms;
        d.free_ms  = c.free_ms;
        d.tid      = c.tid;
        d.exact    = (c.flags & kChunkFlagRefined) != 0;
        d.overhead = d.exact ? c.overhead_exact : overhead_of(c.size, c.usable);

        if (c.state == kChunkFreed) {
            d.status = ChunkStatus::Freed;
        } else if ((c.flags & kChunkFlagMmapped) != 0) {
            d.status = ChunkStatus::Mmapped;
        } else {
            d.status = ChunkStatus::Active;
        }

        if (found_.size() < kInspectorCandidates) {
            found_.push_back(d);
            continue;
        }

        /* Full: keep the largest rather than the first found. Probe order is a
         * property of the hash table, so a first-K list would rename the
         * headline chunk after an unrelated rehash -- the panel would appear to
         * jump to a different allocation with nothing in the target having
         * changed. */
        auto smallest = std::min_element(
            found_.begin(), found_.end(),
            [](const ChunkDetail &a, const ChunkDetail &b) {
                return a.size < b.size;
            });
        if (smallest != found_.end() && smallest->size < d.size) *smallest = d;
    }

    /* Largest first, and ties broken by address so the order is total: two
     * chunks of the same size would otherwise swap places between scans
     * depending on which the table walk reached first. */
    std::sort(found_.begin(), found_.end(),
              [](const ChunkDetail &a, const ChunkDetail &b) {
                  if (a.size != b.size) return a.size > b.size;
                  return a.addr < b.addr;
              });

    /* Hold the selection across the rescan where the same chunk is still here.
     * Without this, `Tab` to the third chunk in a busy cell would snap back to
     * the largest four times a second. */
    pos_ = 0;
    if (selected_ != 0) {
        for (std::size_t i = 0; i < found_.size(); ++i)
            if (found_[i].addr == selected_) { pos_ = i; break; }
    }
    selected_ = found_.empty() ? 0 : found_[pos_].addr;
    cell_ = cell;
}

bool ChunkInspector::refresh(const ChunkTable &table, const HeatMap &map,
                             const MapCursor &cur, std::uint32_t now_ms,
                             bool dirty) {
    const Grid &g = map.grid();
    if (!g.valid()) {
        const bool had = !found_.empty();
        found_.clear();
        total_ = 0;
        pos_   = 0;
        cell_  = SIZE_MAX;
        return had;
    }

    const std::size_t cell = cur.index(g);
    const bool moved = cell != cell_;
    const bool due =
        static_cast<std::uint32_t>(now_ms - last_ms_) >= kInspectRefreshMs;
    if (!moved && !dirty && !due) return false;
    last_ms_ = now_ms;

    const std::uint64_t was_addr = found_.empty() ? 0 : found_[pos_].addr;
    const std::size_t   was_total = total_;

    /* The cheap answer first. A cell with no live bytes, no overhead and no
     * free still fading cannot hold a chunk, and on a real heap that is most of
     * the screen -- so most cursor positions never pay for the scan. Freed
     * records are only worth looking for while their fade is running, which is
     * exactly as long as the table keeps them (M3.4). */
    const bool possible =
        cell < map.cell_count() &&
        (cell_occupied(map.at(cell)) ||
         cell_animating(map.at(cell), map.timings(), now_ms));

    if (!possible) {
        found_.clear();
        total_    = 0;
        pos_      = 0;
        selected_ = 0;
        cell_     = cell;
        return was_addr != 0 || was_total != 0 || moved;
    }

    rescan(table, map, cell);

    const std::uint64_t now_addr = found_.empty() ? 0 : found_[pos_].addr;
    if (was_addr != now_addr || was_total != total_ || moved) return true;

    /* Nothing about the selection changed -- but `Lifetime` is a function of
     * the clock, so a live chunk's panel differs from the one drawn a quarter
     * of a second ago even though the model did not move. Reporting no change
     * there would freeze the number on an idle display, which looks exactly
     * like a hung session. A freed chunk's lifetime is fixed and needs no such
     * tick. */
    return !found_.empty() && current().status != ChunkStatus::Freed;
}

bool ChunkInspector::cycle() noexcept {
    if (found_.size() < 2) return false;
    pos_ = (pos_ + 1) % found_.size();
    selected_ = found_[pos_].addr;
    return true;
}

void ChunkInspector::draw(Framebuffer &fb, Rect area, const Grid &g,
                          const MapCursor &cur, const RegionMap *regions,
                          std::uint32_t now_ms) const noexcept {
    if (area.w <= 0 || area.h <= 0) return;

    char line[192];
    char num[48];
    char human[32];

    panel_rule(fb, area, " CHUNK INSPECTOR ", style_.frame, style_.accent,
               style_.bg);

    /* Before the first `/proc/<pid>/maps` scan there is no coordinate space, so
     * the cursor's coordinate means nothing yet. Said out loud rather than
     * shown as address zero, which would be a real-looking answer to a question
     * that has not been asked yet. */
    if (!g.valid()) {
        panel_text(fb, area, 2, 1, "waiting for the target's memory map",
                   style_.dim, style_.bg);
        return;
    }

    /* Where the cursor is, whether or not anything is there. This is the line
     * that makes an empty cell read as a deliberate answer rather than as a
     * panel that failed to load: the address is real, it is just unallocated. */
    std::uint64_t addr = cur.coord();
    if (regions != nullptr && !regions->empty()) {
        std::uint64_t real = 0;
        if (regions->to_addr(cur.coord(), real)) addr = real;
    }

    const ChunkDetail &d = current();
    const bool have = d.status != ChunkStatus::Unallocated;

    int dy = 1;
    const auto field = [&](const char *label, const char *value, Rgb colour,
                           bool numeric = false) {
        panel_text(fb, area, 2, dy, label, style_.dim, style_.bg);
        if (numeric)
            panel_numeric_right(fb, area, dy, value, colour, style_.dim,
                                style_.bg);
        else
            panel_text(fb, area, kValueCol, dy, value, colour, style_.bg);
        ++dy;
    };

    std::snprintf(line, sizeof line, "0x%016llx",
                  static_cast<unsigned long long>(have ? d.addr : addr));
    field("Address", line, have ? style_.ink : style_.dim, true);

    if (!have) {
        /* The empty state. Named, then explained, then told what to press --
         * three short lines rather than a blank panel, because a blank panel
         * next to a working map reads as a bug in the panel.
         *
         * Kept short enough to survive M5.3's split of the bottom block: a
         * sentence that has to be truncated stops being an explanation and
         * becomes another thing that looks broken. */
        field("Status", chunk_status_str(ChunkStatus::Unallocated), style_.dim);
        panel_text(fb, area, 2, dy++, "address space the target has not used",
                   style_.dim, style_.bg);
        panel_text(fb, area, 2, dy++, "n / N find the next occupied cell",
                   style_.dim, style_.bg);
        return;
    }

    format_count(num, sizeof num, d.size);
    format_byte_size(human, sizeof human, d.size);
    std::snprintf(line, sizeof line, "%s bytes (%s)", num, human);
    field("User Size", line, style_.ink, true);

    /* Real size, with the overhead called out. The `~` is load-bearing: an
     * unrefined chunk's overhead is `usable - size`, which cannot see the chunk
     * header and is therefore always an underestimate. Printing it with the
     * same authority as a measured one is the difference between a profiler and
     * a plausible-looking number. */
    format_count(num, sizeof num, d.chunk_bytes());
    std::snprintf(line, sizeof line, "%s bytes (%s%u B overhead%s)", num,
                  d.exact ? "" : "~", d.overhead,
                  d.exact ? ", measured" : ", inferred");
    field("Real Size", line, style_.ink, true);

    field("Status", chunk_status_str(d.status),
          d.status == ChunkStatus::Freed ? style_.freed : style_.live);

    /* Lifetime: how long it has been alive for something live, and how long it
     * lived for something freed. Two different questions with one label,
     * distinguished by the word after the number rather than by the reader
     * having to remember which status they are looking at. */
    const std::uint32_t end_ms =
        d.status == ChunkStatus::Freed ? d.free_ms : now_ms;
    const std::uint32_t ms = end_ms > d.alloc_ms ? end_ms - d.alloc_ms : 0;
    std::snprintf(line, sizeof line, "%.1f s %s",
                  static_cast<double>(ms) / 1000.0,
                  d.status == ChunkStatus::Freed ? "(lived)" : "(so far)");
    field("Lifetime", line, style_.ink, true);

    /* How crowded the cell is, right-aligned on the status row. A cell is a
     * span of addresses, not an allocation, and a panel that showed one chunk
     * with no hint that four hundred others share the cell would be the most
     * misleading thing on the screen. */
    if (total_ > 1) {
        if (found_.size() < total_)
            std::snprintf(line, sizeof line, " %zu of %zu here (%zu reachable) "
                          "- Tab ", pos_ + 1, total_, found_.size());
        else
            std::snprintf(line, sizeof line, " %zu of %zu in this cell - Tab ",
                          pos_ + 1, total_);
        /* It shares its row with the address, and since M5.3 split the bottom
         * block this panel can be 48 columns wide -- narrow enough for the two
         * to land on each other. An address half overwritten by a count is a
         * wrong answer to the panel's first question, so the count gives way:
         * first to a short form, then to nothing. */
        if (area.w - static_cast<int>(std::strlen(line)) <= kAddressEndCol) {
            std::snprintf(line, sizeof line, " %zu/%zu Tab ", pos_ + 1, total_);
            if (area.w - static_cast<int>(std::strlen(line)) <= kAddressEndCol)
                return;
        }
        panel_text_right(fb, area, 1, line, style_.accent, style_.bg);
    }
}

} // namespace hv
