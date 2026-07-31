/* heapviz - walking a real, uninstrumented process's heap (M2.5).
 *
 * Copyright (C) 2026 Parth Sinha
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * `heap_walker_test` proves the walk handles the ptmalloc layout correctly. It
 * cannot prove that ptmalloc actually *has* that layout, because it invents its
 * own memory: every header in it was written by the test. This is the other
 * half, and the only place the belief is held -- a child process allocates a
 * known set with the real allocator, and the parent recovers it from outside
 * with no injection of any kind.
 *
 * The child is forked rather than found, and that is a requirement rather than
 * convenience: `ptrace_scope = 1` is the default on most distributions and
 * permits `process_vm_readv` only against a descendant. A test that attached to
 * an arbitrary pid would fail on every developer machine with the default
 * sysctl, which is exactly the configuration the feature has to work under for
 * a child and be honest about for anything else.
 *
 * The assertions are one-sided on purpose. The child holds *at least* what it
 * allocated -- libc, the dynamic loader and stdio have their own live chunks in
 * the same arena, and pinning an exact count would be pinning glibc's internal
 * allocation habits, which change between releases. What must hold exactly is
 * the direction: everything the test allocated is found, and nothing invented.
 */

#include "tui/heap_walker.h"
#include "tui/proc_maps.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include <sys/wait.h>
#include <unistd.h>

namespace {

int g_failures = 0;

void check(bool ok, const char *what) {
    if (!ok) { std::fprintf(stderr, "  FAIL %s\n", what); ++g_failures; }
}

/* Big enough that the set is unmistakable against glibc's own chunks, small
 * enough that every one comes from the main arena rather than being mmap'd:
 * an mmap'd chunk sits in a region of its own with no chain to follow, which is
 * a different code path and not the one under test here. */
constexpr int         kChunks    = 4000;
constexpr std::size_t kChunkSize = 512;

/* The child: allocate, tell the parent, then block until the parent is done.
 * Never returns.
 *
 * `write` through a volatile sink for the reason `examples/` does it -- `-O3`
 * deletes an alloc/write/free sequence whose contents are never read, and a
 * test whose allocations were optimised away would assert on nothing while
 * looking like it passed. */
[[noreturn]] void child_main(int ready_fd, int wait_fd) {
    static volatile unsigned char sink = 0;

    auto **ptrs = static_cast<unsigned char **>(
        std::malloc(sizeof(unsigned char *) * kChunks));
    for (int i = 0; i < kChunks; ++i) {
        ptrs[i] = static_cast<unsigned char *>(std::malloc(kChunkSize));
        std::memset(ptrs[i], i & 0xff, kChunkSize);
        sink = ptrs[i][0];
    }
    (void)sink;

    const char go = 'k';
    if (::write(ready_fd, &go, 1) != 1) ::_exit(2);

    /* Held open until the parent closes its end, so the heap under measurement
     * cannot be torn down halfway through the walk. */
    char stop = 0;
    /* The result is stored rather than cast away: `read` is
     * `warn_unused_result` and warnings are errors here. Either outcome ends
     * the child -- a byte, or the parent closing its end. */
    const ssize_t n = ::read(wait_fd, &stop, 1);
    ::_exit(n < 0 ? 1 : 0);
}

void test_a_childs_live_set_is_recovered_from_outside() {
    int ready[2], go[2];
    if (::pipe(ready) != 0 || ::pipe(go) != 0) {
        check(false, "live: pipes");
        return;
    }

    const pid_t pid = ::fork();
    if (pid < 0) { check(false, "live: fork"); return; }
    if (pid == 0) {
        ::close(ready[0]);
        ::close(go[1]);
        child_main(ready[1], go[0]);
    }

    ::close(ready[1]);
    ::close(go[0]);

    char ack = 0;
    const bool started = ::read(ready[0], &ack, 1) == 1 && ack == 'k';
    check(started, "live: the child reported its heap ready");

    if (started) {
        hv::MapsScanner scanner(pid);
        scanner.scan(0);
        check(!scanner.regions().empty(), "live: the child's maps were scanned");

        hv::HeapWalker walker(pid);
        std::vector<hv::WalkedChunk> chunks;
        const hv::WalkResult r = walker.walk(scanner.regions(), chunks);

        check(r.status == hv::WalkStatus::Ok,
              "live: a child process is readable under ptrace_scope=1");

        if (r.status == hv::WalkStatus::Ok) {
            /* The floor, not an estimate: the child allocated these and has not
             * freed them, so a walk that misses any of them has lost part of a
             * live set it was looking straight at. */
            check(r.chunks >= static_cast<std::uint64_t>(kChunks),
                  "live: every allocation the child made was found");
            check(r.bytes >= static_cast<std::uint64_t>(kChunks) * kChunkSize,
                  "live: the bytes add up to at least what was allocated");
            check(chunks.size() == r.chunks,
                  "live: the summary counts what the vector holds");

            /* An upper bound too, loose but not vacuous. A walk that
             * resynchronised onto noise would report chunks without limit, and
             * a floor-only assertion would pass while it did. glibc's own live
             * set is a few hundred chunks, so twice the test's own count is
             * generous and still nowhere near what a runaway walk produces. */
            check(r.chunks < static_cast<std::uint64_t>(kChunks) * 2,
                  "live: no chunks were invented");

            /* Cheap per byte of heap, which is the property that lets this run
             * at 4 Hz. A syscall per chunk would be 4000-odd reads. */
            check(walker.reads() < 200,
                  "live: the walk is blocks of heap, not a read per chunk");

            /* The child's `[heap]` is a few megabytes of ptmalloc and nothing
             * else, so almost all of it should have been readable. A high
             * opaque figure here would mean the chain died early and the walk
             * had quietly stopped believing in most of the heap. */
            check(r.opaque_bytes < r.bytes * 4,
                  "live: most of the heap was accounted for, not written off");
        }

        for (const hv::WalkedChunk &c : chunks) {
            if (c.user_ptr == 0 || c.usable == 0 || c.chunk_size == 0) {
                check(false, "live: no degenerate chunk was emitted");
                break;
            }
        }
    }

    ::close(go[1]); /* releases the child */
    ::close(ready[0]);
    int st = 0;
    ::waitpid(pid, &st, 0);
}

/* A pid that is gone must be named as gone, not reported as an empty heap. An
 * exited target and a target holding nothing look identical in the chunk count,
 * and only one of them means the display should stop updating. */
void test_a_dead_pid_is_named_rather_than_read_as_empty() {
    const pid_t pid = ::fork();
    if (pid < 0) { check(false, "dead: fork"); return; }
    if (pid == 0) ::_exit(0);

    int st = 0;
    ::waitpid(pid, &st, 0); /* reaped, so the pid is truly gone */

    hv::Region r{};
    r.start = 0x1000;
    r.end   = 0x2000;
    r.perms = hv::kPermRead | hv::kPermWrite;
    /* `Region::kind` defaults to `Other`, which `allocatable()` rejects -- so a
     * region left at its default is skipped and the walk returns Ok having done
     * nothing at all. Which is how this test first passed while asserting
     * nothing. */
    r.kind  = hv::RegionKind::Heap;

    hv::HeapWalker walker(static_cast<int>(pid));
    std::vector<hv::WalkedChunk> chunks;
    const hv::WalkResult res = walker.walk({r}, chunks);

    check(res.status != hv::WalkStatus::Ok,
          "dead: walking an exited pid is not reported as a successful walk");
    check(chunks.empty(), "dead: nothing was produced for a dead process");
}

} // namespace

int main() {
    test_a_childs_live_set_is_recovered_from_outside();
    test_a_dead_pid_is_named_rather_than_read_as_empty();

    if (g_failures != 0) {
        std::fprintf(stderr, "heap_walk_live: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("heap_walk_live: all checks passed\n");
    return 0;
}
