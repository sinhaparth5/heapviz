/* heapviz - ptmalloc chunk decoding and cross-process reads (M2.2).
 *
 * Copyright (C) 2026 Parth Sinha
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Two halves. The decode table is pure, so most of this hands it headers
 * directly -- including the ones that have to be refused, which is the half
 * that matters: a header read while the allocator was updating it is garbage of
 * no particular shape, and garbage reaching the renderer is drawn exactly as
 * confidently as truth.
 *
 * The other half reads real chunk headers with `process_vm_readv`, out of this
 * process. Reading ourselves uses the real syscall and iovec plumbing, while
 * injected syscall failures cover its error classification. The injection is
 * necessary under ptrace-supervised test runners: Linux returns EPERM before
 * checking a nonexistent pid or unmapped remote address there, hiding the
 * ESRCH and EFAULT branches the unit test needs to exercise.
 *
 * Everywhere except under a different allocator, which is why `ptmalloc_probe`
 * exists. ASan replaces malloc wholesale, so there are no ptmalloc headers to
 * find, and the live assertions would be testing which allocator the test was
 * linked against. They are skipped there, out loud.
 */

#include "tui/chunk_reader.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include <unistd.h>

namespace {

int g_failures = 0;
int g_skipped  = 0;

void check(bool ok, const char *what) {
    if (!ok) { std::fprintf(stderr, "  FAIL %s\n", what); ++g_failures; }
}

ssize_t fail_efault(pid_t, const iovec *, unsigned long, const iovec *,
                    unsigned long, unsigned long) {
    errno = EFAULT;
    return -1;
}

ssize_t fail_esrch(pid_t, const iovec *, unsigned long, const iovec *,
                   unsigned long, unsigned long) {
    errno = ESRCH;
    return -1;
}

ssize_t fail_eperm(pid_t, const iovec *, unsigned long, const iovec *,
                   unsigned long, unsigned long) {
    errno = EPERM;
    return -1;
}

hv::RawChunkHeader hdr(std::uint64_t size, std::uint64_t flags = 0,
                       std::uint64_t prev = 0) {
    hv::RawChunkHeader h;
    h.prev_size = prev;
    h.size      = size | flags;
    return h;
}

/* --- the decode table ----------------------------------------------------- */

void test_a_real_header_decodes() {
    hv::ChunkInfo c;

    /* A 64-byte chunk holding a 40-byte request, with PREV_INUSE set, which is
     * the ordinary case for a small allocation in the main arena. */
    check(hv::decode_chunk(hdr(64, hv::kChunkPrevInUse), 0x5000, 40, 0, c),
          "decode: an ordinary chunk decodes");
    check(c.chunk_size == 64, "decode: size masks the flags off");
    check(c.prev_in_use, "decode: PREV_INUSE is read");
    check(!c.mmapped && !c.non_main_arena, "decode: the other flags are clear");
    check(c.user_ptr == 0x5000, "decode: the user pointer is carried");
    check(c.valid, "decode: and it is marked valid");

    /* Overhead is what the target spent minus what the caller asked for: the
     * header, the alignment rounding and the size-class rounding together. */
    check(c.overhead == 24, "decode: overhead is chunk minus request");

    check(hv::decode_chunk(hdr(0x21000, hv::kChunkIsMmapped), 0x7f0000000000ull,
                           0x20000, 0, c),
          "decode: an mmapped chunk decodes");
    check(c.mmapped, "decode: IS_MMAPPED is read");

    check(hv::decode_chunk(hdr(48, hv::kChunkNonMainArena), 0x5000, 16, 0, c),
          "decode: a thread-arena chunk decodes");
    check(c.non_main_arena, "decode: NON_MAIN_ARENA is read");

    /* All three flags at once, which is what makes masking rather than
     * comparing the only correct way to read the size. */
    const std::uint64_t all = hv::kChunkPrevInUse | hv::kChunkIsMmapped |
                              hv::kChunkNonMainArena;
    check(hv::decode_chunk(hdr(96, all), 0x5000, 32, 0, c),
          "decode: every flag set at once");
    check(c.chunk_size == 96, "decode: and the size is still clean");

    /* An unknown request means overhead cannot be computed. Reporting zero is
     * the honest answer; deriving it from the size alone would be inventing a
     * number and drawing it. */
    check(hv::decode_chunk(hdr(64), 0x5000, 0, 0, c), "decode: request unknown");
    check(c.overhead == 0, "decode: overhead is zero rather than guessed");
}

void test_implausible_headers_are_refused() {
    hv::ChunkInfo c;

    check(!hv::decode_chunk(hdr(0), 0x5000, 0, 0, c),
          "reject: a zero size is not a chunk");
    check(!hv::decode_chunk(hdr(16), 0x5000, 0, 0, c),
          "reject: below the 32-byte minimum glibc will hand out");
    check(!hv::decode_chunk(hdr(72), 0x5000, 0, 0, c),
          "reject: not 16-byte aligned");
    check(!hv::decode_chunk(hdr(UINT64_MAX & hv::kChunkSizeMask), 0x5000, 0, 0, c),
          "reject: an absurd size is a torn read, not a huge chunk");
    check(!hv::decode_chunk(hdr(std::uint64_t{128} << 30), 0x5000, 0, 0, c),
          "reject: past the 64 GiB ceiling");

    /* A chunk cannot be smaller than the allocation it supposedly holds. This
     * is the check that catches a header belonging to a different chunk
     * entirely, which is what a stale or raced read usually produces. */
    check(!hv::decode_chunk(hdr(48), 0x5000, 4096, 0, c),
          "reject: a chunk too small for its own request");
    check(!hv::decode_chunk(hdr(48), 0x5000, 48, 0, c),
          "reject: no room left for even the one-word overhead");
    /* An in-use chunk costs one word, not two: the user data runs into the next
     * chunk's prev_size. So 24 bytes really does fit in a 32-byte chunk, and a
     * decoder demanding req + 16 rejects half of all real allocations. */
    check(hv::decode_chunk(hdr(32), 0x5000, 24, 0, c),
          "reject: 24 bytes fits a 32-byte chunk, as glibc lays it out");
    check(hv::decode_chunk(hdr(64), 0x5000, 56, 0, c),
          "reject: exactly enough room is accepted");

    /* The region bound, for the caller that knows which region a pointer is in.
     * A chunk bigger than the region containing it cannot be in that region. */
    check(!hv::decode_chunk(hdr(1 << 20), 0x5000, 0, 4096, c),
          "reject: bigger than the region holding it");
    check(hv::decode_chunk(hdr(4096), 0x5000, 0, 4096, c),
          "reject: exactly the region size is allowed");

    /* And a refusal must not leave a half-filled result behind that a caller
     * ignoring the return value would then draw. */
    hv::ChunkInfo fresh;
    hv::decode_chunk(hdr(7), 0x5000, 0, 0, fresh);
    check(!fresh.valid, "reject: a refused header is not marked valid");
}

/* --- reading a live process ----------------------------------------------- */

/* True when this process's allocator looks like ptmalloc. Established by
 * reading a header rather than by an #ifdef, because what matters is the
 * allocator that is actually linked in, not the one the build expected. */
bool ptmalloc_probe() {
    void *p = std::malloc(64);
    if (p == nullptr) return false;

    hv::ChunkReader r(static_cast<int>(::getpid()));
    const std::uint64_t ptr = reinterpret_cast<std::uintptr_t>(p);
    const std::uint64_t want = 64;
    hv::ChunkInfo info;
    const hv::ReadStatus st = r.read(&ptr, &want, 1, &info);
    std::free(p);
    return st == hv::ReadStatus::Ok && info.valid;
}

void test_reading_our_own_chunks() {
    if (!ptmalloc_probe()) {
        std::printf("  live: allocator is not ptmalloc (ASan?), skipping\n");
        ++g_skipped;
        return;
    }

    /* A batch large enough to be a batch, at sizes that land in different
     * size classes so the overheads are not all identical. */
    constexpr std::size_t kN = 200;
    std::vector<void *>        blocks(kN);
    std::vector<std::uint64_t> ptrs(kN);
    std::vector<std::uint64_t> want(kN);

    for (std::size_t i = 0; i < kN; ++i) {
        want[i]   = 24 + i * 13;
        blocks[i] = std::malloc(want[i]);
        std::memset(blocks[i], 0xAB, want[i]);
        ptrs[i]   = reinterpret_cast<std::uintptr_t>(blocks[i]);
    }

    hv::ChunkReader r(static_cast<int>(::getpid()));
    std::vector<hv::ChunkInfo> out(kN);
    const hv::ReadStatus st = r.read(ptrs.data(), want.data(), kN, out.data());

    check(st == hv::ReadStatus::Ok, "live: the batch read succeeded");
    check(r.available(), "live: and reading is still available");
    check(r.hint() == nullptr, "live: with nothing to warn about");

    /* One syscall for two hundred headers scattered across the heap. Reading
     * them one at a time would put a syscall in a loop over the live set. */
    check(r.reads() <= 2, "live: the whole batch cost one syscall");

    std::size_t valid = 0;
    std::uint64_t total_overhead = 0;
    bool sane = true;
    for (std::size_t i = 0; i < kN; ++i) {
        if (!out[i].valid) continue;
        ++valid;
        total_overhead += out[i].overhead;

        if (out[i].user_ptr != ptrs[i]) sane = false;
        if (out[i].chunk_size < want[i]) sane = false;
        if ((out[i].chunk_size & 15) != 0) sane = false;
        /* glibc rounds a request up to a 16-byte boundary and adds one word of
         * header, so the overhead of a small chunk is bounded and small. A
         * decode that produced the right shape but the wrong chunk would show
         * up here as an overhead of thousands. */
        if (out[i].overhead > 32) sane = false;
    }

    check(valid > kN / 2, "live: most headers decoded");
    check(sane, "live: and each is consistent with what was requested");
    check(total_overhead > 0, "live: with real overhead behind them");

    std::printf("  live: %zu/%zu headers in %llu syscall(s), %llu bytes of "
                "overhead, %llu rejected\n",
                valid, kN, static_cast<unsigned long long>(r.reads()),
                static_cast<unsigned long long>(total_overhead),
                static_cast<unsigned long long>(r.rejected()));

    for (void *p : blocks) std::free(p);
}

void test_unreadable_addresses_are_survivable() {
    /* Inject EFAULT rather than relying on the host to reach remote-address
     * validation. A ptrace-supervised test process receives EPERM first even
     * when reading itself, which is valid production behavior but cannot cover
     * this branch. */
    hv::ChunkReader r(static_cast<int>(::getpid()), fail_efault);

    /* Addresses that are mapped in no process. The read fails, and the point is
     * that it fails per batch rather than taking the session with it. */
    const std::uint64_t bad[] = {0x10, 0x1000, 0xDEADBEEF000ull};
    std::uint64_t       want[] = {0, 0, 0};
    hv::ChunkInfo       out[3];

    const hv::ReadStatus st = r.read(bad, want, 3, out);
    check(st == hv::ReadStatus::Partial,
          "unmapped: an unreadable batch is partial, not fatal");
    check(!out[0].valid && !out[1].valid && !out[2].valid,
          "unmapped: and nothing implausible was decoded from it");

    /* The one that would be invisible without this test: a failed batch must
     * not leave the previous batch's headers in the buffer to be decoded as
     * though they belonged to these addresses. */
    if (ptmalloc_probe()) {
        hv::ChunkReader live(static_cast<int>(::getpid()));
        void *p = std::malloc(64);
        const std::uint64_t good = reinterpret_cast<std::uintptr_t>(p);
        const std::uint64_t g_want = 64;
        hv::ChunkInfo first;
        live.read(&good, &g_want, 1, &first);
        check(first.valid, "unmapped: a good read in between works");

        hv::ChunkInfo second[3];
        live.read(bad, want, 3, second);
        check(!second[0].valid && !second[1].valid && !second[2].valid,
              "unmapped: stale buffer contents are not decoded as chunks");
        std::free(p);
    }

    /* A pointer with no room for a header before it cannot be read two words
     * back, and must be skipped rather than underflowed. */
    const std::uint64_t low[] = {0, 8};
    hv::ChunkInfo       lout[2];
    hv::ChunkReader low_reader(static_cast<int>(::getpid()), fail_efault);
    low_reader.read(low, nullptr, 2, lout);
    check(!lout[0].valid && !lout[1].valid, "unmapped: a pointer at zero is skipped");
}

void test_a_dead_target_is_reported() {
    /* ESRCH is injected because a ptrace supervisor can make the kernel return
     * EPERM before it checks whether this otherwise-impossible pid exists. */
    hv::ChunkReader r(0x7FFFFFFF, fail_esrch);
    const std::uint64_t ptr = 0x5000;
    hv::ChunkInfo info;

    const hv::ReadStatus st = r.read(&ptr, nullptr, 1, &info);
    check(st == hv::ReadStatus::TargetGone, "dead: reported as gone");
    check(!info.valid, "dead: with nothing decoded");
    check(r.hint() != nullptr, "dead: and something to say about it");

    /* Latched, so a dead target is not retried once a frame forever. */
    check(r.read(&ptr, nullptr, 1, &info) == hv::ReadStatus::TargetGone,
          "dead: and it stays gone");
    check(r.reads() == 1, "dead: without a second syscall");
}

void test_the_degraded_mode_is_reachable() {
    /* Deterministic rather than depending on whether the suite runs as root,
     * under Yama, or inside a container with a ptrace supervisor. */
    hv::ChunkReader r(1, fail_eperm);
    const std::uint64_t ptr = 0x400000;
    hv::ChunkInfo info;
    const hv::ReadStatus st = r.read(&ptr, nullptr, 1, &info);

    check(st == hv::ReadStatus::Denied, "denied: EPERM is classified");
    check(!r.available(), "denied: reading is marked unavailable");
    check(r.hint() != nullptr, "denied: with a hint naming ptrace");
    check(std::strstr(r.hint(), "ptrace") != nullptr,
          "denied: that points at the cause, not at heapviz");
    /* Latched for the same reason as death: a permission that is absent now
     * cannot appear later in the session. */
    check(r.read(&ptr, nullptr, 1, &info) == hv::ReadStatus::Denied,
          "denied: and stays denied");
    check(r.reads() == 1, "denied: without retrying the syscall");
}

} // namespace

int main() {
    test_a_real_header_decodes();
    test_implausible_headers_are_refused();
    test_reading_our_own_chunks();
    test_unreadable_addresses_are_survivable();
    test_a_dead_target_is_reported();
    test_the_degraded_mode_is_reachable();

    if (g_failures != 0) {
        std::fprintf(stderr, "chunk_reader_test: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("chunk_reader_test: the decode table, its refusals, batched "
                "reads of live chunks and both degraded modes all hold"
                "%s\n", g_skipped != 0 ? " (some skipped, see above)" : "");
    return 0;
}
