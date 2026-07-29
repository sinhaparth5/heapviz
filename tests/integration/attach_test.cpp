/* heapviz - attaching to a real target, end to end (M2.3).
 *
 * Copyright (C) 2026 Parth Sinha
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The milestone's definition of done, as a test: `heapviz -- ./churn` launches,
 * attaches, shows live bounds, and survives the target exiting. Everything here
 * runs the shipped objects -- `launch_target`, `RingSession`, `HeapApp` and the
 * real `EventLoop` -- against a real preloaded process.
 *
 * No pty, because the loop takes its terminal measurement and its `write(2)` as
 * parameters. That is not a shortcut around the display: what needs asserting is
 * what the *model* concluded, and scraping a framebuffer for the string "Live:"
 * would test the header's formatting while claiming to test attachment.
 *
 * Ordering is the one thing to be careful about when editing. `churn` is held in
 * its constructor by HEAPVIZ_WAIT_MS until a consumer claims the ring, so the
 * launch cannot race ahead of the attach; every assertion about events then has
 * a target that is guaranteed to have been watched from its first allocation.
 */

#include "common/heapviz_abi.h"
#include "tui/capabilities.h"
#include "tui/event_loop.h"
#include "tui/heap_app.h"
#include "tui/session.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

int g_failures = 0;

void check(bool ok, const char *what) {
    if (!ok) { std::fprintf(stderr, "  FAIL %s\n", what); ++g_failures; }
}

const char *g_preload = nullptr;
const char *g_churn   = nullptr;

bool fake_size(int, int &w, int &h) { w = 120; h = 40; return true; }

ssize_t discard_write(int, const void *, std::size_t n) {
    return static_cast<ssize_t>(n);
}

hv::LoopConfig loop_config(int in_fd, std::uint64_t frames) {
    hv::LoopConfig cfg;
    cfg.in_fd      = in_fd;
    cfg.out_fd     = -1; /* never touched: the writer is substituted */
    cfg.max_frames = frames;
    cfg.size_fn    = fake_size;
    cfg.writer     = discard_write;
    return cfg;
}

/* Launches churn with the arguments given and returns its pid, or -1. */
int launch_churn(const std::vector<std::string> &args) {
    std::vector<char *> argv;
    argv.push_back(const_cast<char *>(g_churn));
    for (const std::string &a : args)
        argv.push_back(const_cast<char *>(a.c_str()));
    argv.push_back(nullptr);

    const hv::Launch l = hv::launch_target(argv.data(), g_preload,
                                           hv::kAttachTimeoutMs);
    if (l.status != hv::LaunchStatus::Ok) {
        std::fprintf(stderr, "  launch failed: %s: %s\n",
                     hv::launch_status_str(l.status), std::strerror(l.err));
        return -1;
    }
    return l.pid;
}

/* Waits for a launched target to finish. Every test that launches one ends by
 * calling this: a test that returns while its child is still allocating leaves
 * churn writing to the shared stderr during the next test, and leaves an
 * unreaped process behind if the suite is killed. */
void wait_for(int pid) {
    if (pid > 0) ::waitpid(static_cast<pid_t>(pid), nullptr, 0);
}

/* --- the whole thing ------------------------------------------------------ */

void test_launch_attach_and_watch() {
    const int pid = launch_churn({"--threads", "1", "--seconds", "2"});
    check(pid > 0, "e2e: churn launched under LD_PRELOAD");
    if (pid <= 0) return;

    hv::RingSession session;
    const hv::AttachStatus st = session.attach(pid);
    check(st == hv::AttachStatus::Ok, "e2e: attached to it");
    if (st != hv::AttachStatus::Ok) {
        std::fprintf(stderr, "  attach: %s\n", hv::attach_status_str(st));
        return;
    }

    check(session.pid() == pid, "e2e: the session knows its target");
    check(std::strcmp(session.comm(), "churn") == 0,
          "e2e: and reads its name out of the ring header");
    check(session.capacity() != 0, "e2e: the ring has a capacity");

    const int devnull = ::open("/dev/null", O_RDONLY);
    hv::EventLoop loop(loop_config(devnull, 400));
    hv::HeapApp   app(session, hv::Capabilities{});
    loop.run(app);
    ::close(devnull);

    /* The target allocates for two seconds; 400 frames at 60 fps is about six
     * and a half, so it has certainly finished. */
    check(app.events_seen() > 1000, "e2e: events were drained from the ring");
    check(app.state() == hv::SessionState::Exited,
          "e2e: the target exiting was noticed");

    /* Live bounds: the whole point of M2.1 feeding M2.3. A target that has
     * allocated has a heap, and the map is pointed inside it. */
    check(app.maps().scans() > 0, "e2e: its memory map was read");
    check(!app.view_range().empty(), "e2e: the map has real bounds");
    check(app.maps().main_arena(), "e2e: with a main arena in them");

    /* The model converges on the target's own final state. churn frees
     * everything it holds before returning, so a session that watched it from
     * the first allocation ends with almost nothing live -- and, crucially,
     * with the map agreeing. A map showing hundreds of live chunks after the
     * target freed them is the shape a lost `free` takes. */
    check(app.live_chunks() < 32, "e2e: churn's live set drained back down");
    check(app.map().total_live_chunks() <= app.live_chunks(),
          "e2e: the map never claims more live chunks than the model has");

    /* Attaching at launch means there is no earlier history to have missed, so
     * every free should have found its malloc. A handful arrive from libc's own
     * startup allocations, which happen before the ring exists. */
    check(app.frees_unknown() < 64,
          "e2e: nearly every free matched an allocation we saw");

    std::printf("  e2e: %llu events, %llu unmatched frees, %llu live at exit, "
                "%zu regions\n",
                static_cast<unsigned long long>(app.events_seen()),
                static_cast<unsigned long long>(app.frees_unknown()),
                static_cast<unsigned long long>(app.live_chunks()),
                app.maps().regions().size());

    session.detach();
    wait_for(pid);
}

/* --- the map actually fills in -------------------------------------------- */

void test_the_map_sees_the_heap() {
    /* One worker and a steady rate, so the traffic concentrates in a single
     * arena and the displayed region is unambiguous.
     *
     * Note that it is *not* the main arena, which is the thing this test was
     * written after discovering: churn does its work on a spawned thread even
     * at --threads 1, and glibc gives every allocating thread its own arena.
     * A version of `choose_view` that preferred `[heap]` displayed a region
     * holding two chunks out of five hundred, and every assertion above this
     * one still passed. */
    const int pid = launch_churn({"--threads", "1", "--seconds", "2",
                                  "--mode", "steady"});
    if (pid <= 0) { check(false, "map: churn launched"); return; }

    hv::RingSession session;
    if (session.attach(pid) != hv::AttachStatus::Ok) {
        check(false, "map: attached");
        return;
    }

    const int devnull = ::open("/dev/null", O_RDONLY);
    hv::EventLoop loop(loop_config(devnull, 90)); /* ~1.5s: still allocating */
    hv::HeapApp   app(session, hv::Capabilities{});
    loop.run(app);
    ::close(devnull);

    check(app.state() == hv::SessionState::Live,
          "map: sampled while the target was still running");
    check(app.live_chunks() > 16, "map: the target is holding allocations");

    /* The one that matters: those live chunks reached the display. Everything
     * upstream can be correct -- events drained, table populated, bounds found
     * -- and the map still be empty if the grid was pointed at the wrong
     * region, which is exactly the bug this file was written after. */
    check(app.map().total_live_chunks() > 0, "map: and the map has them");
    check(app.map().total_live_bytes() > 0, "map: with bytes behind them");

    /* Most of a steady single-threaded run should land on the map, not a
     * handful of it. Deliberately loose: the region is chosen from a scan up to
     * 500 ms old, so a heap that just grew has allocations beyond its edge. */
    const double on_map = static_cast<double>(app.map().total_live_chunks()) /
                          static_cast<double>(app.live_chunks());
    check(on_map > 0.5, "map: most of the live set is displayed");

    std::printf("  map: %llu of %llu live chunks on the map (%.0f%%), "
                "%llu bytes, %d regions hidden\n",
                static_cast<unsigned long long>(app.map().total_live_chunks()),
                static_cast<unsigned long long>(app.live_chunks()),
                on_map * 100.0,
                static_cast<unsigned long long>(app.map().total_live_bytes()),
                app.hidden_regions());

    session.detach();
    wait_for(pid);
}

/* --- the refusals --------------------------------------------------------- */

void test_a_second_consumer_is_refused() {
    const int pid = launch_churn({"--threads", "1", "--seconds", "2"});
    if (pid <= 0) { check(false, "claim: churn launched"); return; }

    hv::RingSession first;
    check(first.attach(pid) == hv::AttachStatus::Ok, "claim: the first attaches");

    /* `tail` is one cursor. Two consumers would each advance it past events the
     * other never copied out, and both would show a plausible half of the
     * stream with nothing anywhere reporting a problem. */
    hv::RingSession second;
    const hv::AttachStatus st = second.attach(pid, 200);
    check(st == hv::AttachStatus::AlreadyWatched, "claim: the second is refused");
    check(second.other_consumer() == static_cast<std::uint32_t>(::getpid()),
          "claim: and is told who holds it");
    check(!second.attached(), "claim: the refused one mapped nothing");

    /* Releasing hands it on, which is what makes a clean quit different from a
     * crash: the next heapviz can attach. */
    first.detach();
    hv::RingSession third;
    check(third.attach(pid, 1000) == hv::AttachStatus::Ok,
          "claim: released, the next consumer gets it");
    third.detach();
    wait_for(pid);
}

void test_attaching_to_nothing() {
    /* A live process with no interceptor in it: this one. The segment never
     * appears, and the timeout is the only thing that ends the wait. */
    hv::RingSession s;
    const hv::AttachStatus st = s.attach(static_cast<int>(::getpid()), 150);
    check(st == hv::AttachStatus::NoSegment, "none: an unpreloaded target");
    check(!s.attached(), "none: nothing was mapped");

    /* A pid that does not exist is answered rather than waited on: polling for
     * three seconds cannot make a dead process appear. */
    hv::RingSession dead;
    const std::uint64_t t0 = hv::monotonic_ns();
    const hv::AttachStatus dst = dead.attach(0x7FFFFFFF, 5000);
    const std::uint64_t took_ms = (hv::monotonic_ns() - t0) / 1000000ull;
    check(dst == hv::AttachStatus::TargetGone, "none: a dead pid is reported");
    check(took_ms < 1000, "none: and is not waited out");
}

void test_launching_something_that_is_not_there() {
    /* Without the pipe this would look like a successful launch followed by a
     * three-second attach timeout, and heapviz would report "target is not
     * running libheapviz.so" for a command that does not exist. */
    char name[] = "/nonexistent/heapviz-no-such-program";
    char *argv[] = {name, nullptr};

    const hv::Launch l = hv::launch_target(argv, g_preload, 100);
    check(l.status == hv::LaunchStatus::ExecFailed,
          "exec: a missing command is reported as a missing command");
    check(l.err == ENOENT, "exec: with the errno that says so");
    check(l.pid < 0, "exec: and no pid to attach to");
}

void test_finding_the_preload() {
    /* The beside-the-binary search is deliberately not asserted here. It looks
     * next to /proc/self/exe, and /proc/self/exe is this test, which lives in
     * tests/ -- a directory the shipped layout does not have. Asserting on it
     * would mean either testing that the search fails, which says nothing, or
     * moving the library to satisfy the test. What the search actually has to
     * get right for a user is covered by `heapviz -- <cmd>` finding it in
     * build/<preset>/, where the binary and the library really are siblings. */
    ::setenv("HEAPVIZ_PRELOAD", "/somewhere/else/libheapviz.so", 1);
    check(hv::find_preload() == "/somewhere/else/libheapviz.so",
          "preload: an explicit path wins, even one that does not exist");

    /* Taken as given rather than validated, so a user who names the wrong file
     * gets the loader's complaint about that file instead of heapviz silently
     * injecting a different one. */
    ::unsetenv("HEAPVIZ_PRELOAD");
}

} // namespace

int main(int argc, char **argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: %s <libheapviz.so> <churn>\n", argv[0]);
        return 2;
    }
    g_preload = argv[1];
    g_churn   = argv[2];

    test_finding_the_preload();
    test_launching_something_that_is_not_there();
    test_attaching_to_nothing();
    test_launch_attach_and_watch();
    test_the_map_sees_the_heap();
    test_a_second_consumer_is_refused();

    if (g_failures != 0) {
        std::fprintf(stderr, "attach_test: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("attach_test: launching, attaching, draining into the map, the "
                "single-consumer claim and the refusals all hold\n");
    return 0;
}
