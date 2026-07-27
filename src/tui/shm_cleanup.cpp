/* heapviz - stale shared-memory segment cleanup.
 *
 * Copyright (C) 2026 Parth Sinha
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "tui/shm_cleanup.h"

#include "common/heapviz_abi.h"

#include <cerrno>
#include <csignal>
#include <cstring>
#include <dirent.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace hv {

namespace {

/* POSIX shared memory is a tmpfs mount on Linux, and shm_open names map to
 * files directly beneath it. There is no portable way to enumerate shm objects,
 * so this is Linux-specific, which the project already is. */
constexpr char kShmDir[] = "/dev/shm";

/* The interceptor builds names as HV_SHM_NAME_PREFIX + decimal pid. Parsing it
 * back is what tells us which process a segment belonged to. Returns false for
 * anything that is not one of ours, so an unrelated file in /dev/shm is never
 * a candidate for unlinking. */
bool parse_pid(const char *entry, std::int32_t &pid_out) {
    constexpr char prefix[] = HV_SHM_NAME_PREFIX;
    /* HV_SHM_NAME_PREFIX carries the leading slash shm_open wants; the
     * directory entry does not have it. */
    const char *want = (prefix[0] == '/') ? prefix + 1 : prefix;
    const std::size_t want_len = std::strlen(want);

    if (std::strncmp(entry, want, want_len) != 0) return false;

    const char *digits = entry + want_len;
    if (*digits == '\0') return false;

    std::int64_t v = 0;
    for (const char *p = digits; *p != '\0'; ++p) {
        if (*p < '0' || *p > '9') return false;
        v = v * 10 + (*p - '0');
        if (v > 0x7FFFFFFF) return false;
    }
    pid_out = static_cast<std::int32_t>(v);
    return true;
}

/* kill(pid, 0) reports whether we could signal the process. EPERM means it
 * exists but belongs to someone else, which still counts as alive; only ESRCH
 * means gone. Getting this backwards would delete a live target's ring. */
bool pid_alive(std::int32_t pid) {
    if (pid <= 0) return false;
    if (::kill(static_cast<pid_t>(pid), 0) == 0) return true;
    return errno != ESRCH;
}

} // namespace

std::vector<StaleSegment> list_segments() {
    std::vector<StaleSegment> out;

    DIR *d = ::opendir(kShmDir);
    if (d == nullptr) return out;

    for (dirent *e = ::readdir(d); e != nullptr; e = ::readdir(d)) {
        std::int32_t pid = 0;
        if (!parse_pid(e->d_name, pid)) continue;

        StaleSegment s;
        s.name = e->d_name;
        s.pid  = pid;

        std::string path = std::string(kShmDir) + "/" + e->d_name;
        struct stat st {};
        if (::stat(path.c_str(), &st) == 0) {
            s.bytes = static_cast<std::uint64_t>(st.st_size);
        }
        s.owner_alive = pid_alive(pid);
        out.push_back(std::move(s));
    }
    ::closedir(d);
    return out;
}

int reap_stale_segments(bool dry_run, std::uint64_t *bytes_freed) {
    int           removed = 0;
    std::uint64_t freed   = 0;

    for (const StaleSegment &s : list_segments()) {
        if (s.owner_alive) continue; /* someone is being profiled right now */

        if (!dry_run) {
            /* shm_unlink wants the name with its leading slash, the form the
             * producer passed to shm_open. */
            const std::string shm_name = "/" + s.name;
            if (::shm_unlink(shm_name.c_str()) != 0) continue;
        }
        ++removed;
        freed += s.bytes;
    }

    if (bytes_freed != nullptr) *bytes_freed = freed;
    return removed;
}

} // namespace hv
