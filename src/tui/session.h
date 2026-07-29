/* heapviz - attaching to a target's ring (M2.3).
 *
 * Copyright (C) 2026 Parth Sinha
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The consumer half of the shared-memory contract: find a target's segment,
 * check that it is one of ours and one this build understands, map it, and take
 * exclusive ownership of the read cursor. `tests/support/ring_attach.h` has been
 * the reference implementation of this since M1; this is the same sequence with
 * the failures given names the UI can print instead of a null return.
 *
 * WHY THE ORDER OF THE CHECKS IS THE WHOLE DESIGN
 * ----------------------------------------------
 * The producer publishes `magic` last, with a release store, precisely so a
 * consumer can tell "the constructor has not finished" from "this is not a
 * heapviz segment". Reading `capacity` before the magic matches would be
 * reading a field that is not yet written, and the mapping length computed from
 * it would be garbage. So: map the header alone, match the magic or retry,
 * check the ABI version, only then read capacity and map the rest.
 *
 * The claim is last and is a compare-and-swap rather than a store, because
 * `tail` is a single cursor. Two consumers would each advance it past events the
 * other never copied out, and both would show a plausible half of the stream
 * with no error anywhere -- which is exactly the failure a profiler must not
 * have, since the user has no independent way to notice.
 *
 * WHY LAUNCHING NEEDS A PIPE
 * --------------------------
 * `heapviz -- ./a.out` forks and execs with LD_PRELOAD injected. If the exec
 * fails there is no target, no segment, and nothing to attach to -- and the
 * attach would eventually time out and report "target is not running
 * libheapviz.so", which is a confident and completely wrong diagnosis of
 * "command not found". The close-on-exec pipe is how the parent learns which
 * of the two happened.
 */

#ifndef HEAPVIZ_TUI_SESSION_H
#define HEAPVIZ_TUI_SESSION_H

#include "common/heapviz_abi.h"

#include <cstdint>
#include <string>

namespace hv {

/* Long enough for a target that is still running its own static initialisers,
 * short enough that a wrong `--pid` is answered rather than waited on. */
constexpr int kAttachTimeoutMs = 3000;

enum class AttachStatus : std::uint8_t {
    Ok,
    NoSegment,      /* nothing appeared: not preloaded, or the wrong pid    */
    TargetGone,     /* the process exited before a segment showed up        */
    AbiMismatch,    /* a heapviz segment from a differently versioned build */
    AlreadyWatched, /* another heapviz owns the read cursor                 */
    MapFailed,      /* shm_open or mmap failed for a reason of its own      */
};

/* A sentence, not a word: these are printed to a user who has just had an
 * attach refused and needs to know what to do differently. */
const char *attach_status_str(AttachStatus s) noexcept;

class RingSession {
public:
    RingSession() = default;
    ~RingSession();

    /* The mapping and the claim are both owned, and neither is copyable. */
    RingSession(const RingSession &) = delete;
    RingSession &operator=(const RingSession &) = delete;

    /* Polls for the target's segment until it appears or `timeout_ms` runs out.
     * Polling rather than waiting on anything is deliberate: there is no
     * synchronisation primitive shared with a process that has not created its
     * segment yet, and the alternative -- an inotify watch on /dev/shm -- is a
     * lot of machinery for a wait measured in milliseconds. */
    AttachStatus attach(int pid, int timeout_ms = kAttachTimeoutMs);

    /* Releases the claim and unmaps. Idempotent, and the destructor calls it:
     * a claim that outlives its consumer locks the ring against every future
     * one, and the process that could have told anyone is gone. */
    void detach() noexcept;

    bool attached() const noexcept { return ring_ != nullptr; }
    int  pid()      const noexcept { return pid_; }

    /* Populated on AlreadyWatched and AbiMismatch respectively, so the error
     * can name the other consumer's pid and the version that was found. */
    std::uint32_t other_consumer() const noexcept { return other_consumer_; }
    std::uint32_t target_abi()     const noexcept { return target_abi_; }

    /* The target's `comm`, for the title bar. NUL-terminated even if the
     * producer filled all 16 bytes. Empty when not attached. */
    const char *comm() const noexcept { return comm_; }

    std::uint32_t drain(HvEvent *out, std::uint32_t max) noexcept;

    /* Ring telemetry, straight off the header. Zero when not attached, so a
     * caller need not branch on `attached()` to draw a status line. */
    std::uint64_t capacity()     const noexcept;
    std::uint64_t queued()       const noexcept; /* head - tail, unread depth */
    std::uint64_t dropped()      const noexcept;
    std::uint64_t total_events() const noexcept;

    /* The producer ran its destructor. Distinct from the process being gone:
     * a SIGKILLed target never sets this, which is why the UI checks both. */
    bool producer_exited() const noexcept;

private:
    HvRingHeader *ring_  = nullptr;
    std::uint64_t bytes_ = 0;
    int           pid_   = 0;
    std::uint32_t other_consumer_ = 0;
    std::uint32_t target_abi_     = 0;
    char          comm_[17]       = {};
};

/* --- launching a target -------------------------------------------------- */

enum class LaunchStatus : std::uint8_t {
    Ok,
    NoPreload,   /* libheapviz.so could not be found to inject          */
    ForkFailed,
    ExecFailed,  /* the command does not exist or is not executable     */
};

const char *launch_status_str(LaunchStatus s) noexcept;

struct Launch {
    LaunchStatus status = LaunchStatus::Ok;
    int          pid    = -1;
    int          err    = 0; /* errno from the failed step, for strerror */
};

/* Where `libheapviz.so` is, for LD_PRELOAD. In order: `HEAPVIZ_PRELOAD` if the
 * user set it, then next to the running binary (which is what a build tree
 * looks like), then `../lib` beside it (which is what an install looks like).
 * Empty when none of those exist -- reported rather than guessed, because an
 * LD_PRELOAD the loader cannot resolve is a warning on stderr that scrolls past
 * and a target that silently produces nothing. */
std::string find_preload();

/* fork + exec with LD_PRELOAD injected, appending to whatever the user already
 * had rather than replacing it. `wait_ms` becomes HEAPVIZ_WAIT_MS, which holds
 * the target in its constructor until a consumer claims the ring: without it a
 * program that allocates and exits is finished, and has unlinked its segment,
 * before heapviz has finished starting up. */
Launch launch_target(char *const argv[], const std::string &preload,
                     int wait_ms);

/* True when the pid is a process we could signal. `kill(pid, 0)` failing with
 * EPERM means it exists and belongs to someone else, which still counts. */
bool process_alive(int pid) noexcept;

/* Reaps a child that has exited, so a target launched by `-- <cmd>` does not
 * stay a zombie for as long as heapviz runs. Returns true once it is reaped;
 * false while it is still running, which is not an error. */
bool reap_if_exited(int pid) noexcept;

} // namespace hv

#endif /* HEAPVIZ_TUI_SESSION_H */
