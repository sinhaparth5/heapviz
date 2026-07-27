/* heapviz - stale segment reaping.
 *
 * Copyright (C) 2026 Parth Sinha
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The dangerous direction here is not "failed to clean up" but "cleaned up a
 * segment whose target was still running", which would pull the ring out from
 * under a live profiling session. Both directions are checked, and the live
 * case uses this test's own pid so there is no doubt the process exists.
 */

#include "common/heapviz_abi.h"
#include "tui/shm_cleanup.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

int g_failures = 0;

void check(bool ok, const char *what) {
    if (!ok) { std::fprintf(stderr, "  FAIL %s\n", what); ++g_failures; }
}

/* Creates a segment named exactly as the interceptor would for `pid`. */
bool make_segment(std::int32_t pid, std::size_t bytes) {
    char name[HV_SHM_NAME_MAX];
    hv_shm_name(name, sizeof(name), pid);

    shm_unlink(name); /* in case a previous run left one behind */
    const int fd = shm_open(name, O_CREAT | O_EXCL | O_RDWR, 0600);
    if (fd < 0) { std::perror("shm_open"); return false; }
    if (ftruncate(fd, static_cast<off_t>(bytes)) != 0) {
        std::perror("ftruncate");
        close(fd);
        shm_unlink(name);
        return false;
    }
    close(fd);
    return true;
}

bool segment_exists(std::int32_t pid) {
    char name[HV_SHM_NAME_MAX];
    hv_shm_name(name, sizeof(name), pid);
    const int fd = shm_open(name, O_RDONLY, 0);
    if (fd < 0) return false;
    close(fd);
    return true;
}

void unlink_segment(std::int32_t pid) {
    char name[HV_SHM_NAME_MAX];
    hv_shm_name(name, sizeof(name), pid);
    shm_unlink(name);
}

/* A pid that is certainly gone: fork a child that exits immediately and reap
 * it. Its pid cannot be reused while we hold the zombie's slot open, and after
 * waitpid it is free but overwhelmingly unlikely to be recycled within this
 * test. Inventing a large constant instead would risk hitting a real process. */
std::int32_t dead_pid() {
    const pid_t p = fork();
    if (p == 0) _exit(0);
    if (p < 0) { std::perror("fork"); std::exit(1); }
    int status = 0;
    waitpid(p, &status, 0);
    return static_cast<std::int32_t>(p);
}

} // namespace

int main() {
    const std::int32_t live = static_cast<std::int32_t>(getpid());
    const std::int32_t dead = dead_pid();

    if (!make_segment(dead, 4096) || !make_segment(live, 4096)) return 1;

    /* Both should be listed, with only the live one marked as such. */
    {
        const auto segs = hv::list_segments();
        const auto find = [&](std::int32_t pid) {
            return std::find_if(segs.begin(), segs.end(),
                                [&](const hv::StaleSegment &s) { return s.pid == pid; });
        };
        const auto it_dead = find(dead);
        const auto it_live = find(live);

        check(it_dead != segs.end(), "list: found the dead producer's segment");
        check(it_live != segs.end(), "list: found the live producer's segment");
        if (it_dead != segs.end())
            check(!it_dead->owner_alive, "list: the dead producer is reported dead");
        if (it_live != segs.end())
            check(it_live->owner_alive, "list: the live producer is reported alive");
    }

    /* A dry run must report without removing anything. */
    {
        std::uint64_t freed = 0;
        const int n = hv::reap_stale_segments(true, &freed);
        check(n >= 1, "dry run: reported at least the one stale segment");
        check(segment_exists(dead), "dry run: removed nothing");
    }

    /* The real thing. */
    {
        std::uint64_t freed = 0;
        const int n = hv::reap_stale_segments(false, &freed);
        check(n >= 1, "reap: removed at least one segment");
        check(freed >= 4096, "reap: reported the bytes reclaimed");
        check(!segment_exists(dead), "reap: the dead producer's segment is gone");
        check(segment_exists(live),
              "reap: a LIVE producer's segment was left alone");
    }

    unlink_segment(live);

    if (g_failures != 0) {
        std::fprintf(stderr, "shm_cleanup_test: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("shm_cleanup_test: stale rings are reaped, live ones are not\n");
    return 0;
}
