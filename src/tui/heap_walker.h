/* heapviz - reading a heap that was never instrumented (M2.5).
 *
 * Copyright (C) 2026 Parth Sinha
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * WHY THIS EXISTS
 * ---------------
 * `LD_PRELOAD` interposition binds when a process loads. A process that is
 * already running has bound its `malloc` calls straight to libc, and nothing
 * heapviz does afterwards can redirect them -- so a target that was not started
 * under the interceptor can never produce events, no matter how it is attached.
 *
 * It can still be *read*. glibc writes a header before every allocation, and the
 * headers form a chain across the whole region: chunk N's size field is the
 * offset of chunk N+1. So the live set can be recovered from outside the process
 * with `process_vm_readv` and no injection at all. That is what this does.
 *
 * WHAT IT COSTS, AND WHAT IT CANNOT SEE
 * -------------------------------------
 * A walk is a *snapshot*, not a stream. There are no allocation timestamps, so
 * nothing here can say when a chunk was allocated, how long it has lived, or
 * which chunks are new since the last pass -- everything M3.4's aging and M5.5's
 * leak diff are built on. Two walks can be compared by address, which is a
 * weaker thing than comparing by time, and the caller has to say so.
 *
 * It over-reports small frees, and the amount is not bounded. A chunk is judged
 * in use by its successor's PREV_INUSE bit, which is what the allocator itself
 * consults -- but glibc does not clear it when a small chunk goes into the
 * tcache or a fastbin, because those bins are private caches that intend to
 * hand the chunk straight back out. Such a chunk is free to the program and
 * in-use to anything reading the heap from outside, including this. There is no
 * fix that stays outside the process: the tcache lives in the target's own TLS,
 * so finding it means knowing where each thread's `tcache_perthread_struct` is,
 * which is a glibc-version-specific hunt through thread-local storage. The
 * bound is 64 chunks per size class per thread, so on a heap doing heavy small
 * allocation the live figure reads high, and on one doing large allocation it
 * is exact.
 *
 * It also sees only what ptmalloc manages. A runtime that reserves its heap with
 * `mmap` and sub-allocates inside it -- V8, Bun, the JVM, most Python
 * allocators -- is opaque here: the reservation is one region and its contents
 * have no chunk headers to follow. Reporting 132 KB of malloc traffic for a
 * process holding 622 MB is a correct answer to the question this asks, and a
 * misleading answer to the question the user had, so `WalkResult::opaque_bytes`
 * carries the difference and the UI is expected to show it.
 *
 * READING A HEAP THAT IS STILL MOVING
 * -----------------------------------
 * The target is running. The chain can be torn mid-update at any point, and a
 * torn header is garbage of no particular shape rather than an obviously wrong
 * value. Two rules keep that from reaching the display:
 *
 *   - Every header is range-checked against the region before it is followed.
 *     A size of zero, a misaligned size, or one that would step past the end of
 *     the region ends the walk for that region rather than resynchronising on
 *     whatever happens to be there.
 *   - Every walk is bounded, in blocks read and in chunks produced. A garbage
 *     size field of 32 bytes across a 1 GiB region is 33 million iterations,
 *     which is indistinguishable from a hang from the outside.
 *
 * A region that ends early is reported, not hidden: `WalkResult::truncated` is
 * what tells the caller that the number it is about to display is a floor.
 *
 * ONE SYSCALL PER BLOCK, NOT PER CHUNK
 * ------------------------------------
 * The obvious implementation reads each header where it lands, which is a
 * syscall per chunk -- 700 syscalls for the 924 KB heap this was first tested
 * against, and millions for a real one. Instead the region is read in blocks and
 * the chain is followed inside the block, so the cost is proportional to the
 * bytes of heap rather than to the number of allocations in it. Chunks that
 * straddle a block boundary are handled by restarting the next read at the
 * cursor rather than at the next block boundary.
 */

#ifndef HEAPVIZ_TUI_HEAP_WALKER_H
#define HEAPVIZ_TUI_HEAP_WALKER_H

#include "tui/chunk_reader.h"
#include "tui/proc_maps.h"

#include <cstddef>
#include <cstdint>
#include <vector>

#include <sys/types.h>
#include <sys/uio.h>

namespace hv {

/* Read granularity. Large enough that the syscall count is a rounding error on
 * any real heap, small enough that the buffer is not a notable allocation and a
 * region that turns out to be garbage is abandoned after one read. */
constexpr std::size_t kWalkBlockBytes = 256 * 1024;

/* Ceilings, per region. Both exist to bound a walk over a torn or hostile chain
 * rather than to express a real limit: a heap that genuinely holds more than
 * this is reported truncated, which is honest, where a walk that never returned
 * would look like heapviz had hung. */
constexpr std::size_t kMaxChunksPerRegion = 4u << 20;
constexpr std::size_t kMaxBlocksPerRegion = 8192; /* 2 GiB at 256 KiB blocks */

/* The smallest thing ptmalloc will hand out, and the alignment every chunk size
 * carries. A header failing either is not a chunk. */
constexpr std::uint64_t kWalkMinChunk = 32;
constexpr std::uint64_t kWalkAlign    = 16;

/* One allocation as the walk found it. There is no timestamp, and that absence
 * is the defining difference from `Chunk`: see the header comment. */
struct WalkedChunk {
    std::uint64_t user_ptr   = 0; /* what malloc would have returned */
    std::uint64_t chunk_size = 0; /* the whole chunk, header included */
    std::uint64_t usable     = 0; /* what the caller may write to     */
};

enum class WalkStatus : std::uint8_t {
    Ok,
    Denied,     /* EPERM/EACCES: ptrace_scope, or another user's process */
    TargetGone, /* ESRCH: the process exited while being walked          */
    Failed,
};

const char *walk_status_str(WalkStatus s) noexcept;

struct WalkResult {
    WalkStatus    status = WalkStatus::Ok;
    std::uint64_t chunks = 0;
    std::uint64_t bytes  = 0; /* usable bytes across `chunks`            */
    std::uint64_t free_chunks = 0;
    std::uint64_t free_bytes  = 0;

    /* Bytes in regions the walk could not make sense of: no chunk chain was
     * found, or the chain ended after covering only part of the region. This is
     * a runtime's own mmap'd heap, and it is usually most of the process. */
    std::uint64_t opaque_bytes = 0;

    /* At least one region hit a ceiling, so `chunks` is a floor. */
    bool truncated = false;
};

class HeapWalker {
public:
    /* Substituted in tests for the same reason `ChunkReader` does it: the real
     * syscall returns EPERM before it looks at its arguments when the caller is
     * itself being traced, which makes the interesting failures untestable. */
    using ReadFn = ChunkReader::ReadFn;

    explicit HeapWalker(int pid, ReadFn read_fn = nullptr);

    int pid() const noexcept { return pid_; }

    /* Walks every allocatable region in `regions`, replacing `out`.
     *
     * A region that yields no plausible chain is not an error: it is counted
     * into `opaque_bytes` and skipped, because a JS runtime's 1 GiB arena is
     * exactly that and is the common case rather than a fault. Denied and
     * TargetGone stop the pass, since neither can be fixed by trying the next
     * region. */
    WalkResult walk(const std::vector<Region> &regions,
                    std::vector<WalkedChunk> &out);

    /* False once a read has come back EPERM, for `ChunkReader::available`'s
     * reason: the permission cannot appear later in a session. */
    bool available() const noexcept { return !denied_; }

    /* Names ptrace_scope rather than saying "unavailable", because the fix is a
     * sysctl and the word "unavailable" sends people to read heapviz's source. */
    const char *hint() const noexcept;

    std::uint64_t reads()  const noexcept { return reads_; }
    std::uint64_t blocks() const noexcept { return blocks_; }

private:
    /* Walks one region. Returns false when the chain did not survive, which the
     * caller treats as "opaque" rather than as failure. */
    bool walk_region(const Region &r, std::vector<WalkedChunk> &out,
                     WalkResult &sum);

    /* One `process_vm_readv` into `block_`, returning bytes read or -1. */
    ssize_t read_block(std::uint64_t addr, std::size_t len);

    int    pid_;
    ReadFn read_fn_ = nullptr;
    bool   denied_  = false;
    bool   gone_    = false;

    std::uint64_t reads_  = 0;
    std::uint64_t blocks_ = 0;
    std::uint64_t cursor_start_ = 0; /* where the probe found the chain */

    /* Reused across passes, so a 4 Hz walk does not allocate. */
    std::vector<std::uint8_t> block_;
};

} // namespace hv

#endif /* HEAPVIZ_TUI_HEAP_WALKER_H */
