/* heapviz - the M4.6 frame budget: 200x50, 60 FPS, heavy churn, under 1 ms CPU.
 *
 * Copyright (C) 2026 Parth Sinha
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * M4.1 through M4.5 each made a claim about cost. This is the one place that
 * checks them together, on the shape the roadmap named: a 200x50 terminal, a
 * full-screen heap map, and a target churning hard enough that every frame is a
 * dirty frame.
 *
 * WHY CPU AND NOT WALL CLOCK
 * --------------------------
 * A paced loop hits its deadlines identically whether it slept or spun to get
 * there, so wall time cannot fail this test -- 180 frames at 60 FPS take three
 * seconds no matter how much work each one did. getrusage is what separates
 * "waited" from "worked", and it is the measurement two real bugs in this repo
 * hid from (see CLAUDE.md, "Things that have bitten this codebase"). Budget is
 * CPU-per-drawn-frame, because a frame that skipped its draw did not spend the
 * budget and must not be allowed to average it down.
 *
 * WHY ONE WRITE PER FRAME IS CHECKED AT THE SEAM
 * ----------------------------------------------
 * The roadmap box says `strace -c`. What strace would be looking for is that
 * the byte stream reaches the terminal in one call rather than one per cell, and
 * that decision is made in Renderer::flush -- which takes its write(2) as a
 * parameter. Counting invocations of that parameter counts exactly the syscalls
 * strace would count, without needing ptrace. The caveat strace would also show
 * is kept honest here by reporting bytes per frame: flush retries on a short
 * write, so a real tty that accepts less than a frame's worth costs more than
 * one call, and the number below is what tells you whether that is near.
 *
 * The budget is 1 ms of CPU against a 16.6 ms frame period. That is not a
 * 16x margin for comfort; it is the room the target process needs. heapviz is
 * measuring something, and every microsecond it spends is stolen from the thing
 * under inspection.
 */

#include "tui/demo_heap.h"
#include "tui/event_loop.h"
#include "tui/map_view.h"
#include "tui/renderer.h"
#include "tui/terminal.h"

#include <cstdio>
#include <fcntl.h>
#include <sys/resource.h>
#include <unistd.h>

namespace {

int g_failures = 0;

void check(bool ok, const char *what) {
    if (!ok) { std::fprintf(stderr, "  FAIL %s\n", what); ++g_failures; }
}

/* The geometry M4.6 names. */
constexpr int      kCols   = 200;
constexpr int      kRows   = 50;
constexpr unsigned kFps    = 60;
constexpr unsigned kFrames = 180; /* three seconds at 60 FPS */

/* Allocator traffic per frame. 1200 ops at 60 FPS is 72k events a second, an
 * order of magnitude past what a normal program does and enough that the map's
 * aggregates never settle -- which matters, because the heat ramp's animating
 * path costs ~7x its settled path (M3.4). This measures the expensive one. */
constexpr unsigned kOpsPerFrame = 1200;

bool fixed_size(int, int &w, int &h) {
    w = kCols;
    h = kRows;
    return true;
}

std::uint64_t g_write_calls = 0;
std::uint64_t g_write_bytes = 0;

/* Accepts the whole buffer, as a terminal with room usually does, so one call
 * here means one write(2) there. */
ssize_t counting_write(int, const void *, std::size_t n) {
    ++g_write_calls;
    g_write_bytes += n;
    return static_cast<ssize_t>(n);
}

/* User plus system time for this process, in microseconds. */
std::uint64_t cpu_us() {
    rusage ru{};
    ::getrusage(RUSAGE_SELF, &ru);
    const auto to_us = [](const timeval &tv) -> std::uint64_t {
        return static_cast<std::uint64_t>(tv.tv_sec) * 1000000ull +
               static_cast<std::uint64_t>(tv.tv_usec);
    };
    return to_us(ru.ru_utime) + to_us(ru.ru_stime);
}

/* The real map, the real heat ramp, the real synthetic heap that --term-check
 * drives. Nothing here is a stand-in except the event source. */
class ChurnApp final : public hv::LoopApp {
public:
    ChurnApp() : view_(hv::Capabilities{}) { heap_.seed(2000); }

    void resized(int w, int h) override {
        heap_.fit(hv::Rect{0, 0, w, h});
    }

    bool update(std::uint64_t now_ns) override {
        if (start_ns_ == 0) start_ns_ = now_ns;
        now_ms_ = static_cast<std::uint32_t>((now_ns - start_ns_) / 1000000u);
        return heap_.churn(now_ms_, kOpsPerFrame);
    }

    bool animating() const override { return true; }

    void draw(hv::Framebuffer &fb, const hv::LoopStats &) override {
        view_.draw(fb, hv::Rect{0, 0, fb.width(), fb.height()},
                   heap_.map(), now_ms_);
    }

    const hv::DemoHeap &heap() const noexcept { return heap_; }

private:
    hv::MapView   view_;
    hv::DemoHeap  heap_;
    std::uint64_t start_ns_ = 0;
    std::uint32_t now_ms_   = 0;
};

/* --------------------------------------------------------------------------
 * the run
 * ------------------------------------------------------------------------ */

/* One paced run. Returns CPU microseconds per drawn frame, or -1 on a failure
 * already reported. `verbose` prints the breakdown; the repeats do not. */
double run_the_budget(bool verbose) {
    /* /dev/null as stdin: always ready to read but never delivering, so the
     * loop's poll() behaves as it does against a terminal nobody is typing at
     * rather than spinning on a permanently-readable fd. */
    const int devnull = ::open("/dev/null", O_RDONLY);
    check(devnull >= 0, "budget: /dev/null opened");
    if (devnull < 0) return -1.0;

    hv::LoopConfig cfg;
    cfg.in_fd      = devnull;
    cfg.out_fd     = -1;
    cfg.target_fps = kFps;
    cfg.max_frames = kFrames;
    cfg.size_fn    = fixed_size;
    cfg.writer     = counting_write;

    ChurnApp app;
    hv::EventLoop loop(cfg);

    /* Warm the caches and touch every page of both buffers before the clock
     * starts. First-frame cost is a full repaint plus page faults, which is a
     * real cost but not the steady-state one this budget is about. */
    loop.resize(kCols, kRows);
    app.resized(kCols, kRows);

    g_write_calls = 0;
    g_write_bytes = 0;

    const std::uint64_t wall0 = hv::monotonic_ns();
    const std::uint64_t cpu0  = cpu_us();
    const hv::LoopExit  why   = loop.run(app);
    const std::uint64_t cpu   = cpu_us() - cpu0;
    const std::uint64_t wall  = hv::monotonic_ns() - wall0;

    ::close(devnull);

    const hv::LoopStats &s = loop.stats();

    check(why == hv::LoopExit::FrameLimit, "budget: ran to the frame limit");
    check(s.frames == kFrames, "budget: ran every frame");
    check(s.drawn == kFrames, "budget: heavy churn draws every frame");

    const double cpu_per_frame_us =
        s.drawn ? static_cast<double>(cpu) / static_cast<double>(s.drawn) : 0.0;
    const double wall_ms = static_cast<double>(wall) / 1e6;
    const double duty    = static_cast<double>(cpu) * 100000.0 /
                           static_cast<double>(wall ? wall : 1);

    if (verbose) {
        std::printf("  paced: %dx%d, %u ops/frame, %llu frames\n", kCols, kRows,
                    kOpsPerFrame, static_cast<unsigned long long>(s.frames));
        std::printf("    worst frame's work   %8.1f us   (period %.0f)\n",
                    static_cast<double>(s.worst.total_ns) / 1000.0, 1e6 / kFps);
        std::printf("      drain %.1f  update %.1f  draw %.1f  diff %.1f"
                    "  write %.1f (us)\n",
                    static_cast<double>(s.worst.drain_ns) / 1000.0,
                    static_cast<double>(s.worst.update_ns) / 1000.0,
                    static_cast<double>(s.worst.draw_ns) / 1000.0,
                    static_cast<double>(s.worst.diff_ns) / 1000.0,
                    static_cast<double>(s.worst.write_ns) / 1000.0);
        std::printf("    duty cycle           %8.2f %% of one core\n", duty);
        std::printf("    fps %.1f over %.0f ms, %llu overruns\n", s.fps, wall_ms,
                    static_cast<unsigned long long>(s.overruns));
        std::printf("    writes %llu, %.0f bytes/frame, %llu live chunks\n",
                    static_cast<unsigned long long>(g_write_calls),
                    s.drawn ? static_cast<double>(g_write_bytes) /
                              static_cast<double>(s.drawn) : 0.0,
                    static_cast<unsigned long long>(app.heap().live_count()));
    }

    /* M4.6 box 2. One call per drawn frame, and the count is exact: a stray
     * per-row or per-cell write would show up as a multiple of 50 or 10000. */
    check(g_write_calls == s.drawn, "budget: exactly one write per drawn frame");
    check(s.writes == s.drawn, "budget: the loop agrees about its own writes");

    /* The pacing still held while all this was going on. Not the same claim as
     * the budget: a loop can be cheap per frame and still run too fast. */
    check(wall_ms > static_cast<double>(kFrames) * 1000.0 / kFps * 0.9,
          "budget: pacing was not overrun");

    /* Overruns are frames whose *work* missed the 16.6 ms deadline. At a few
     * hundred microseconds of work there is 50x of headroom, so a handful means
     * the machine was busy, not that heapviz regressed; a flood means the frame
     * path got expensive. 5% is the line between those two readings. */
    check(s.overruns * 20 <= s.frames, "budget: deadlines met");

    return cpu_per_frame_us;
}

/* M4.6 box 1, best of several runs.
 *
 * Best-of rather than mean, because this is a ceiling and the thing being
 * ceilinged is heapviz, not the machine. Every source of noise here is additive
 * -- another process taking the core, a migration, the caches going cold during
 * the 16 ms this loop spends asleep -- so the cheapest run is the one least
 * contaminated by things that are not heapviz. A regression, by contrast, is
 * present in every run and moves the minimum too.
 *
 * The spread is worth reading, not just the minimum: on an idle machine these
 * land within about 30% of each other, and a wider spread than that means the
 * number below is describing whatever else was running. */
void run_the_budget_best_of(int runs) {
    double best  = 0.0;
    double worst = 0.0;

    for (int i = 0; i < runs; ++i) {
        const double us = run_the_budget(i == 0);
        if (us < 0.0) return;
        std::printf("    run %d  CPU/drawn frame  %8.1f us\n", i + 1, us);
        if (i == 0 || us < best)  best  = us;
        if (i == 0 || us > worst) worst = us;
    }

    std::printf("    best %.1f us, worst %.1f us   (budget 1000)\n", best, worst);
    check(best < 1000.0, "budget: under 1 ms CPU per drawn frame");
}

/* The same work as one loop iteration, unpaced, so the split between phases is
 * a mean over many frames rather than the single worst one LoopStats keeps.
 * `worst` is the right thing for the overlay -- it is what missed a deadline --
 * but it is the wrong thing for deciding which phase to go and look at. */
void run_the_phases_alone() {
    hv::Framebuffer fb;
    check(fb.resize(kCols, kRows), "phases: framebuffer sized");

    hv::Renderer r;
    r.reserve(kCols, kRows);

    hv::MapView  view{hv::Capabilities{}};
    hv::DemoHeap heap;
    heap.fit(hv::Rect{0, 0, kCols, kRows});
    heap.seed(2000);

    const hv::Rect area{0, 0, kCols, kRows};
    constexpr int  kReps   = 300;
    constexpr int  kRounds = 3; /* best-of, for the same reason as the paced run */

    /* Warm, and get the front buffer describing a real frame rather than a
     * blank one, so the first measured diff is a diff and not a repaint. */
    view.draw(fb, area, heap.map(), 0);
    r.render(fb);
    fb.swap();

    std::uint64_t best_update = 0, best_draw = 0, best_diff = 0;
    std::uint64_t bytes = 0;

    for (int round = 0; round < kRounds; ++round) {
        std::uint64_t update_ns = 0, draw_ns = 0, diff_ns = 0;
        bytes = 0;

        for (int i = 0; i < kReps; ++i) {
            const auto now = static_cast<std::uint32_t>(round * kReps + i) *
                             1000u / kFps;

            std::uint64_t t = hv::monotonic_ns();
            heap.churn(now, kOpsPerFrame);
            update_ns += hv::monotonic_ns() - t;

            t = hv::monotonic_ns();
            view.draw(fb, area, heap.map(), now);
            draw_ns += hv::monotonic_ns() - t;

            t = hv::monotonic_ns();
            bytes += r.render(fb);
            diff_ns += hv::monotonic_ns() - t;

            fb.swap();
        }

        /* Each phase's own minimum, not the minimum round: the phases are
         * independent, and a round that was slow overall may still hold the
         * cleanest reading for one of them. */
        if (round == 0 || update_ns < best_update) best_update = update_ns;
        if (round == 0 || draw_ns   < best_draw)   best_draw   = draw_ns;
        if (round == 0 || diff_ns   < best_diff)   best_diff   = diff_ns;
    }

    const auto per = [](std::uint64_t total) {
        return static_cast<double>(total) / kReps / 1000.0;
    };

    std::printf("  phases, best of %d rounds of %d unpaced frames (%d cells)\n",
                kRounds, kReps, kCols * kRows);
    std::printf("    update %6.1f  draw %6.1f  diff %6.1f  = %6.1f us,"
                " %.0f bytes\n",
                per(best_update), per(best_draw), per(best_diff),
                per(best_update + best_draw + best_diff),
                static_cast<double>(bytes) / kReps);

    /* Generous on purpose: a smoke alarm for an accidental O(n^2) or a
     * per-cell allocation, not a second budget. The budget is the CPU figure
     * from the paced run, which includes all of this. */
    check(per(best_update + best_draw + best_diff) < 2000.0,
          "phases: a full frame's work is under 2 ms");
}

} // namespace

int main() {
    std::printf("frame budget (M4.6)\n");
    run_the_phases_alone();
    run_the_budget_best_of(5);

    if (g_failures) {
        std::fprintf(stderr, "\n%d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("  ok\n");
    return 0;
}
