/* heapviz - reading ptmalloc chunk headers out of the target (M2.2).
 *
 * Copyright (C) 2026 Parth Sinha
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The interceptor already reports `malloc_usable_size`, which is exact and free,
 * so this is not how heapviz learns how big an allocation is. Two things need
 * the real chunk header, and neither is served by the event stream:
 *
 *   - **Overhead.** `usable` is what the caller may write; the chunk is bigger,
 *     by a header and by whatever rounding the allocator applied. The gap is
 *     what the overhead markers draw, and it is only visible from the header.
 *   - **Chunks nobody saw allocated.** heapviz attaches to a process that
 *     already has a heap. Everything allocated before that is invisible to the
 *     event stream and always will be. D5 goes further: after a ring overflow
 *     the model has a phantom leak in it that no amount of replaying can fix,
 *     and reading the target's actual chunks is the only ground truth there is.
 *
 * WHY READS ARE BATCHED
 * ---------------------
 * `process_vm_readv` takes vectors on both sides, so one syscall can fetch a
 * thousand headers scattered across the address space. Per-chunk reads would put
 * a syscall in a loop that runs over the live set, which at a hundred thousand
 * chunks is not a profiler, it is a denial of service against the thing being
 * profiled.
 *
 * WHY IT IS ALLOWED TO FAIL
 * -------------------------
 * `process_vm_readv` needs the same uid or `CAP_SYS_PTRACE`, and
 * `/proc/sys/kernel/yama/ptrace_scope = 1` blocks it for a non-descendant even
 * then. That is a normal configuration on a normal machine, so `EPERM` is not an
 * error path, it is a supported mode: heapviz says overhead is unavailable and
 * carries on with interceptor data. Refusing to start would be trading the
 * ninety percent of the tool that works for the ten that does not.
 *
 * And it never calls `PTRACE_ATTACH`. That stops the target. A profiler whose
 * price is stopping the process it is measuring has changed the thing it is
 * measuring.
 */

#ifndef HEAPVIZ_TUI_CHUNK_READER_H
#define HEAPVIZ_TUI_CHUNK_READER_H

#include <cstddef>
#include <cstdint>
#include <vector>

#include <sys/types.h>
#include <sys/uio.h>

namespace hv {

/* glibc keeps three flags in the low bits of the chunk size, which is free
 * because every chunk is at least 16-byte aligned. */
constexpr std::uint64_t kChunkPrevInUse    = 1;
constexpr std::uint64_t kChunkIsMmapped    = 2;
constexpr std::uint64_t kChunkNonMainArena = 4;
constexpr std::uint64_t kChunkSizeMask     = ~std::uint64_t{7};

/* malloc(0) still returns a chunk, and the smallest glibc will hand out is 32
 * bytes on 64-bit. Anything smaller is not a chunk, it is a misread. */
constexpr std::uint64_t kMinChunkBytes = 32;
constexpr std::uint64_t kChunkAlign    = 16;

/* The two words before the user pointer: `prev_size` then `size`. The user
 * pointer is therefore `chunk + 2 * sizeof(size_t)`, and that is the offset to
 * read a header back from. */
constexpr std::uint64_t kChunkHeaderBytes = 2 * sizeof(std::uint64_t);

/* But an in-use chunk only *costs* one word, not two.
 *
 * glibc lets the user data of an in-use chunk run into the following chunk's
 * `prev_size` field, because that field is only meaningful while the preceding
 * chunk is free. So a request of 24 bytes fits in a 32-byte chunk, not the
 * 40-byte one a two-word header would imply. Using the read offset as the size
 * floor rejected every chunk whose rounding landed in the wrong half -- almost
 * exactly half of a run of varied sizes, which looks far more like a decoder
 * bug than like the arithmetic it actually was. */
constexpr std::uint64_t kChunkMinOverheadBytes = sizeof(std::uint64_t);

/* An implausible size is far more likely to be a torn read than a real chunk of
 * that size, so decoding refuses anything above this outright. 64 GiB is beyond
 * any single ptmalloc chunk that is not already mmapped. */
constexpr std::uint64_t kMaxChunkBytes = std::uint64_t{64} << 30;

/* Exactly as it sits in the target. */
struct RawChunkHeader {
    std::uint64_t prev_size = 0;
    std::uint64_t size      = 0;
};

struct ChunkInfo {
    std::uint64_t user_ptr   = 0;
    std::uint64_t chunk_size = 0; /* the real allocation, header included */
    std::uint32_t overhead   = 0; /* chunk_size - requested                */
    bool mmapped        = false;
    bool non_main_arena = false;
    bool prev_in_use    = false;
    bool valid          = false;  /* false when the header did not survive
                                     the sanity checks below                */
};

enum class ReadStatus : std::uint8_t {
    Ok,
    Denied,      /* EPERM/EACCES: no ptrace rights, or yama says no  */
    TargetGone,  /* ESRCH: the process exited                        */
    Partial,     /* some vectors came back short; what arrived is ok  */
    Failed,
};

const char *read_status_str(ReadStatus s) noexcept;

/* Turns a raw header into a decoded chunk, rejecting anything implausible.
 *
 * Pure, so the whole decision table is testable without a target process --
 * which matters more here than usual, because the inputs that need rejecting
 * are exactly the ones that are hard to produce on demand. A header read while
 * the allocator was midway through updating it is garbage of no particular
 * shape, and garbage reaching the renderer is drawn as confidently as truth.
 *
 * `requested` is what the caller asked for, used only to compute overhead; pass
 * zero when it is unknown. `region_span` bounds the plausible size and may be
 * zero to skip that check. */
bool decode_chunk(const RawChunkHeader &raw, std::uint64_t user_ptr,
                  std::uint64_t requested, std::uint64_t region_span,
                  ChunkInfo &out) noexcept;

class ChunkReader {
public:
    /* Same signature as process_vm_readv. Tests inject a failure at this one
     * boundary so EFAULT, ESRCH and EPERM remain independently covered even
     * when the test runner itself is under ptrace (which makes the real syscall
     * return EPERM before it examines the pid or remote vectors). */
    using ReadFn = ssize_t (*)(pid_t, const iovec *, unsigned long,
                               const iovec *, unsigned long, unsigned long);

    explicit ChunkReader(int pid, ReadFn read_fn = nullptr) noexcept
        : pid_(pid), read_fn_(read_fn) {}

    int pid() const noexcept { return pid_; }

    /* Reads the headers for `n` user pointers in as few syscalls as the kernel's
     * iovec limit allows, decoding each into `out[i]`. `out[i].valid` is false
     * for any that did not survive decoding, which is per-chunk rather than
     * fatal: one bad header must not discard the batch.
     *
     * `requested` may be null, in which case overhead is not computed.
     *
     * Returns Denied once and then stays unavailable -- see `available`. */
    ReadStatus read(const std::uint64_t *ptrs, const std::uint64_t *requested,
                    std::size_t n, ChunkInfo *out);

    /* False once a read has come back EPERM. The permission cannot appear
     * later in a session, so retrying it every half second would be a syscall
     * per frame that is known in advance to fail. */
    bool available() const noexcept { return !denied_; }

    /* The one-line explanation for the status bar, or null when nothing is
     * wrong. Deliberately names the cause, because "unavailable" sends people
     * looking for a bug in heapviz rather than at ptrace_scope. */
    const char *hint() const noexcept;

    std::uint64_t reads()     const noexcept { return reads_; }
    std::uint64_t headers()   const noexcept { return headers_; }
    std::uint64_t rejected()  const noexcept { return rejected_; }

private:
    int  pid_;
    ReadFn read_fn_ = nullptr;
    bool denied_ = false;
    bool gone_   = false;

    /* Reused across reads so a periodic enrichment pass does not allocate. */
    std::vector<RawChunkHeader> buf_;

    std::uint64_t reads_    = 0;
    std::uint64_t headers_  = 0;
    std::uint64_t rejected_ = 0;
};

} // namespace hv

#endif /* HEAPVIZ_TUI_CHUNK_READER_H */
