/* heapviz - reading a heap that was never instrumented (M2.5).
 *
 * Copyright (C) 2026 Parth Sinha
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "tui/heap_walker.h"

#include <cerrno>
#include <cstring>

namespace hv {

const char *walk_status_str(WalkStatus s) noexcept {
    switch (s) {
    case WalkStatus::Ok:         return "ok";
    case WalkStatus::Denied:     return "ptrace denied";
    case WalkStatus::TargetGone: return "target exited";
    case WalkStatus::Failed:     return "read failed";
    }
    return "unknown";
}

HeapWalker::HeapWalker(int pid, ReadFn read_fn)
    : pid_(pid), read_fn_(read_fn) {
    block_.resize(kWalkBlockBytes);
}

const char *HeapWalker::hint() const noexcept {
    if (denied_)
        return "cannot read this process: raise ptrace_scope or grant "
               "cap_sys_ptrace";
    if (gone_) return "target exited: heap frozen";
    return nullptr;
}

ssize_t HeapWalker::read_block(std::uint64_t addr, std::size_t len) {
    iovec local{block_.data(), len};
    iovec remote{reinterpret_cast<void *>(addr), len};

    ++reads_;
    const ssize_t n =
        read_fn_ != nullptr
            ? read_fn_(static_cast<pid_t>(pid_), &local, 1, &remote, 1, 0)
            : ::process_vm_readv(static_cast<pid_t>(pid_), &local, 1, &remote,
                                 1, 0);
    if (n < 0) {
        switch (errno) {
        case EPERM:
        case EACCES: denied_ = true; break;
        case ESRCH:  gone_   = true; break;
        default: break;
        }
    }
    return n;
}

namespace {

/* How many consecutive plausible headers a candidate offset must produce before
 * it is believed. Four is enough to make a coincidence unlikely -- a random
 * qword passing the size checks is already uncommon, and four in a row that also
 * chain to each other is not something noise does -- while staying reachable on
 * an arena holding only a handful of allocations. */
constexpr int         kMinChainProbe   = 4;
constexpr std::size_t kMaxProbeBytes   = 8 * 1024;

/* Follows the chain from `off` within an already-read block, returning how many
 * headers survive. Pure and buffer-local: no syscalls, so probing two hundred
 * candidate offsets costs one read rather than two hundred. */
int score_chain(const std::uint8_t *block, std::size_t avail, std::size_t off,
                std::uint64_t region_span) noexcept {
    int n = 0;
    while (off + 2 * sizeof(std::uint64_t) <= avail && n < 64) {
        std::uint64_t size_field = 0;
        std::memcpy(&size_field, block + off + sizeof(std::uint64_t),
                    sizeof size_field);
        const std::uint64_t size = size_field & ~std::uint64_t{7};
        if ((size & (kWalkAlign - 1)) != 0 ||
            size > region_span)
            break;
        off += static_cast<std::size_t>(size);
        ++n;
    }
    return n;
}

/* Where this region's chunk chain begins.
 *
 * The main arena's first chunk sits at the start of `[heap]`, so offset zero is
 * the answer there. A thread arena does not: glibc puts a `heap_info` at the
 * front of every 64 MiB-aligned mapping, and the arena's first mapping carries a
 * `malloc_state` after it -- together a few kilobytes whose size depends on the
 * glibc build. Hardcoding either sizeof would make heapviz wrong on the next
 * glibc, and silently: the walk would start mid-structure and reject the region
 * as opaque, which looks exactly like a runtime's own mmap'd heap.
 *
 * So the offset is found rather than assumed. Every 16-byte-aligned candidate in
 * the first few kilobytes is scored by how far its chain runs, and the best is
 * taken. That is self-validating -- a wrong guess scores nothing -- and it
 * costs one already-performed read. */
std::size_t find_chain_start(const std::uint8_t *block, std::size_t avail,
                             std::uint64_t region_span) noexcept {
    const std::size_t limit = avail < kMaxProbeBytes ? avail : kMaxProbeBytes;

    std::size_t best_off   = 0;
    int         best_score = 0;
    for (std::size_t off = 0; off + 2 * sizeof(std::uint64_t) <= limit;
         off += kWalkAlign) {
        const int score = score_chain(block, avail, off, region_span);
        if (score > best_score) {
            best_score = score;
            best_off   = off;
            /* A chain that runs the length of the probe is not going to be
             * beaten by a later offset, and every offset inside it would be a
             * worse alignment of the same data. */
            if (score >= 64) break;
        }
    }
    return best_score >= kMinChainProbe ? best_off : std::size_t(-1);
}

} // namespace

/* The chain, one region at a time.
 *
 * A chunk is [prev_size][size|flags][payload]. `size` includes the header and
 * is 16-aligned, so the low three bits carry flags -- of which only PREV_INUSE
 * is consulted here, and it belongs to the *next* chunk: a chunk is in use when
 * its successor says the previous one is. That indirection is why the loop
 * carries `prev_*` and emits one chunk behind where it is reading.
 *
 * The last chunk in an arena is the top chunk, which is free space rather than
 * an allocation and is deliberately not emitted. */
bool HeapWalker::walk_region(const Region &r, std::vector<WalkedChunk> &out,
                             WalkResult &sum) {
    const std::uint64_t start = r.start;
    const std::uint64_t end   = r.end;
    if (end <= start || end - start < kWalkMinChunk) return false;

    /* One read to locate the chain, before the walk proper. Its result is
     * discarded rather than reused: the walk re-reads from the cursor, which
     * keeps the loop below with a single entry path instead of a first
     * iteration that behaves differently from the rest. */
    {
        const std::uint64_t span = end - start;
        const std::size_t probe_len =
            span < kWalkBlockBytes ? static_cast<std::size_t>(span)
                                   : kWalkBlockBytes;
        const ssize_t got = read_block(start, probe_len);
        if (got <= 0) return false;
        const std::size_t off =
            find_chain_start(block_.data(), static_cast<std::size_t>(got), span);
        if (off == std::size_t(-1)) return false; /* no chain: opaque */
        cursor_start_ = start + off;
    }

    std::uint64_t cursor = cursor_start_;
    std::size_t   blocks = 0;
    std::size_t   found  = 0;

    /* The chunk awaiting a verdict: its successor's PREV_INUSE decides it. */
    bool          have_prev = false;
    std::uint64_t prev_addr = 0;
    std::uint64_t prev_size = 0;

    while (cursor + 2 * sizeof(std::uint64_t) <= end) {
        if (++blocks > kMaxBlocksPerRegion) { sum.truncated = true; break; }

        const std::uint64_t remaining = end - cursor;
        const std::size_t want = remaining < kWalkBlockBytes
                                     ? static_cast<std::size_t>(remaining)
                                     : kWalkBlockBytes;
        const ssize_t got = read_block(cursor, want);
        if (got <= 0) {
            /* A region that cannot be read at all is not a broken chain; the
             * caller decides what to make of it. Denied and TargetGone are
             * latched by read_block and checked by the caller. */
            break;
        }
        ++blocks_;

        const auto avail = static_cast<std::size_t>(got);
        std::size_t off = 0;
        bool        block_ok = true;

        /* Both header words must be inside what was read. When they are not,
         * the next read restarts at the cursor rather than at a block boundary,
         * which is what lets a chunk straddle one. */
        while (off + 2 * sizeof(std::uint64_t) <= avail) {
            std::uint64_t size_field = 0;
            std::memcpy(&size_field,
                        block_.data() + off + sizeof(std::uint64_t),
                        sizeof size_field);

            const std::uint64_t size = size_field & ~std::uint64_t{7};
            const bool prev_in_use   = (size_field & 1) != 0;
            const std::uint64_t here = cursor + off;

            /* The verdict on the previous chunk, now that its successor has
             * been read. Free chunks are counted but not emitted: they are
             * holes, and M5.4's fragmentation figure already owns holes. */
            if (have_prev) {
                if (prev_in_use) {
                    WalkedChunk c;
                    c.user_ptr   = prev_addr + 2 * sizeof(std::uint64_t);
                    c.chunk_size = prev_size;
                    /* ptmalloc lets a chunk use its successor's prev_size word,
                     * so usable is size minus one word, not two. */
                    c.usable     = prev_size - sizeof(std::uint64_t);
                    out.push_back(c);
                    ++sum.chunks;
                    sum.bytes += c.usable;
                    if (++found >= kMaxChunksPerRegion) {
                        sum.truncated = true;
                        block_ok = false;
                        break;
                    }
                } else {
                    ++sum.free_chunks;
                    sum.free_bytes += prev_size;
                }
                have_prev = false;
            }

            /* Anything that fails these is not a chunk header, and following it
             * would resynchronise the walk onto noise. The chain ends here. */
            if ((size & (kWalkAlign - 1)) != 0 ||
                here + size > end || size > end - start) {
                block_ok = false;
                break;
            }

            prev_addr = here;
            prev_size = size;
            have_prev = true;
            off += static_cast<std::size_t>(size);
        }

        if (!block_ok) break;

        /* No progress means the block held no complete header, which for a
         * block-sized read can only happen at the very end of the region. */
        if (off == 0) break;
        cursor += off;
    }

    /* The trailing chunk is never emitted: with no successor to consult, its
     * in-use bit is unknown, and in a well-formed arena it is the top chunk --
     * free space by definition. */
    return found > 0;
}

WalkResult HeapWalker::walk(const std::vector<Region> &regions,
                            std::vector<WalkedChunk> &out) {
    WalkResult sum;
    out.clear();

    if (denied_) { sum.status = WalkStatus::Denied; return sum; }
    if (gone_)   { sum.status = WalkStatus::TargetGone; return sum; }

    for (const Region &r : regions) {
        if (!r.allocatable()) continue;
        /* A reservation with no read permission holds nothing yet: glibc maps
         * 64 MiB per thread arena PROT_NONE and commits a prefix. Reading it
         * would fail per block and cost a syscall each time. */
        if ((r.perms & kPermRead) == 0) continue;

        const bool chained = walk_region(r, out, sum);

        if (denied_) { sum.status = WalkStatus::Denied; return sum; }
        if (gone_)   { sum.status = WalkStatus::TargetGone; return sum; }

        /* Everything the chain did not account for. For a ptmalloc arena this
         * is the top chunk and rounding; for a runtime's own mmap'd heap it is
         * the whole region, which is the number the UI has to show so that
         * "3 MB live" against a 600 MB process is not read as the truth. */
        std::uint64_t accounted = 0;
        for (const WalkedChunk &c : out) {
            if (c.user_ptr >= r.start && c.user_ptr < r.end)
                accounted += c.chunk_size;
        }
        const std::uint64_t span = r.end - r.start;
        sum.opaque_bytes += chained ? (span > accounted ? span - accounted : 0)
                                    : span;
    }

    return sum;
}

} // namespace hv
