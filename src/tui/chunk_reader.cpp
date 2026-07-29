/* heapviz - reading ptmalloc chunk headers out of the target (M2.2).
 *
 * Copyright (C) 2026 Parth Sinha
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "tui/chunk_reader.h"

#include <cerrno>

#include <sys/uio.h>
#include <unistd.h>

namespace hv {

namespace {

/* The kernel caps a single process_vm_readv at UIO_MAXIOV (1024) vectors per
 * side, and returns EINVAL rather than a short read if asked for more. */
constexpr std::size_t kMaxVectors = 1024;

} // namespace

const char *read_status_str(ReadStatus s) noexcept {
    switch (s) {
    case ReadStatus::Ok:         return "ok";
    case ReadStatus::Denied:     return "ptrace denied: overhead unavailable";
    case ReadStatus::TargetGone: return "target exited";
    case ReadStatus::Partial:    return "partial read";
    case ReadStatus::Failed:     return "read failed";
    }
    return "read failed";
}

bool decode_chunk(const RawChunkHeader &raw, std::uint64_t user_ptr,
                  std::uint64_t requested, std::uint64_t region_span,
                  ChunkInfo &out) noexcept {
    const std::uint64_t size = raw.size & kChunkSizeMask;

    /* Every one of these rejects a shape a real chunk cannot have. A header
     * caught mid-update, or a pointer that was freed and unmapped between the
     * table being walked and the read landing, produces whatever happened to be
     * at that address -- and it is drawn exactly as confidently as a real
     * chunk unless it is refused here. */
    if (size < kMinChunkBytes) return false;
    if ((size & (kChunkAlign - 1)) != 0) return false;
    if (size > kMaxChunkBytes) return false;
    if (region_span != 0 && size > region_span) return false;

    /* The chunk has to hold what the caller asked for plus the one word an
     * in-use chunk actually costs -- see kChunkMinOverheadBytes; it is not the
     * two words the header is read back from. A chunk smaller than the
     * allocation it supposedly contains is the clearest possible signal that
     * this is not that chunk's header. */
    if (requested != 0 && size < requested + kChunkMinOverheadBytes) return false;

    out.user_ptr       = user_ptr;
    out.chunk_size     = size;
    out.mmapped        = (raw.size & kChunkIsMmapped) != 0;
    out.non_main_arena = (raw.size & kChunkNonMainArena) != 0;
    out.prev_in_use    = (raw.size & kChunkPrevInUse) != 0;

    /* What the target spent minus what the caller asked for: the header, the
     * alignment rounding, and the size-class rounding, together. Zero when the
     * request is unknown rather than a guess derived from the size alone. */
    if (requested != 0 && size >= requested) {
        const std::uint64_t over = size - requested;
        out.overhead = over > UINT32_MAX ? UINT32_MAX
                                         : static_cast<std::uint32_t>(over);
    } else {
        out.overhead = 0;
    }

    out.valid = true;
    return true;
}

const char *ChunkReader::hint() const noexcept {
    if (denied_) return "ptrace denied: overhead unavailable";
    if (gone_)   return "target exited: chunk detail frozen";
    return nullptr;
}

ReadStatus ChunkReader::read(const std::uint64_t *ptrs,
                             const std::uint64_t *requested, std::size_t n,
                             ChunkInfo *out) {
    if (ptrs == nullptr || out == nullptr || n == 0) return ReadStatus::Ok;
    if (denied_) return ReadStatus::Denied;
    if (gone_)   return ReadStatus::TargetGone;

    /* Zeroed, not just resized. Any header the kernel does not fill -- a batch
     * that came back EFAULT, a pointer too low to have one, the tail of a short
     * read -- is then decoded as a size of zero, which `decode_chunk` rejects
     * outright. Left at whatever the last pass wrote, those slots would decode
     * as perfectly plausible chunks belonging to addresses they have nothing to
     * do with. */
    buf_.assign(n, RawChunkHeader{});
    for (std::size_t i = 0; i < n; ++i) out[i] = ChunkInfo{};

    ReadStatus status = ReadStatus::Ok;
    std::size_t done = 0;

    while (done < n) {
        const std::size_t batch = (n - done) < kMaxVectors ? (n - done)
                                                           : kMaxVectors;

        /* One local vector and one remote vector per header. The kernel pairs
         * them up by total length rather than element-wise, but since every
         * element is the same size on both sides they correspond one to one --
         * which is what lets a single syscall gather headers from a thousand
         * unrelated addresses. */
        iovec local[kMaxVectors];
        iovec remote[kMaxVectors];
        std::size_t vectors = 0;

        for (std::size_t i = 0; i < batch; ++i) {
            const std::uint64_t p = ptrs[done + i];
            /* A user pointer below its own header is not an address we can read
             * two words before; skip rather than underflow into the kernel. */
            if (p < kChunkHeaderBytes) continue;

            local[vectors].iov_base  = &buf_[done + i];
            local[vectors].iov_len   = sizeof(RawChunkHeader);
            remote[vectors].iov_base =
                reinterpret_cast<void *>(static_cast<std::uintptr_t>(
                    p - kChunkHeaderBytes));
            remote[vectors].iov_len  = sizeof(RawChunkHeader);
            ++vectors;
        }

        if (vectors == 0) { done += batch; continue; }

        const ssize_t got = ::process_vm_readv(
            static_cast<pid_t>(pid_), local, static_cast<unsigned long>(vectors),
            remote, static_cast<unsigned long>(vectors), 0);

        ++reads_;

        if (got < 0) {
            switch (errno) {
            case EPERM:
            case EACCES:
                /* Latched: the permission cannot appear later in a session, and
                 * retrying it on a timer is a syscall per frame known in advance
                 * to fail. */
                denied_ = true;
                return ReadStatus::Denied;
            case ESRCH:
                gone_ = true;
                return ReadStatus::TargetGone;
            case EFAULT:
                /* At least one address is not mapped any more, and the kernel
                 * will not say which. Ordinary: the chunk table is walked
                 * without stopping the target, so a chunk can be freed and its
                 * arena unmapped between the walk and the read. The batch is
                 * lost, not the pass. */
                status = ReadStatus::Partial;
                done += batch;
                continue;
            default:
                return ReadStatus::Failed;
            }
        }

        if (static_cast<std::size_t>(got) <
            vectors * sizeof(RawChunkHeader)) {
            /* A short read means the tail of the batch never arrived. Decoding
             * it anyway would be decoding whatever `buf_` held from last time,
             * which is a plausible chunk header from a previous pass -- the
             * worst possible kind of wrong. */
            status = ReadStatus::Partial;
        }

        done += batch;
    }

    for (std::size_t i = 0; i < n; ++i) {
        const std::uint64_t want = requested != nullptr ? requested[i] : 0;
        if (!decode_chunk(buf_[i], ptrs[i], want, 0, out[i])) ++rejected_;
        else ++headers_;
    }

    return status;
}

} // namespace hv
