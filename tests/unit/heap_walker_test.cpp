/* heapviz - the external heap walk, against a synthetic process (M2.5).
 *
 * Copyright (C) 2026 Parth Sinha
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The walk is the one part of heapviz that reads a data structure it does not
 * own, written by a program it did not build, while that program is still
 * modifying it. Every interesting input is therefore one that cannot be produced
 * on demand from a real target: a header torn mid-update, a size field of zero,
 * a chain that starts four kilobytes into the region. So the target here is a
 * buffer, reached through `HeapWalker`'s injected reader, and each test lays out
 * the exact bytes whose handling it is asserting.
 *
 * The counterpart is `tests/integration/attach_test.cpp`, which walks a real
 * process and checks the count against a program with a known live set. Neither
 * replaces the other: this one holds the decisions, that one holds the belief
 * that ptmalloc actually looks like this.
 */

#include "tui/heap_walker.h"

#include <cstdio>
#include <cstring>
#include <vector>

#include <cerrno>

namespace {

int g_failures = 0;

void check(bool ok, const char *what) {
    if (!ok) { std::fprintf(stderr, "  FAIL %s\n", what); ++g_failures; }
}

/* The synthetic target. A flat buffer that stands in for the address space,
 * based at a plausible-looking heap address so nothing can accidentally depend
 * on the region starting at zero. */
constexpr std::uint64_t kBase = 0x5555'0000'0000ull;

std::vector<std::uint8_t> g_mem;
int  g_errno_to_raise = 0;

ssize_t fake_read(pid_t, const iovec *local, unsigned long liovcnt,
                  const iovec *remote, unsigned long, unsigned long) {
    if (g_errno_to_raise != 0) { errno = g_errno_to_raise; return -1; }
    if (liovcnt != 1) { errno = EINVAL; return -1; }

    const auto addr = reinterpret_cast<std::uint64_t>(remote[0].iov_base);
    if (addr < kBase) { errno = EFAULT; return -1; }
    const std::uint64_t off = addr - kBase;
    if (off >= g_mem.size()) { errno = EFAULT; return -1; }

    std::size_t n = remote[0].iov_len;
    if (off + n > g_mem.size()) n = static_cast<std::size_t>(g_mem.size() - off);
    std::memcpy(local[0].iov_base, g_mem.data() + off, n);
    return static_cast<ssize_t>(n);
}

hv::Region region_of(std::uint64_t bytes) {
    hv::Region r;
    r.start = kBase;
    r.end   = kBase + bytes;
    r.kind  = hv::RegionKind::Heap;
    r.perms = hv::kPermRead | hv::kPermWrite | hv::kPermPrivate;
    return r;
}

/* Writes one chunk header at `off`. `size` is the whole chunk including the
 * header; `prev_in_use` is the flag that speaks for the *previous* chunk. */
void put_header(std::size_t off, std::uint64_t size, bool prev_in_use) {
    const std::uint64_t prev_size = 0;
    const std::uint64_t field = size | (prev_in_use ? 1u : 0u);
    std::memcpy(g_mem.data() + off, &prev_size, sizeof prev_size);
    std::memcpy(g_mem.data() + off + 8, &field, sizeof field);
}

/* Lays out `n` chunks of `size` bytes each from `at`, all in use, followed by a
 * top chunk covering the rest. Returns the offset just past the last one. */
std::size_t lay_chain(std::size_t at, int n, std::uint64_t size) {
    std::size_t off = at;
    for (int i = 0; i < n; ++i) {
        put_header(off, size, true);
        off += static_cast<std::size_t>(size);
    }
    /* The top chunk: in use as far as its predecessor is concerned, and never
     * itself emitted because nothing follows it to vouch for it. */
    if (off + 16 <= g_mem.size())
        put_header(off, g_mem.size() - off, true);
    return off;
}

hv::WalkResult run(const hv::Region &r, std::vector<hv::WalkedChunk> &out) {
    hv::HeapWalker w(1234, fake_read);
    std::vector<hv::Region> regions{r};
    return w.walk(regions, out);
}

/* --- the decisions ---------------------------------------------------------*/

void test_a_plain_chain_is_walked() {
    g_mem.assign(64 * 1024, 0);
    lay_chain(0, 10, 64);

    std::vector<hv::WalkedChunk> out;
    const hv::WalkResult r = run(region_of(g_mem.size()), out);

    check(r.status == hv::WalkStatus::Ok, "plain: the walk succeeds");
    check(r.chunks == 10, "plain: every chunk is found");
    check(out.size() == 10, "plain: and every one is emitted");
    if (out.size() == 10) {
        check(out[0].user_ptr == kBase + 16,
              "plain: the user pointer is past both header words");
        check(out[0].chunk_size == 64, "plain: the chunk size is the whole chunk");
        check(out[0].usable == 64 - 8,
              "plain: usable borrows the successor's prev_size word");
        check(out[9].user_ptr == kBase + 9 * 64 + 16,
              "plain: the last one lands where the chain says");
    }
}

void test_the_top_chunk_is_not_an_allocation() {
    g_mem.assign(64 * 1024, 0);
    lay_chain(0, 3, 64);

    std::vector<hv::WalkedChunk> out;
    const hv::WalkResult r = run(region_of(g_mem.size()), out);

    /* Four headers were written, three of them allocations. The top chunk is
     * free space; counting it would report the whole unused arena as live, which
     * on a freshly grown heap is most of it. */
    check(r.chunks == 3, "top: the trailing chunk is not counted");
}

void test_a_free_chunk_is_not_emitted() {
    g_mem.assign(64 * 1024, 0);
    /* Three chunks where the middle one is free: it is the *third* header's
     * PREV_INUSE that says so, which is the indirection worth pinning. */
    put_header(0, 64, true);
    put_header(64, 64, true);        /* speaks for chunk 0: in use */
    put_header(128, 64, false);      /* speaks for chunk 1: free   */
    put_header(192, 64, true);       /* speaks for chunk 2: in use */
    put_header(256, g_mem.size() - 256, true);

    std::vector<hv::WalkedChunk> out;
    const hv::WalkResult r = run(region_of(g_mem.size()), out);

    check(r.chunks == 3, "free: the free chunk is left out of the live set");
    check(r.free_chunks == 1, "free: and counted as a hole instead");
    for (const hv::WalkedChunk &c : out)
        check(c.user_ptr != kBase + 64 + 16,
              "free: the freed address is not among the live chunks");
}

/* A header torn mid-update, partway along a chain that is otherwise sound.
 *
 * The chain has to be long enough to be *found* before the tear, or this asserts
 * on the opaque path instead and passes for the wrong reason -- which is exactly
 * what the first version of this test did, and it only came to light when the
 * sanity check was deliberately deleted and the test stayed green. Ten chunks
 * ahead of the tear clears `kMinChainProbe` with room to spare. */
void test_a_torn_header_ends_the_chain() {
    g_mem.assign(64 * 1024, 0);
    lay_chain(0, 20, 64);

    /* Aligned and above the minimum, so only the bounds check rejects it: a size
     * that would step outside the region. This is the shape a half-written size
     * field actually takes, and the one a walk could most plausibly follow. */
    put_header(10 * 64, 0x7fff'0000ull, true);

    std::vector<hv::WalkedChunk> out;
    const hv::WalkResult r = run(region_of(g_mem.size()), out);

    check(r.status == hv::WalkStatus::Ok, "torn: a torn chain is not an error");
    check(r.chunks >= 8, "torn: everything before the tear is still reported");
    check(r.chunks < 20, "torn: the walk stops rather than inventing chunks");
    check(r.opaque_bytes > 0,
          "torn: and what it could not account for is reported");

    /* Nothing may be emitted from beyond the tear: past that point the walk has
     * no idea where a chunk begins, and a plausible-looking address is worse
     * than a missing one. */
    for (const hv::WalkedChunk &c : out)
        check(c.user_ptr < kBase + 10 * 64 + 16,
              "torn: no chunk is invented past the torn header");
}

/* The same, but torn to a size the loop could not advance past. A size field
 * that decodes to zero would leave the cursor where it is, and a walk that did
 * not reject it would spin without ever returning -- which from the outside is
 * indistinguishable from heapviz having hung. */
void test_a_zero_size_does_not_stall_the_walk() {
    g_mem.assign(64 * 1024, 0);
    lay_chain(0, 20, 64);
    put_header(10 * 64, 0, true);

    std::vector<hv::WalkedChunk> out;
    const hv::WalkResult r = run(region_of(g_mem.size()), out);

    check(r.status == hv::WalkStatus::Ok, "zero: the walk returns at all");
    check(r.chunks >= 8 && r.chunks < 20,
          "zero: stopping at the bad header, having kept what came before");
}

void test_a_chain_that_starts_late_is_found() {
    g_mem.assign(64 * 1024, 0);
    /* A thread arena: glibc's heap_info and malloc_state occupy the front, so
     * offset zero is a structure rather than a chunk. The bytes before the chain
     * are left as a plausible-looking mixture rather than zeros, because zeros
     * would be rejected by the size checks for the wrong reason. */
    constexpr std::size_t kPrefix = 2192;
    for (std::size_t i = 0; i < kPrefix; i += 8) {
        const std::uint64_t junk = 0x7fff'0000'1234ull + i;
        std::memcpy(g_mem.data() + i, &junk, sizeof junk);
    }
    lay_chain(kPrefix, 12, 96);

    std::vector<hv::WalkedChunk> out;
    const hv::WalkResult r = run(region_of(g_mem.size()), out);

    check(r.chunks == 12, "arena: the chain is found past the arena header");
    if (!out.empty())
        check(out[0].user_ptr == kBase + kPrefix + 16,
              "arena: and the first chunk is where it was laid, not at 0");
}

void test_a_region_with_no_chain_is_opaque() {
    g_mem.assign(64 * 1024, 0);
    /* A runtime's own mmap'd heap: real memory, no ptmalloc headers. This must
     * be reported as unreadable-but-present rather than as an error or as an
     * empty heap, because it is usually most of the process. */
    for (std::size_t i = 0; i < g_mem.size(); i += 8) {
        const std::uint64_t junk = 0x0102'0304'0506'0700ull + i;
        std::memcpy(g_mem.data() + i, &junk, sizeof junk);
    }

    std::vector<hv::WalkedChunk> out;
    const hv::WalkResult r = run(region_of(g_mem.size()), out);

    check(r.status == hv::WalkStatus::Ok, "opaque: not an error");
    check(r.chunks == 0, "opaque: nothing is claimed to be live");
    check(r.opaque_bytes == g_mem.size(),
          "opaque: the whole region is reported as unaccounted for");
}

void test_a_chunk_across_a_block_boundary() {
    /* Larger than one read, with the chunk size chosen so that chunks land
     * astride the boundary rather than on it. A walk that restarted each read at
     * the next block boundary instead of at the cursor would lose one chunk per
     * block, which on a large heap is a slow undercount rather than a crash. */
    const std::uint64_t size = 96;
    g_mem.assign(hv::kWalkBlockBytes * 2 + 4096, 0);
    const int n = static_cast<int>((g_mem.size() - 4096) / size);
    lay_chain(0, n, size);

    std::vector<hv::WalkedChunk> out;
    const hv::WalkResult r = run(region_of(g_mem.size()), out);

    check(r.chunks == static_cast<std::uint64_t>(n),
          "boundary: no chunk is lost where the reads meet");
    check(r.status == hv::WalkStatus::Ok, "boundary: and the walk completes");
}

void test_permission_is_named_not_guessed() {
    g_mem.assign(64 * 1024, 0);
    lay_chain(0, 4, 64);

    g_errno_to_raise = EPERM;
    std::vector<hv::WalkedChunk> out;
    hv::HeapWalker w(1234, fake_read);
    std::vector<hv::Region> regions{region_of(g_mem.size())};
    const hv::WalkResult r = w.walk(regions, out);
    g_errno_to_raise = 0;

    check(r.status == hv::WalkStatus::Denied, "perm: EPERM is reported as denied");
    check(!w.available(), "perm: and latches, so it is not retried every frame");
    check(w.hint() != nullptr && std::strstr(w.hint(), "ptrace_scope") != nullptr,
          "perm: the hint names ptrace_scope rather than saying 'unavailable'");
}

void test_a_target_that_exits_mid_walk() {
    g_mem.assign(64 * 1024, 0);
    lay_chain(0, 4, 64);

    g_errno_to_raise = ESRCH;
    std::vector<hv::WalkedChunk> out;
    hv::HeapWalker w(1234, fake_read);
    std::vector<hv::Region> regions{region_of(g_mem.size())};
    const hv::WalkResult r = w.walk(regions, out);
    g_errno_to_raise = 0;

    check(r.status == hv::WalkStatus::TargetGone,
          "gone: ESRCH is death, not a read failure");
}

void test_a_reservation_is_not_read() {
    g_mem.assign(64 * 1024, 0);
    lay_chain(0, 4, 64);

    /* glibc maps 64 MiB PROT_NONE per thread arena and commits a prefix. Reading
     * an unreadable region costs a syscall per block to learn what the
     * permission bits already said. */
    hv::Region r = region_of(g_mem.size());
    r.perms = hv::kPermPrivate; /* no read bit */

    std::vector<hv::WalkedChunk> out;
    hv::HeapWalker w(1234, fake_read);
    std::vector<hv::Region> regions{r};
    const hv::WalkResult res = w.walk(regions, out);

    check(res.chunks == 0, "perms: an unreadable region yields nothing");
    check(w.reads() == 0, "perms: and is not read at all");
}

} // namespace

int main() {
    test_a_plain_chain_is_walked();
    test_the_top_chunk_is_not_an_allocation();
    test_a_free_chunk_is_not_emitted();
    test_a_torn_header_ends_the_chain();
    test_a_zero_size_does_not_stall_the_walk();
    test_a_chain_that_starts_late_is_found();
    test_a_region_with_no_chain_is_opaque();
    test_a_chunk_across_a_block_boundary();
    test_permission_is_named_not_guessed();
    test_a_target_that_exits_mid_walk();
    test_a_reservation_is_not_read();

    if (g_failures != 0) {
        std::fprintf(stderr, "heap_walker: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("heap_walker: all checks passed\n");
    return 0;
}
