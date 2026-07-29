/* heapviz - live chunk tracking table (M3.2).
 *
 * Copyright (C) 2026 Parth Sinha
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * One record per allocation the target is holding, keyed by pointer. Every
 * drained event is a lookup, so at ten million allocations a second this table
 * is the hot path of the whole consumer: the frame budget is spent here or it
 * is not spent at all.
 *
 * WHY ROBIN HOOD, AND WHY NO TOMBSTONES
 * -------------------------------------
 * The workload this tool exists to watch is churn: a stream of inserts and
 * erasures at roughly equal rates, forever. That is the exact workload a
 * tombstone table handles worst -- deleted markers accumulate, probe sequences
 * lengthen, and lookups degrade steadily over a session with no change in the
 * live set to explain it. Backward-shift deletion keeps the table as compact
 * after a million free()s as it was before them.
 *
 * Robin Hood insertion (steal the slot from a record closer to home, carry the
 * evicted one onward) bounds the variance rather than the mean. It costs a
 * little on insert and buys a short worst-case probe, which is what matters
 * when a single slow lookup lands inside a 16 ms frame.
 *
 * WHY FIBONACCI HASHING
 * ---------------------
 * The keys are malloc'd pointers: 16-byte aligned, so the low four bits are
 * always zero, and clustered into a handful of arenas, so the high bits barely
 * move either. `ptr % capacity` on that input maps every allocation in an arena
 * onto a fraction of the table and leaves the rest empty. Multiplying by the
 * 64-bit golden-ratio constant spreads the informative middle bits across the
 * whole word, and the shift then takes them from the top.
 */

#ifndef HEAPVIZ_TUI_CHUNK_TABLE_H
#define HEAPVIZ_TUI_CHUNK_TABLE_H

#include <cstddef>
#include <cstdint>
#include <vector>

namespace hv {

/* Per-record flags, in one of the three bytes that were already padding. M2.2's
 * enrichment needs to know which records it has already corrected, and the
 * alternative was widening Chunk past 32 bytes -- which would cost a second
 * cache fetch on every probe that walks past a neighbour, forever, to serve a
 * display detail. */
enum ChunkFlags : std::uint8_t {
    kChunkFlagNone = 0,
    /* The exact chunk overhead has been read out of the target and folded into
     * the heat map, so folding it again would double-count. Cleared wholesale
     * by `HeatMap::rebuild`, which recomputes every cell from the approximate
     * figure and thereby undoes the correction. */
    kChunkFlagRefined = 1u << 0,
};

enum ChunkState : std::uint8_t {
    kChunkEmpty = 0, /* the slot has never held anything, or was shifted out */
    kChunkLive  = 1,
    kChunkFreed = 2, /* freed, kept only so the fade animation has something
                        to fade; M3.4 discards it once it is fully faded */
};

/* Exactly 32 bytes, so two records share a cache line and a probe that walks
 * past a neighbour costs no second fetch. The pinning static_assert is in the
 * .cpp; if a field is added, something else has to give.
 *
 * `key == 0` marks an empty slot. A successful malloc never returns null, so
 * the sentinel cannot collide with a real allocation.
 *
 * Timestamps are milliseconds since the session started, not the event's raw
 * nanoseconds. Aging (M3.4) works in units of 200-2000 ms, so millisecond
 * resolution is finer than anything that will be displayed, and it halves what
 * two timestamps cost. 32 bits of milliseconds is 49 days of session.
 */
struct Chunk {
    std::uint64_t key;      /* the pointer malloc returned                */
    std::uint32_t size;     /* what the caller asked for                  */
    std::uint32_t usable;   /* what the allocator actually gave           */
    std::uint32_t alloc_ms;
    std::uint32_t free_ms;  /* only meaningful when state is kChunkFreed  */
    std::uint32_t tid;
    std::uint8_t  state;
    std::uint8_t  flags;           /* kChunkFlag* above                       */
    /* The exact overhead M2.2 read out of the target, kept so the correction
     * can be *undone* when the chunk is freed. Without it the cell keeps the
     * exact figure while `on_free` removes the approximate one, and the
     * difference accumulates in the cell on every free.
     *
     * 16 bits because that is what was left of the padding; a chunk whose
     * overhead exceeds 64 KiB is simply left approximate, which is honest and
     * vanishingly rare. */
    std::uint16_t overhead_exact;
};

struct ChunkTableStats {
    std::uint64_t inserts   = 0;
    std::uint64_t updates   = 0; /* an insert onto a key already present  */
    std::uint64_t erases    = 0;
    std::uint64_t rehashes  = 0;
    std::uint64_t evictions = 0; /* freed records dropped to stay in budget */
    std::uint64_t drops     = 0; /* live records that could NOT be admitted */
    std::size_t   max_probe = 0; /* longest probe seen, for the health line */
};

class ChunkTable {
public:
    explicit ChunkTable(std::size_t initial_capacity = 1024);

    /* Sizes the table to hold `entries` without rehashing. Rounds up to a power
     * of two with the load factor already accounted for. */
    bool reserve(std::size_t entries);

    /* Bounded memory policy. Zero means unbounded, which is the default.
     *
     * When the table is at its cap, the oldest *freed* records are evicted
     * first: they exist only to be faded out, so losing the oldest of them
     * costs an animation nobody is still watching. A live allocation is never
     * evicted to make room -- that would silently corrupt the picture the tool
     * exists to show, turning a leak into a blank cell. If nothing is left to
     * evict, the new record is refused and `stats().drops` counts it, so the UI
     * can say so out loud rather than quietly showing less than the truth. */
    void set_max_entries(std::size_t n);

    void clear() noexcept;

    std::size_t size()     const noexcept { return size_; }
    std::size_t capacity() const noexcept { return slots_.size(); }
    std::size_t max_entries() const noexcept { return max_entries_; }
    double      load_factor() const noexcept;

    Chunk       *find(std::uint64_t key) noexcept;
    const Chunk *find(std::uint64_t key) const noexcept;

    /* Records a live allocation. Replaces any record already under `key`, which
     * is the normal case for an address the allocator has recycled. Returns
     * false only when the bounded-memory policy refused it. */
    bool insert_live(std::uint64_t key, std::uint32_t size,
                     std::uint32_t usable, std::uint32_t alloc_ms,
                     std::uint32_t tid);

    /* Moves a record to kChunkFreed, keeping it for the fade. False when the
     * key is unknown, which happens constantly and is not an error: heapviz
     * attaches to a process that already has a heap. */
    bool mark_freed(std::uint64_t key, std::uint32_t free_ms) noexcept;

    /* Removes a record outright, closing the gap by backward shift. */
    bool erase(std::uint64_t key) noexcept;

    /* Records the exact overhead for a key and marks it refined. False when the
     * key is unknown, or when the figure does not fit -- in which case the
     * record keeps its approximation rather than a truncated lie. */
    bool mark_refined(std::uint64_t key, std::uint32_t exact_overhead) noexcept;

    /* Clears every refinement mark. Called after a rebuild, which recomputes
     * overhead from the approximate figure and so undoes the corrections: a
     * record still marked refined would never be corrected again. */
    void clear_refined() noexcept;

    const ChunkTableStats &stats() const noexcept { return stats_; }

    /* Raw slot access for M3.3's per-cell aggregation, which walks the table
     * once per frame rather than probing it per cell. Slots with
     * `state == kChunkEmpty` are holes and must be skipped. */
    const Chunk *slots()      const noexcept { return slots_.data(); }
    std::size_t  slot_count() const noexcept { return slots_.size(); }

private:
    std::size_t ideal_slot(std::uint64_t key) const noexcept;
    std::size_t probe_distance(std::size_t slot) const noexcept;
    bool        grow_to(std::size_t new_capacity);
    bool        make_room();
    bool        pop_evictable();
    void        refill_freed_queue();
    void        push_freed(std::uint64_t key);
    void        place(Chunk rec);

    std::vector<Chunk> slots_;
    std::size_t size_        = 0;
    std::size_t mask_        = 0;
    unsigned    shift_       = 64;
    std::size_t max_entries_ = 0;

    /* Freed keys in the order they were freed, so eviction can take the oldest
     * without scanning. Keys rather than slot indices: a backward shift or a
     * rehash moves records between slots, and an index recorded here would go
     * stale the moment either happened.
     *
     * Strictly a cache. It can be empty (the cap was set after the frees had
     * already happened) or overrun (stale hints for keys since erased or
     * reused), so eviction rebuilds it from a scan of the table when it comes
     * up dry rather than concluding there is nothing to evict. That keeps the
     * queue an optimisation rather than the thing correctness rests on. */
    std::vector<std::uint64_t> freed_fifo_;
    std::size_t freed_head_ = 0;
    std::size_t freed_tail_ = 0;

    ChunkTableStats stats_{};
};

} // namespace hv

#endif /* HEAPVIZ_TUI_CHUNK_TABLE_H */
