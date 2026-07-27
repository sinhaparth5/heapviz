/* heapviz - end-to-end interception test.
 *
 * Copyright (C) 2026 Parth Sinha
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Forks a target with LD_PRELOAD=libheapviz.so, attaches to its ring from this
 * process (as the real TUI will), and drains while it runs.
 *
 * This is the only test that exercises the whole M1 path at once: the dlsym
 * bootstrap, the re-entrancy guard, the hooks, the shared memory setup, and the
 * ring, across a genuine process boundary. Everything else tests one piece.
 *
 * The child must be started stopped-ish and drained live, because the segment
 * is unlinked in the interceptor's destructor: wait for the child to exit first
 * and there is nothing left to read.
 */

#include "support/ring_attach.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/wait.h>
#include <unistd.h>

namespace {
constexpr std::uint32_t kBatch = 4096;
using hvtest::sleep_ms;
using hvtest::attach;
} // namespace

int main(int argc, char **argv) {
    if (argc != 3) {
        std::fprintf(stderr, "usage: intercept_e2e <libheapviz.so> <target>\n");
        return 2;
    }
    const char *so     = argv[1];
    const char *target = argv[2];

    const pid_t pid = fork();
    if (pid < 0) { std::perror("fork"); return 1; }

    if (pid == 0) {
        setenv("LD_PRELOAD", so, 1);
        // Sized so the workload still wraps the ring dozens of times (which is
        // what exercises the lap-parity path) without dropping so much that
        // rare events, like the >128 KB mmap-path allocations, vanish.
        setenv("HEAPVIZ_CAPACITY", "65536", 1);
        // Hold the target in its constructor until we have attached, so a
        // fast program cannot finish and unlink the segment first.
        setenv("HEAPVIZ_WAIT_MS", "5000", 1);
        execl(target, target, "--mode", "mixed", "--threads", "4",
              "--seconds", "2", "--rate", "20000", nullptr);
        std::perror("execl");
        _exit(127);
    }

    std::uint64_t bytes = 0;
    HvRingHeader *ring = attach(pid, bytes, 5000);
    if (ring == nullptr) {
        std::fprintf(stderr, "could not attach to target ring\n");
        kill(pid, SIGKILL);
        waitpid(pid, nullptr, 0);
        return 1;
    }

    std::printf("attached: pid=%d comm=%s capacity=%llu\n", ring->pid,
                ring->comm, static_cast<unsigned long long>(ring->capacity));

    auto *batch = static_cast<HvEvent *>(std::calloc(kBatch, sizeof(HvEvent)));
    if (batch == nullptr) { std::perror("calloc"); return 1; }

    std::uint64_t counts[HV_OP__COUNT] = {};
    std::uint64_t drained = 0;
    std::uint64_t bad_op = 0, zero_ptr = 0, zero_ts = 0;
    bool saw_large = false;   // an mmap-path allocation
    int status = 0;

    // Drain until the child exits AND the producer flag is set. Draining only
    // while the child lives would race the final events.
    for (;;) {
        const std::uint32_t n = hv_ring_drain(ring, batch, kBatch);
        for (std::uint32_t i = 0; i < n; ++i) {
            const HvEvent &e = batch[i];
            if (e.op >= HV_OP__COUNT) { ++bad_op; continue; }
            counts[e.op]++;
            if (e.ptr == 0) ++zero_ptr;
            if (e.timestamp == 0) ++zero_ts;
            if (e.size >= 128 * 1024) saw_large = true;
        }
        drained += n;

        if (n == 0) {
            if (ring->producer_exited.load(std::memory_order_acquire) != 0)
                break;
            const pid_t r = waitpid(pid, &status, WNOHANG);
            if (r == pid) {
                // Child gone; one more drain pass to catch stragglers.
                for (int i = 0; i < 4; ++i) {
                    const std::uint32_t m = hv_ring_drain(ring, batch, kBatch);
                    for (std::uint32_t k = 0; k < m; ++k)
                        if (batch[k].op < HV_OP__COUNT) counts[batch[k].op]++;
                    drained += m;
                }
                break;
            }
            sleep_ms(1);
        }
    }

    waitpid(pid, &status, 0);

    const std::uint64_t dropped = ring->dropped.load(std::memory_order_relaxed);
    const std::uint64_t total   = ring->total_events.load(std::memory_order_relaxed);

    std::printf("drained %llu events (producer pushed %llu, dropped %llu)\n",
                static_cast<unsigned long long>(drained),
                static_cast<unsigned long long>(total),
                static_cast<unsigned long long>(dropped));
    std::printf("  malloc=%llu free=%llu calloc=%llu realloc=%llu memalign=%llu\n",
                static_cast<unsigned long long>(counts[HV_OP_MALLOC]),
                static_cast<unsigned long long>(counts[HV_OP_FREE]),
                static_cast<unsigned long long>(counts[HV_OP_CALLOC]),
                static_cast<unsigned long long>(counts[HV_OP_REALLOC]),
                static_cast<unsigned long long>(counts[HV_OP_MEMALIGN]));

    munmap(ring, static_cast<std::size_t>(bytes));
    std::free(batch);

    int failures = 0;
    auto check = [&](bool ok, const char *what) {
        if (!ok) { std::fprintf(stderr, "  FAIL %s\n", what); ++failures; }
    };

    check(WIFEXITED(status) && WEXITSTATUS(status) == 0, "target exited cleanly");
    check(drained > 10000, "drained a meaningful number of events");
    check(counts[HV_OP_MALLOC] > 0, "saw malloc events");
    check(counts[HV_OP_FREE]   > 0, "saw free events");
    check(counts[HV_OP_CALLOC] > 0, "saw calloc events");
    check(counts[HV_OP_REALLOC] > 0, "saw realloc events");
    check(saw_large, "saw an allocation over the 128 KB mmap threshold");
    check(bad_op   == 0, "no corrupt opcodes");
    check(zero_ptr == 0, "no null pointers reported");
    check(zero_ts  == 0, "no zero timestamps");

    if (failures != 0) {
        std::fprintf(stderr, "intercept_e2e: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("intercept_e2e: interception verified across a process boundary\n");
    return 0;
}
