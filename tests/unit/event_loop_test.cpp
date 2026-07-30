/* heapviz - event loop and frame pacing (M4.5).
 *
 * Copyright (C) 2026 Parth Sinha
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The loop's two claims are about what it does *not* do: it does not redraw
 * faster than the frame rate, and it does not burn CPU when nothing is
 * happening. Neither can be checked by looking at a return value, so most of
 * what follows is measured -- elapsed wall time against the number of frames.
 *
 * The loop takes its file descriptors, its clock's consumers, its terminal
 * measurement and its write(2) as parameters, which is what lets all of this be
 * a unit test: no pty, no child process, nothing on the screen. The one thing
 * that cannot be faked here is a real SIGWINCH from a real terminal, which is
 * what tests/integration/event_loop_pty_test.cpp is for.
 *
 * ORDERING MATTERS. hv::request_quit() sets a process-global flag with no way
 * back, so every scenario that trips it runs last. The check() at the top of
 * each scenario turns a future mis-ordering into a named failure rather than a
 * confusing cascade.
 */

#include "tui/event_loop.h"
#include "tui/terminal.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <sys/resource.h>
#include <unistd.h>

namespace {

int g_failures = 0;

void check(bool ok, const char *what) {
    if (!ok) { std::fprintf(stderr, "  FAIL %s\n", what); ++g_failures; }
}

/* --- fakes for the seams ------------------------------------------------ */

int g_size_w = 40;
int g_size_h = 12;

bool fake_size(int, int &w, int &h) {
    w = g_size_w;
    h = g_size_h;
    return true;
}

std::uint64_t g_discarded = 0;

ssize_t discard_write(int, const void *, std::size_t n) {
    g_discarded += n;
    return static_cast<ssize_t>(n);
}

ssize_t failing_write(int, const void *, std::size_t) {
    errno = EIO;
    return -1;
}

/* --- a minimal application ---------------------------------------------- */

class TestApp : public hv::LoopApp {
public:
    bool animate = false;
    bool reset_pending = false;

    unsigned drains  = 0;
    unsigned updates = 0;
    unsigned draws   = 0;

    std::string keys;
    int resized_w = 0, resized_h = 0;
    int resizes   = 0;

    unsigned drain() override { ++drains; return 0; }

    bool update(std::uint64_t) override { ++updates; return false; }

    bool animating() const override { return animate; }

    bool take_stats_reset() override {
        const bool reset = reset_pending;
        reset_pending = false;
        return reset;
    }

    void resized(int w, int h) override {
        resized_w = w;
        resized_h = h;
        ++resizes;
    }

    bool key(char c) override {
        keys.push_back(c);
        if (c == 'q') hv::request_quit();
        return true;
    }

    /* Deliberately sparse. Most cells stay at empty_cell(), which is what makes
     * the full-repaint check below meaningful: a differ running against a
     * freshly cleared front buffer would skip every blank cell and leave the
     * old frame's remains on a grown terminal. */
    void draw(hv::Framebuffer &fb, const hv::LoopStats &) override {
        ++draws;
        fb.clear();
        fb.text(0, 0, "heapviz", hv::kDefaultFg, hv::kDefaultBg);
    }
};

/* --- helpers ------------------------------------------------------------ */

std::uint64_t ms_since(std::uint64_t t0) {
    return (hv::monotonic_ns() - t0) / (1000ull * 1000ull);
}

/* User plus system time for this process, in milliseconds. Wall time says the
 * loop waited; this says whether it waited by sleeping or by spinning, which
 * is the difference the whole design is about. */
std::uint64_t cpu_ms() {
    rusage ru{};
    ::getrusage(RUSAGE_SELF, &ru);
    const auto to_ms = [](const timeval &tv) -> std::uint64_t {
        return static_cast<std::uint64_t>(tv.tv_sec) * 1000ull +
               static_cast<std::uint64_t>(tv.tv_usec) / 1000ull;
    };
    return to_ms(ru.ru_utime) + to_ms(ru.ru_stime);
}

hv::LoopConfig base_config(int in_fd, unsigned fps, std::uint64_t frames) {
    hv::LoopConfig cfg;
    cfg.in_fd      = in_fd;
    cfg.out_fd     = -1; /* never touched: writer is substituted */
    cfg.target_fps = fps;
    cfg.max_frames = frames;
    cfg.size_fn    = fake_size;
    cfg.writer     = discard_write;
    return cfg;
}

/* A pipe whose write end stays open, so poll() on the read end blocks for the
 * full timeout instead of reporting EOF. This is what a live terminal with
 * nobody typing looks like. */
struct QuietPipe {
    int rd = -1, wr = -1;
    QuietPipe() {
        int fds[2];
        if (::pipe(fds) != 0) { std::perror("pipe"); return; }
        rd = fds[0];
        wr = fds[1];
    }
    ~QuietPipe() {
        if (rd >= 0) ::close(rd);
        if (wr >= 0) ::close(wr);
    }
};

/* ====================================================================== */
/* Scenarios                                                              */
/* ====================================================================== */

/* Idle is the case the loop is built for: nothing arrived, nothing is
 * animating, so after the first frame there is nothing to draw and nothing to
 * write. The frames still happen -- the ring has to be checked -- but they cost
 * a poll() and two counters. */
void test_idle_frames_skip_the_draw() {
    check(!hv::quit_requested(), "ordering: the quit flag is still clear");

    QuietPipe pipe;
    g_size_w = 40;
    g_size_h = 12;
    g_discarded = 0;

    TestApp app;
    hv::EventLoop loop(base_config(pipe.rd, 200, 25));

    const std::uint64_t c0  = cpu_ms();
    const std::uint64_t t0  = hv::monotonic_ns();
    const hv::LoopExit  why = loop.run(app);
    const std::uint64_t elapsed = ms_since(t0);
    const std::uint64_t cpu     = cpu_ms() - c0;

    const hv::LoopStats &s = loop.stats();

    check(why == hv::LoopExit::FrameLimit, "idle: ran to the frame limit");
    check(s.frames == 25, "idle: every frame happened");
    check(s.drawn == 1, "idle: only the first frame drew");
    check(s.skipped == 24, "idle: the other 24 skipped draw, diff and write");
    check(s.writes == 1, "idle: one write(2) for the run, not one per frame");
    check(app.drains == 25, "idle: the ring is still drained every frame");
    check(app.draws == 1, "idle: draw() was not called on skipped frames");

    /* 25 frames at 200 fps is 125 ms of deadlines, and the poll timeout is
     * rounded up, so no frame can finish early. A loop that truncated the
     * timeout instead lands near 100 ms: poll(2) returns 0 immediately for a
     * zero timeout, and the loop reads that as "the deadline arrived". */
    check(elapsed >= 115, "idle: every frame ran for its full period");

    /* The claim the loop exists to support. Waiting is not the same as idling:
     * a poll() timeout truncated to zero milliseconds, or a poll on an fd that
     * is always readable, both wait exactly as long and burn a whole core doing
     * it. Only the CPU-time ratio can tell those apart. */
    check(cpu * 4 < elapsed,
          "idle: the wait was spent asleep, not spinning on the CPU");
}

/* The other half: something is animating, so every frame draws. This is the
 * upper bound the pacing enforces. */
void test_animating_draws_every_frame() {
    check(!hv::quit_requested(), "ordering: the quit flag is still clear");

    QuietPipe pipe;
    TestApp app;
    app.animate = true;

    hv::EventLoop loop(base_config(pipe.rd, 100, 15));

    const std::uint64_t t0 = hv::monotonic_ns();
    loop.run(app);
    const std::uint64_t elapsed = ms_since(t0);

    const hv::LoopStats &s = loop.stats();
    check(s.drawn == 15, "animating: every frame drew");
    check(s.skipped == 0, "animating: nothing was skipped");

    /* 15 frames at 100 fps is 150 ms. Without pacing this would finish in well
     * under a millisecond, since the app draws seven characters. */
    check(elapsed >= 140, "animating: draws are capped at the frame rate");
    check(elapsed < 2000, "animating: pacing did not overshoot wildly");
}

/* An exhausted stdin is always ready, so poll() on it returns instantly and
 * forever. A loop that kept polling would still hit every deadline on time --
 * it would just call poll() and read() a few hundred thousand times on the way,
 * turning the idle case into a spin at 100% of a core. Only the CPU-time ratio
 * separates the two.
 *
 * Both shapes of "no input left" are checked, because Linux reports them
 * differently and they are handled by different branches: /dev/null is readable
 * with a read() of zero bytes, while a pipe with no writers raises POLLHUP and
 * may never set POLLIN at all. */
void check_exhausted_input(int fd, const char *label) {
    TestApp app;
    hv::EventLoop loop(base_config(fd, 200, 20));

    const std::uint64_t c0  = cpu_ms();
    const std::uint64_t t0  = hv::monotonic_ns();
    const hv::LoopExit  why = loop.run(app);
    const std::uint64_t elapsed = ms_since(t0);
    const std::uint64_t cpu     = cpu_ms() - c0;

    char what[128];
    std::snprintf(what, sizeof what, "%s: the loop kept running", label);
    check(why == hv::LoopExit::FrameLimit, what);

    std::snprintf(what, sizeof what, "%s: it still paced itself", label);
    check(elapsed >= 92, what);

    std::snprintf(what, sizeof what,
                  "%s: it slept out the frame instead of spinning", label);
    check(cpu * 4 < elapsed, what);
}

void test_exhausted_stdin_does_not_spin() {
    check(!hv::quit_requested(), "ordering: the quit flag is still clear");

    const int devnull = ::open("/dev/null", O_RDONLY);
    if (devnull < 0) { std::perror("/dev/null"); ++g_failures; return; }
    check_exhausted_input(devnull, "eof(read)");
    ::close(devnull);

    int fds[2];
    if (::pipe(fds) != 0) { std::perror("pipe"); ++g_failures; return; }
    ::close(fds[1]); /* no writers: POLLHUP on fds[0] */
    check_exhausted_input(fds[0], "eof(hup)");
    ::close(fds[0]);
}

/* A resize must repaint every cell, not just the cells that differ from a
 * blank buffer. resize() clears both buffers, so a differ run without
 * full_repaint skips every blank cell and leaves the old frame's remains
 * visible on the part of the screen that just appeared. */
void test_resize_repaints_every_cell() {
    check(!hv::quit_requested(), "ordering: the quit flag is still clear");

    /* An app that grows the terminal from inside the loop, at the one point a
     * real SIGWINCH would be noticed. */
    class ResizingApp final : public TestApp {
    public:
        bool update(std::uint64_t now) override {
            TestApp::update(now);
            if (updates == 3) {
                g_size_w = 60;
                g_size_h = 20;
                hv::request_resize();
            }
            return false;
        }
    };

    QuietPipe pipe;
    g_size_w = 40;
    g_size_h = 12;

    ResizingApp app;
    hv::EventLoop loop(base_config(pipe.rd, 500, 6));
    loop.run(app);

    const hv::LoopStats &s = loop.stats();

    check(s.repaints == 2, "resize: the initial sizing and the growth both counted");
    check(app.resizes == 2, "resize: the app was told about both");
    check(app.resized_w == 60 && app.resized_h == 20,
          "resize: the app was told the new geometry");
    check(loop.framebuffer().width() == 60 && loop.framebuffer().height() == 20,
          "resize: the framebuffer follows the terminal");

    /* The decisive check. The resize frame is the last one that drew, and it
     * must have emitted all 60x20 cells. Without the full repaint it would
     * emit only the seven characters of "heapviz". */
    check(loop.renderer().cells_examined() == 60u * 20u,
          "resize: the whole new grid was examined");
    check(loop.renderer().cells_emitted() == 60u * 20u,
          "resize: every cell was repainted, not only the non-blank ones");
}

/* A terminal smaller than the loop can draw into is refused rather than
 * rendered into. M4.4 replaces this with an in-TUI message; the loop's job is
 * only to stop before it tries. */
void test_refuses_a_terminal_that_is_too_small() {
    check(!hv::quit_requested(), "ordering: the quit flag is still clear");

    QuietPipe pipe;
    g_size_w = 10;
    g_size_h = 3;

    TestApp app;
    hv::EventLoop loop(base_config(pipe.rd, 200, 5));
    const hv::LoopExit why = loop.run(app);

    check(why == hv::LoopExit::TooSmall, "too small: the loop refused to draw");
    check(app.draws == 0, "too small: nothing was drawn into a 10x3 grid");

    g_size_w = 40;
    g_size_h = 12;
}

/* The terminal disappearing mid-frame is a real exit path: the pty is gone when
 * the parent terminal emulator dies. */
void test_write_failure_ends_the_loop() {
    check(!hv::quit_requested(), "ordering: the quit flag is still clear");

    QuietPipe pipe;
    TestApp app;
    hv::LoopConfig cfg = base_config(pipe.rd, 200, 5);
    cfg.writer = failing_write;

    hv::EventLoop loop(cfg);
    const hv::LoopExit why = loop.run(app);

    check(why == hv::LoopExit::WriteFailed,
          "write failure: the loop stopped instead of drawing into the void");
    check(loop.stats().frames == 1, "write failure: it stopped on the first frame");
}

/* Per-phase timing exists so a slow frame can be attributed. A draw that
 * overruns the frame period has to show up in draw_ns specifically, and has to
 * be counted as an overrun. */
void test_timing_attributes_a_slow_draw() {
    check(!hv::quit_requested(), "ordering: the quit flag is still clear");

    class SlowApp final : public TestApp {
    public:
        void draw(hv::Framebuffer &fb, const hv::LoopStats &s) override {
            TestApp::draw(fb, s);
            const std::uint64_t until = hv::monotonic_ns() + 8ull * 1000 * 1000;
            while (hv::monotonic_ns() < until) { /* burn 8 ms */ }
        }
    };

    QuietPipe pipe;
    SlowApp app;
    app.animate = true;

    /* 500 fps is a 2 ms period, so an 8 ms draw overruns every frame. */
    hv::EventLoop loop(base_config(pipe.rd, 500, 4));
    loop.run(app);

    const hv::LoopStats &s = loop.stats();

    check(s.last.draw_ns >= 7ull * 1000 * 1000,
          "timing: the slow phase was attributed to draw");
    check(s.last.drain_ns < 5ull * 1000 * 1000,
          "timing: the fast phases were not blamed for it");
    check(s.last.update_ns < 5ull * 1000 * 1000,
          "timing: update was not blamed for it");
    check(s.last.total_ns >= s.last.draw_ns,
          "timing: the total covers the phases");
    check(s.worst.total_ns >= s.last.draw_ns, "timing: the worst frame is kept");
    check(s.overruns == 4, "timing: every frame was counted as an overrun");
}

void test_stats_reset_starts_a_new_loop_window() {
    check(!hv::quit_requested(), "ordering: the quit flag is still clear");

    class ResettingApp final : public TestApp {
    public:
        unsigned drain() override {
            ++drains;
            return 2;
        }

        bool update(std::uint64_t now) override {
            TestApp::update(now);
            if (updates == 3) reset_pending = true;
            return false;
        }
    };

    QuietPipe pipe;
    ResettingApp app;
    hv::EventLoop loop(base_config(pipe.rd, 500, 4));
    loop.run(app);

    const hv::LoopStats &s = loop.stats();
    check(app.updates == 7,
          "reset: the frame limit became relative to the new window");
    check(s.frames == 4 && s.drawn == 4,
          "reset: frame and draw counters contain only the new window");
    check(s.events == 8,
          "reset: drained-event diagnostics contain only the new window");
    check(!app.reset_pending, "reset: the request was consumed once");
}

/* Keys reach the app, and `q` exits through the same flag the signal handlers
 * set. Runs last: request_quit() is process-global and one-way. */
void test_keys_reach_the_app_and_q_quits() {
    check(!hv::quit_requested(), "ordering: the quit flag is still clear");

    QuietPipe pipe;
    const char input[] = "abq";
    if (::write(pipe.wr, input, 3) != 3) {
        std::fprintf(stderr, "  FAIL setup: could not prime the pipe\n");
        ++g_failures;
        return;
    }

    TestApp app;
    hv::EventLoop loop(base_config(pipe.rd, 60, 50));
    const hv::LoopExit why = loop.run(app);

    check(why == hv::LoopExit::Quit, "keys: q ended the loop");
    check(app.keys == "abq", "keys: every byte reached the app in order");
    check(loop.stats().frames == 1,
          "keys: quit was noticed within the frame it arrived in");
    check(hv::quit_requested(), "keys: q went through the shared quit flag");
}

} // namespace

int main() {
    test_idle_frames_skip_the_draw();
    test_animating_draws_every_frame();
    test_exhausted_stdin_does_not_spin();
    test_resize_repaints_every_cell();
    test_refuses_a_terminal_that_is_too_small();
    test_write_failure_ends_the_loop();
    test_timing_attributes_a_slow_draw();
    test_stats_reset_starts_a_new_loop_window();

    /* Last: sets the process-global quit flag, which nothing clears. */
    test_keys_reach_the_app_and_q_quits();

    if (g_failures != 0) {
        std::fprintf(stderr, "event_loop_test: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("event_loop_test: paced, idle-cheap, and it survives a resize\n");
    return 0;
}
