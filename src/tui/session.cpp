/* heapviz - attaching to a target's ring (M2.3).
 *
 * Copyright (C) 2026 Parth Sinha
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "tui/session.h"

#include "common/heapviz_ring.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

#include <fcntl.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

namespace hv {

namespace {

constexpr int kPollIntervalMs = 5;

void sleep_ms(long ms) noexcept {
    timespec ts{ms / 1000, (ms % 1000) * 1000000};
    ::nanosleep(&ts, nullptr);
}

bool file_exists(const std::string &path) noexcept {
    struct stat st {};
    return !path.empty() && ::stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

/* The directory the running heapviz binary lives in. /proc/self/exe rather than
 * argv[0], which is whatever the caller felt like passing and is routinely a
 * bare name found on PATH. */
std::string exe_dir() {
    char buf[4096];
    const ssize_t n = ::readlink("/proc/self/exe", buf, sizeof buf - 1);
    if (n <= 0) return {};
    buf[n] = '\0';

    std::string path(buf);
    const std::size_t slash = path.rfind('/');
    return slash == std::string::npos ? std::string{} : path.substr(0, slash);
}

} // namespace

const char *attach_status_str(AttachStatus s) noexcept {
    switch (s) {
    case AttachStatus::Ok:             return "attached";
    case AttachStatus::NoSegment:      return "target is not running libheapviz.so";
    case AttachStatus::TargetGone:     return "target exited before it could be attached to";
    case AttachStatus::AbiMismatch:    return "target was built against a different heapviz ABI";
    case AttachStatus::AlreadyWatched: return "target is already being watched by another heapviz";
    case AttachStatus::MapFailed:      return "could not map the target's shared memory";
    }
    return "could not map the target's shared memory";
}

const char *launch_status_str(LaunchStatus s) noexcept {
    switch (s) {
    case LaunchStatus::Ok:         return "launched";
    case LaunchStatus::NoPreload:  return "could not find libheapviz.so to inject";
    case LaunchStatus::ForkFailed: return "could not fork";
    case LaunchStatus::ExecFailed: return "could not run the command";
    }
    return "could not run the command";
}

RingSession::~RingSession() { detach(); }

AttachStatus RingSession::attach(int pid, int timeout_ms) {
    detach();

    char name[HV_SHM_NAME_MAX];
    hv_shm_name(name, sizeof name, pid);

    for (int waited = 0;; waited += kPollIntervalMs) {
        const int fd = ::shm_open(name, O_RDWR, 0);
        if (fd < 0) {
            /* No segment yet. If the process is not there either, waiting the
             * rest of the timeout out cannot change the answer, and a wrong
             * --pid deserves a reply rather than three seconds of nothing.
             *
             * Checked only on this path: a live process with no segment is the
             * ordinary "still starting up" case and must keep polling. */
            if (!process_alive(pid)) return AttachStatus::TargetGone;
            if (waited >= timeout_ms) return AttachStatus::NoSegment;
            sleep_ms(kPollIntervalMs);
            continue;
        }

        /* Header first: the mapping length depends on `capacity`, which is not
         * safe to read until the magic says the constructor has finished. */
        void *probe = ::mmap(nullptr, sizeof(HvRingHeader),
                             PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (probe == MAP_FAILED) { ::close(fd); return AttachStatus::MapFailed; }

        auto *hdr = static_cast<HvRingHeader *>(probe);
        if (hdr->magic.load(std::memory_order_acquire) != HEAPVIZ_ABI_MAGIC) {
            /* The segment exists but the producer is still initialising it.
             * The magic is published last with a release store precisely so
             * this is distinguishable from a foreign segment. */
            ::munmap(probe, sizeof(HvRingHeader));
            ::close(fd);
            if (waited >= timeout_ms) return AttachStatus::NoSegment;
            sleep_ms(kPollIntervalMs);
            continue;
        }

        if (hdr->abi_version != HEAPVIZ_ABI_VERSION ||
            hdr->event_size != sizeof(HvEvent)) {
            target_abi_ = hdr->abi_version;
            ::munmap(probe, sizeof(HvRingHeader));
            ::close(fd);
            return AttachStatus::AbiMismatch;
        }

        const std::uint64_t capacity = hdr->capacity;
        std::memcpy(comm_, hdr->comm, sizeof hdr->comm);
        comm_[sizeof hdr->comm] = '\0';
        ::munmap(probe, sizeof(HvRingHeader));

        const auto page = static_cast<std::uint64_t>(::sysconf(_SC_PAGESIZE));
        const std::uint64_t bytes = hv_mapping_size(capacity, page);

        void *full = ::mmap(nullptr, static_cast<std::size_t>(bytes),
                            PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        ::close(fd);
        if (full == MAP_FAILED) return AttachStatus::MapFailed;

        auto *ring = static_cast<HvRingHeader *>(full);

        std::uint32_t owner = 0;
        if (hv_ring_claim(ring, static_cast<std::uint32_t>(::getpid()), &owner)
            == 0) {
            other_consumer_ = owner;
            ::munmap(full, static_cast<std::size_t>(bytes));
            return AttachStatus::AlreadyWatched;
        }

        ring_  = ring;
        bytes_ = bytes;
        pid_   = pid;
        return AttachStatus::Ok;
    }
}

void RingSession::detach() noexcept {
    if (ring_ == nullptr) return;
    hv_ring_release(ring_, static_cast<std::uint32_t>(::getpid()));
    ::munmap(ring_, static_cast<std::size_t>(bytes_));
    ring_  = nullptr;
    bytes_ = 0;
    comm_[0] = '\0';
}

std::uint32_t RingSession::drain(HvEvent *out, std::uint32_t max) noexcept {
    if (ring_ == nullptr || out == nullptr || max == 0) return 0;
    return hv_ring_drain(ring_, out, max);
}

std::uint64_t RingSession::capacity() const noexcept {
    return ring_ == nullptr ? 0 : ring_->capacity;
}

std::uint64_t RingSession::queued() const noexcept {
    if (ring_ == nullptr) return 0;
    const std::uint64_t head = ring_->head.load(std::memory_order_relaxed);
    const std::uint64_t tail = ring_->tail.load(std::memory_order_relaxed);
    /* Both are free-running sequence numbers that only ever increase, so the
     * difference is the unread depth and needs no wrap handling of its own. */
    return head - tail;
}

std::uint64_t RingSession::dropped() const noexcept {
    return ring_ == nullptr ? 0
                            : ring_->dropped.load(std::memory_order_relaxed);
}

std::uint64_t RingSession::total_events() const noexcept {
    return ring_ == nullptr
               ? 0
               : ring_->total_events.load(std::memory_order_relaxed);
}

bool RingSession::producer_exited() const noexcept {
    return ring_ != nullptr &&
           ring_->producer_exited.load(std::memory_order_acquire) != 0;
}

/* --- launching ----------------------------------------------------------- */

std::string find_preload() {
    if (const char *env = ::getenv("HEAPVIZ_PRELOAD");
        env != nullptr && env[0] != '\0') {
        /* Taken as given even if it does not exist: the user said where it is,
         * and silently falling back to a different library than the one they
         * named would be worse than the loader's own complaint. */
        return env;
    }

    const std::string dir = exe_dir();
    if (!dir.empty()) {
        std::string beside = dir + "/libheapviz.so";
        if (file_exists(beside)) return beside;

        std::string installed = dir + "/../lib/libheapviz.so";
        if (file_exists(installed)) return installed;
    }
    return {};
}

Launch launch_target(char *const argv[], const std::string &preload,
                     int wait_ms) {
    Launch out;
    if (argv == nullptr || argv[0] == nullptr || preload.empty()) {
        out.status = LaunchStatus::NoPreload;
        return out;
    }

    /* Everything the child needs is built here, before the fork, rather than
     * between the fork and the exec. The parent is single-threaded (the event
     * loop is the only one), so a child allocating there would in fact be safe
     * -- but "safe because nothing else holds the malloc lock right now" is a
     * property of the whole program that this function cannot check and a
     * future thread would silently break. */
    std::string preload_value = preload;
    if (const char *existing = ::getenv("LD_PRELOAD");
        existing != nullptr && existing[0] != '\0') {
        /* Appended rather than assigned: a user profiling something that
         * already needs its own LD_PRELOAD should not have to choose. */
        preload_value += ':';
        preload_value += existing;
    }

    char wait_value[16] = {};
    const bool set_wait = wait_ms > 0 && ::getenv("HEAPVIZ_WAIT_MS") == nullptr;
    if (set_wait) std::snprintf(wait_value, sizeof wait_value, "%d", wait_ms);

    /* Close-on-exec, so a successful exec closes it and the parent reads EOF.
     * Anything actually written to it is the child's errno from a failed exec:
     * the one report that has to cross a process boundary that no longer has a
     * process on the far side of it.
     *
     * pipe(2) plus fcntl rather than pipe2(2), which is a GNU extension and
     * would mean putting _GNU_SOURCE on all of heapviz_core for one call. */
    int fds[2];
    if (::pipe(fds) != 0) {
        out.status = LaunchStatus::ForkFailed;
        out.err = errno;
        return out;
    }
    ::fcntl(fds[0], F_SETFD, FD_CLOEXEC);
    ::fcntl(fds[1], F_SETFD, FD_CLOEXEC);

    const pid_t child = ::fork();
    if (child < 0) {
        out.err = errno;
        ::close(fds[0]);
        ::close(fds[1]);
        out.status = LaunchStatus::ForkFailed;
        return out;
    }

    if (child == 0) {
        ::close(fds[0]);

        ::setenv("LD_PRELOAD", preload_value.c_str(), 1);
        if (set_wait) ::setenv("HEAPVIZ_WAIT_MS", wait_value, 1);

        ::execvp(argv[0], argv);

        /* Only reachable when the exec failed. */
        const int e = errno;
        const ssize_t ignored = ::write(fds[1], &e, sizeof e);
        (void)ignored;
        ::_exit(127);
    }

    ::close(fds[1]);

    int child_errno = 0;
    const ssize_t n = ::read(fds[0], &child_errno, sizeof child_errno);
    ::close(fds[0]);

    if (n == static_cast<ssize_t>(sizeof child_errno)) {
        /* The child told us why, then exited. Reap it so the diagnosis is not
         * accompanied by a zombie. */
        int st = 0;
        ::waitpid(child, &st, 0);
        out.status = LaunchStatus::ExecFailed;
        out.err = child_errno;
        return out;
    }

    out.status = LaunchStatus::Ok;
    out.pid = static_cast<int>(child);
    return out;
}

bool process_alive(int pid) noexcept {
    if (pid <= 0) return false;
    if (::kill(static_cast<pid_t>(pid), 0) == 0) return true;
    return errno != ESRCH;
}

bool reap_if_exited(int pid) noexcept {
    if (pid <= 0) return false;
    int st = 0;
    const pid_t r = ::waitpid(static_cast<pid_t>(pid), &st, WNOHANG);
    /* -1 with ECHILD means it was never ours or has already been reaped, which
     * for the caller's purposes is the same answer as having just reaped it. */
    return r == static_cast<pid_t>(pid) || (r < 0 && errno == ECHILD);
}

} // namespace hv
