/* heapviz - transitive interception check.
 *
 * Copyright (C) 2026 Parth Sinha
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Runs transitive_target under LD_PRELOAD and asserts heapviz saw at least as
 * many allocations as the target reported making. The target counts only its
 * own direct calls; libc and libstdc++ allocate on top of that, so the observed
 * count should comfortably exceed it. Seeing FEWER means an indirect path
 * (strdup, asprintf, getline, operator new) is escaping interception.
 */

#include "support/ring_attach.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/wait.h>
#include <unistd.h>

int main(int argc, char **argv) {
    if (argc != 3) {
        std::fprintf(stderr, "usage: transitive_e2e <libheapviz.so> <target>\n");
        return 2;
    }
    const char *so     = argv[1];
    const char *target = argv[2];

    int pipefd[2];
    if (pipe(pipefd) != 0) { std::perror("pipe"); return 1; }

    const pid_t pid = fork();
    if (pid < 0) { std::perror("fork"); return 1; }

    if (pid == 0) {
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        close(pipefd[1]);
        setenv("LD_PRELOAD", so, 1);
        setenv("HEAPVIZ_CAPACITY", "65536", 1);
        // Hold the target in its constructor until we have attached, so a
        // fast program cannot finish and unlink the segment first.
        setenv("HEAPVIZ_WAIT_MS", "5000", 1);
        execl(target, target, nullptr);
        std::perror("execl");
        _exit(127);
    }
    close(pipefd[1]);

    std::uint64_t bytes = 0;
    HvRingHeader *ring = hvtest::attach(pid, bytes, 5000);
    if (ring == nullptr) {
        std::fprintf(stderr, "could not attach to target ring\n");
        kill(pid, SIGKILL);
        waitpid(pid, nullptr, 0);
        return 1;
    }

    constexpr std::uint32_t kBatch = 8192;
    auto *batch = static_cast<HvEvent *>(std::calloc(kBatch, sizeof(HvEvent)));
    if (batch == nullptr) { std::perror("calloc"); return 1; }

    std::uint64_t allocs = 0, frees = 0;
    int status = 0;
    bool child_done = false;

    for (;;) {
        const std::uint32_t n = hv_ring_drain(ring, batch, kBatch);
        for (std::uint32_t i = 0; i < n; ++i) {
            if (batch[i].op == HV_OP_FREE) ++frees;
            else                           ++allocs;
        }
        if (n == 0) {
            if (child_done) break;
            if (ring->producer_exited.load(std::memory_order_acquire) != 0) {
                child_done = true; /* one more pass for stragglers */
                continue;
            }
            if (waitpid(pid, &status, WNOHANG) == pid) { child_done = true; continue; }
            hvtest::sleep_ms(1);
        }
    }
    waitpid(pid, &status, 0);

    /* Read what the target claimed it did. */
    char buf[256] = {};
    const ssize_t rn = read(pipefd[0], buf, sizeof(buf) - 1);
    close(pipefd[0]);
    long reported = 0;
    if (rn > 0) {
        const char *p = std::strstr(buf, ": ");
        if (p != nullptr) reported = std::strtol(p + 2, nullptr, 10);
    }

    const std::uint64_t dropped = ring->dropped.load(std::memory_order_relaxed);
    munmap(ring, static_cast<std::size_t>(bytes));
    std::free(batch);

    std::printf("target reported %ld allocating calls\n", reported);
    std::printf("heapviz saw %llu allocations, %llu frees (dropped %llu)\n",
                static_cast<unsigned long long>(allocs),
                static_cast<unsigned long long>(frees),
                static_cast<unsigned long long>(dropped));

    int failures = 0;
    auto check = [&](bool ok, const char *what) {
        if (!ok) { std::fprintf(stderr, "  FAIL %s\n", what); ++failures; }
    };

    check(WIFEXITED(status) && WEXITSTATUS(status) == 0, "target exited cleanly");
    check(reported > 0, "target reported its own call count");
    check(dropped == 0, "no events dropped (ring was sized for this run)");
    check(allocs >= static_cast<std::uint64_t>(reported),
          "observed allocations cover every call the target made");
    check(frees > 0, "saw frees");

    if (failures != 0) {
        std::fprintf(stderr, "transitive_e2e: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("transitive_e2e: strdup/asprintf/getline/operator new all "
                "reach the interceptor\n");
    return 0;
}
