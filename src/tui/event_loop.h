/* heapviz - single-threaded event loop and frame pacing (M4.5).
 *
 * Copyright (C) 2026 Parth Sinha
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * One thread does everything: drain the ring, fold events into the model, draw,
 * diff, write, then wait out the rest of the frame. There is no render thread
 * and no lock, which is what lets the ring stay MPSC with a single consumer and
 * what makes a frame's cost a number you can read off a clock rather than a
 * distribution over two schedulers.
 *
 * THE TWO PROPERTIES THAT MATTER
 * ------------------------------
 * 1. Bounded frame rate. Work is paced to a deadline, so a target allocating at
 *    ten million events a second cannot make the TUI redraw ten million times.
 *
 * 2. Idle costs nothing. The wait is a poll(2) on stdin with a timeout, not a
 *    sleep, so a keystroke is picked up within a frame; and a frame in which
 *    nothing changed skips draw, diff and write entirely, so it costs one
 *    poll() and a couple of atomic loads.
 *
 * There is deliberately no idle back-off (stretching the frame period after a
 * quiet spell). A wakeup that skips the draw is a few microseconds of work, so
 * 60 of them a second is already under a tenth of a percent of a core; backing
 * off would trade first-event latency for a saving too small to measure.
 */

#ifndef HEAPVIZ_TUI_EVENT_LOOP_H
#define HEAPVIZ_TUI_EVENT_LOOP_H

#include "tui/framebuffer.h"
#include "tui/renderer.h"

#include <cstdint>

namespace hv {

/* CLOCK_MONOTONIC in nanoseconds. Monotonic, not realtime: a frame deadline
 * must not move because someone stepped the clock or NTP slewed it. */
std::uint64_t monotonic_ns() noexcept;

/* Where one frame's time went. Each phase is measured separately because they
 * fail differently: a slow drain means the ring is backed up, a slow draw means
 * the model got expensive, a slow write means the terminal is the bottleneck.
 * A single "frame time" number cannot tell those apart. */
struct FrameTiming {
    std::uint64_t drain_ns  = 0; /* pulling events out of the ring      */
    std::uint64_t update_ns = 0; /* folding them into the model         */
    std::uint64_t draw_ns   = 0; /* painting the back buffer            */
    std::uint64_t diff_ns   = 0; /* back vs front, into the byte stream */
    std::uint64_t write_ns  = 0; /* the one write(2)                    */
    std::uint64_t total_ns  = 0; /* all of the above, excluding the wait */
};

struct LoopStats {
    std::uint64_t frames        = 0; /* iterations of the loop            */
    std::uint64_t drawn         = 0; /* of those, ones that painted       */
    std::uint64_t skipped       = 0; /* frames + skipped == drawn + ...   */
    std::uint64_t repaints      = 0; /* full repaints, i.e. resizes       */
    std::uint64_t writes        = 0; /* write(2) calls; <= drawn          */
    std::uint64_t bytes_written = 0;
    std::uint64_t events        = 0; /* events drained across the run     */
    std::uint64_t overruns      = 0; /* frames whose work missed the deadline */

    FrameTiming last{};
    FrameTiming worst{};

    /* Frames actually painted per second, averaged over the last second. Zero
     * until the first second has elapsed. An idle heapviz reads 0 fps, which is
     * the honest number: it is not drawing. */
    double fps = 0.0;
};

/* What the loop drives. Split this way so the loop can be tested against a fake
 * application with no ring, no terminal and no clock of its own; M3 supplies
 * the real one.
 *
 * Every method except draw() has a do-nothing default, so a test app overrides
 * only the part it is about.
 */
class LoopApp {
public:
    virtual ~LoopApp();

    /* Pull whatever is waiting out of the ring. Returns how many events were
     * taken; a non-zero count marks the frame dirty. */
    virtual unsigned drain();

    /* Fold drained events into the model. Returns true if anything the user
     * could see changed. */
    virtual bool update(std::uint64_t now_ns);

    /* One byte of input. Escape-sequence decoding is M5's problem; this is the
     * raw stream. Returns true if the model changed. */
    virtual bool key(char byte);

    /* True when the frame must be redrawn even though no event and no key
     * arrived -- a blinking cursor, a decaying heatmap. */
    virtual bool animating() const;

    /* The framebuffer has already been resized to w x h and the next frame is
     * already a full repaint; this is for model state that tracks geometry. */
    virtual void resized(int w, int h);

    /* Paint the back buffer. Called only on frames that will actually be
     * drawn. */
    virtual void draw(Framebuffer &fb, const LoopStats &stats) = 0;
};

/* How the loop measures the terminal. Returns false to leave the size alone,
 * which is what a failed ioctl means: keep drawing at the size that worked. */
using SizeFn = bool (*)(int fd, int &w, int &h);

/* ioctl(TIOCGWINSZ), the default. Exposed so a caller can size a framebuffer
 * before the loop starts. */
bool terminal_size(int fd, int &w, int &h) noexcept;

struct LoopConfig {
    int in_fd  = 0; /* STDIN_FILENO  */
    int out_fd = 1; /* STDOUT_FILENO */

    /* 60 is the ceiling, not the target: the loop draws at most this often and
     * usually less, because idle frames skip the draw. */
    unsigned target_fps = 60;

    /* Minimum usable geometry. Below this the loop stops rather than rendering
     * a heap map into six columns. */
    int min_width  = 20;
    int min_height = 6;

    /* --- test seams. All default to the production path. --- */

    /* Stop after this many iterations. Zero means run until quit. */
    std::uint64_t max_frames = 0;

    /* Substitute for ioctl(TIOCGWINSZ) and for write(2). */
    SizeFn            size_fn = nullptr;
    Renderer::WriteFn writer  = nullptr;
};

/* stdin reaching EOF is deliberately not in here. `heapviz --pid N </dev/null`
 * is a reasonable thing to run, and the events it displays come from the ring,
 * not the keyboard. The loop stops polling a dead stdin and carries on. */
enum class LoopExit {
    Quit,         /* q, SIGINT, SIGTERM, SIGHUP                   */
    FrameLimit,   /* max_frames reached (tests only)              */
    TooSmall,     /* the terminal shrank below the usable minimum */
    WriteFailed,  /* the terminal went away mid-frame             */
    ResizeFailed, /* the new geometry could not be allocated      */
};

const char *loop_exit_str(LoopExit e) noexcept;

class EventLoop {
public:
    explicit EventLoop(const LoopConfig &cfg) noexcept;

    /* Sizes the framebuffer and the renderer for w x h. Called by run() on the
     * first frame and again on every SIGWINCH; public so a caller can pre-size
     * before the terminal is even entered. */
    bool resize(int w, int h);

    LoopExit run(LoopApp &app);

    const LoopStats &stats() const noexcept { return stats_; }
    Framebuffer     &framebuffer() noexcept { return fb_; }

    /* cells_emitted() / cells_examined() from the last frame, for the timing
     * overlay and for the tests that check a resize really did repaint every
     * cell rather than only the ones that differ from a blank buffer. */
    const Renderer &renderer() const noexcept { return r_; }

private:
    /* Blocks until the frame deadline, servicing input. Returns false when the
     * loop should stop. */
    bool wait_for_deadline(LoopApp &app, std::uint64_t deadline, bool &dirty);

    LoopConfig  cfg_;
    Framebuffer fb_;
    Renderer    r_;
    LoopStats   stats_{};

    std::uint64_t period_ns_ = 0;

    /* fps is a rate, so it needs a window: frames drawn since this mark. */
    std::uint64_t fps_mark_ns_    = 0;
    std::uint64_t fps_mark_drawn_ = 0;

    bool     input_open_ = true;
    LoopExit exit_       = LoopExit::Quit;
};

} // namespace hv

#endif /* HEAPVIZ_TUI_EVENT_LOOP_H */
