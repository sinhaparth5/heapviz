/* heapviz - live chunk tracking table (M3.2).
 *
 * Copyright (C) 2026 Parth Sinha
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "tui/chunk_table.h"

#include <algorithm>
#include <bit>
#include <utility>

namespace hv {

static_assert(sizeof(Chunk) == 32, "a probe must not straddle two cache lines");
static_assert(alignof(Chunk) == 8, "Chunk alignment feeds its 32-byte size");

namespace {

/* 2^64 / phi. The point is that its bits are irregular: multiplying by it turns
 * a change anywhere in the key into a change in the high bits, which is where
 * the shift then reads from. */
constexpr std::uint64_t kFibonacci = 0x9E3779B97F4A7C15ull;

/* Below this a table costs more in rehashing than it saves in memory, and it
 * also keeps log2(capacity) >= 4 so the hash shift can never reach 64, which
 * would be undefined. */
constexpr std::size_t kMinCapacity = 16;

/* Grow at 85%. Robin Hood tolerates a high load factor -- that is most of the
 * point of it -- but the probe loop needs at least one empty slot to terminate,
 * so this can never reach 100. */
constexpr std::size_t kLoadNumerator   = 85;
constexpr std::size_t kLoadDenominator = 100;

std::size_t round_capacity(std::size_t n) noexcept {
    if (n <= kMinCapacity) return kMinCapacity;
    return std::bit_ceil(n);
}

constexpr Chunk kEmptySlot{};

} // namespace

ChunkTable::ChunkTable(std::size_t initial_capacity) {
    const std::size_t cap = round_capacity(initial_capacity);
    slots_.assign(cap, kEmptySlot);
    mask_  = cap - 1;
    shift_ = 64u - static_cast<unsigned>(std::countr_zero(cap));
}

std::size_t ChunkTable::ideal_slot(std::uint64_t key) const noexcept {
    return static_cast<std::size_t>((key * kFibonacci) >> shift_);
}

std::size_t ChunkTable::probe_distance(std::size_t slot) const noexcept {
    return (slot - ideal_slot(slots_[slot].key)) & mask_;
}

double ChunkTable::load_factor() const noexcept {
    if (slots_.empty()) return 0.0;
    return static_cast<double>(size_) / static_cast<double>(slots_.size());
}

void ChunkTable::clear() noexcept {
    for (Chunk &c : slots_) c = kEmptySlot;
    size_       = 0;
    freed_head_ = 0;
    freed_tail_ = 0;
}

bool ChunkTable::reserve(std::size_t entries) {
    /* Undo the load factor: to hold `entries` without rehashing, the table has
     * to be big enough that `entries` is still under 85% of it. */
    const std::size_t needed =
        (entries * kLoadDenominator) / kLoadNumerator + 1;
    if (needed <= slots_.size()) return true;
    return grow_to(round_capacity(needed));
}

void ChunkTable::set_max_entries(std::size_t n) {
    max_entries_ = n;

    if (n == 0) {
        freed_fifo_.clear();
        freed_head_ = freed_tail_ = 0;
        return;
    }

    /* One spare slot so a full ring is distinguishable from an empty one. */
    freed_fifo_.assign(n + 1, 0);
    freed_head_ = freed_tail_ = 0;
    refill_freed_queue();

    /* Lowering the cap under an already-larger table takes effect now rather
     * than on the next insert, so `size() <= max_entries()` is an invariant a
     * caller can rely on instead of a promise about the future. */
    while (size_ > max_entries_ && make_room()) {}
}

Chunk *ChunkTable::find(std::uint64_t key) noexcept {
    return const_cast<Chunk *>(
        static_cast<const ChunkTable *>(this)->find(key));
}

const Chunk *ChunkTable::find(std::uint64_t key) const noexcept {
    if (key == 0 || slots_.empty()) return nullptr;

    std::size_t slot = ideal_slot(key);
    std::size_t dist = 0;
    for (;;) {
        const Chunk &cur = slots_[slot];
        if (cur.state == kChunkEmpty) return nullptr;
        if (cur.key == key) return &cur;

        /* The Robin Hood invariant doing real work: every record is at least as
         * far from home as the one before it in a probe run. Once we are
         * further out than the incumbent, our key cannot be deeper in the run
         * either -- it would have stolen this slot on the way past. Without
         * this the lookup has to walk to the next empty slot instead.
         *
         * No test proves this line, and the benchmark says why: with Fibonacci
         * hashing the worst probe is 1-3 slots, so walking to the empty slot
         * costs the same 26 ns a miss already costs and the mutation that
         * deletes this is invisible. It earns its place when the load factor
         * climbs or the key distribution degrades, which is exactly when
         * nobody will be watching a benchmark. Kept deliberately, and the test
         * suite says plainly that it does not cover it. */
        if (dist > probe_distance(slot)) return nullptr;

        slot = (slot + 1) & mask_;
        ++dist;
    }
}

void ChunkTable::place(Chunk rec) {
    std::size_t slot = ideal_slot(rec.key);
    std::size_t dist = 0;

    for (;;) {
        Chunk &cur = slots_[slot];
        if (cur.state == kChunkEmpty) {
            cur = rec;
            if (dist > stats_.max_probe) stats_.max_probe = dist;
            return;
        }

        /* Take the slot from anything that is closer to its ideal than we are,
         * and carry the evicted record onward. This is what keeps the worst
         * probe short: displacement is spread across records instead of
         * accumulating on whichever key was unlucky. */
        const std::size_t cur_dist = probe_distance(slot);
        if (cur_dist < dist) {
            std::swap(cur, rec);
            dist = cur_dist;
        }

        slot = (slot + 1) & mask_;
        ++dist;
        if (dist > stats_.max_probe) stats_.max_probe = dist;
    }
}

bool ChunkTable::grow_to(std::size_t new_capacity) {
    const std::size_t cap = round_capacity(new_capacity);
    if (cap <= slots_.size()) return true;

    std::vector<Chunk> old;
    old.swap(slots_);

    slots_.assign(cap, kEmptySlot);
    mask_  = cap - 1;
    shift_ = 64u - static_cast<unsigned>(std::countr_zero(cap));

    /* Every key's ideal slot changed with the shift, so this is a full
     * reinsertion, not a copy. */
    for (const Chunk &c : old) {
        if (c.state != kChunkEmpty) place(c);
    }
    ++stats_.rehashes;
    return true;
}

void ChunkTable::push_freed(std::uint64_t key) {
    if (freed_fifo_.empty()) return;
    freed_fifo_[freed_tail_] = key;
    freed_tail_ = (freed_tail_ + 1) % freed_fifo_.size();
    if (freed_tail_ == freed_head_) {
        /* Ring full: the oldest hint is overwritten. That costs eviction
         * ordering, never a record -- refill_freed_queue recovers it. */
        freed_head_ = (freed_head_ + 1) % freed_fifo_.size();
    }
}

bool ChunkTable::pop_evictable() {
    while (freed_head_ != freed_tail_) {
        const std::uint64_t key = freed_fifo_[freed_head_];
        freed_head_ = (freed_head_ + 1) % freed_fifo_.size();

        /* The queue is a hint, not a second index. A key in it may since have
         * been erased outright, or reused by the allocator and now live again;
         * both are ordinary and both mean "skip it", not "something is wrong". */
        const Chunk *c = find(key);
        if (c == nullptr || c->state != kChunkFreed) continue;

        erase(key);
        ++stats_.evictions;
        return true;
    }
    return false;
}

/* Rebuilds the eviction order from the table itself, oldest free first.
 *
 * O(capacity) plus a sort, which is why it is a fallback rather than the
 * mechanism: it runs when the queue was never populated (a cap set after the
 * frees already happened) or has been overrun by stale hints, not on the steady
 * path. Doing it here rather than reporting "nothing to evict" is what makes
 * the queue an optimisation instead of a source of truth that can be wrong. */
void ChunkTable::refill_freed_queue() {
    if (freed_fifo_.empty()) return;

    std::vector<std::pair<std::uint32_t, std::uint64_t>> freed;
    for (const Chunk &c : slots_) {
        if (c.state == kChunkFreed) freed.emplace_back(c.free_ms, c.key);
    }
    std::sort(freed.begin(), freed.end());

    freed_head_ = freed_tail_ = 0;
    const std::size_t room = freed_fifo_.size() - 1;
    const std::size_t n = freed.size() < room ? freed.size() : room;
    for (std::size_t i = 0; i < n; ++i) push_freed(freed[i].second);
}

/* Evicts the oldest freed record. Returns false only when there is genuinely
 * nothing left that can be given up without losing live data. */
bool ChunkTable::make_room() {
    if (pop_evictable()) return true;
    refill_freed_queue();
    return pop_evictable();
}

bool ChunkTable::insert_live(std::uint64_t key, std::uint32_t size,
                             std::uint32_t usable, std::uint32_t alloc_ms,
                             std::uint32_t tid) {
    if (key == 0) return false;

    /* An address the allocator has recycled. Overwriting in place keeps the
     * probe layout untouched, which matters because recycling is the common
     * case in the churn this tool watches, not an edge case. */
    if (Chunk *existing = find(key)) {
        const bool was_freed = existing->state == kChunkFreed;
        existing->size     = size;
        existing->usable   = usable;
        existing->alloc_ms = alloc_ms;
        existing->free_ms  = 0;
        existing->tid      = tid;
        existing->state    = kChunkLive;
        ++stats_.updates;
        /* Its stale entry stays in the freed queue; make_room skips it now that
         * the record is live again. */
        (void)was_freed;
        return true;
    }

    if (max_entries_ != 0 && size_ >= max_entries_ && !make_room()) {
        ++stats_.drops;
        return false;
    }

    if ((size_ + 1) * kLoadDenominator > slots_.size() * kLoadNumerator) {
        if (!grow_to(slots_.size() * 2)) {
            ++stats_.drops;
            return false;
        }
    }

    Chunk rec{};
    rec.key      = key;
    rec.size     = size;
    rec.usable   = usable;
    rec.alloc_ms = alloc_ms;
    rec.free_ms  = 0;
    rec.tid      = tid;
    rec.state    = kChunkLive;

    place(rec);
    ++size_;
    ++stats_.inserts;
    return true;
}

bool ChunkTable::mark_freed(std::uint64_t key, std::uint32_t free_ms) noexcept {
    Chunk *c = find(key);
    if (c == nullptr) return false;

    c->state   = kChunkFreed;
    c->free_ms = free_ms;

    push_freed(key);
    return true;
}

bool ChunkTable::erase(std::uint64_t key) noexcept {
    Chunk *c = find(key);
    if (c == nullptr) return false;

    std::size_t i = static_cast<std::size_t>(c - slots_.data());

    /* Backward shift, not a tombstone. Walk forward pulling each displaced
     * record back one slot until reaching a record already at home or an empty
     * slot; either terminates the run without leaving a hole that later probes
     * would have to walk past. */
    for (;;) {
        const std::size_t j = (i + 1) & mask_;
        if (slots_[j].state == kChunkEmpty) break;
        if (probe_distance(j) == 0) break; /* at home; moving it would lose it */
        slots_[i] = slots_[j];
        i = j;
    }

    slots_[i] = kEmptySlot;
    --size_;
    ++stats_.erases;
    return true;
}

} // namespace hv
