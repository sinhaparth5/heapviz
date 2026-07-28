/* heapviz - single-threaded event loop and frame pacing (M4.5).
 *
 * Copyright (C) 2026 Parth Sinha
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "tui/event_loop.h"

#include "tui/terminal.h"

#include <cerrno>
#include <ctime>
#include <poll.h>
#include <sys/ioctl.h>
#include <unistd.h>

namespace hv {

namespace {

constexpr std::uint64_t kNsPerSec = 1000ull * 1000ull * 1000ull;

/* How much of stdin is taken in one go. Any terminal escape sequence fits many
 * times over, and a paste larger than this simply arrives across two frames. */
constexpr std::size_t kInputChunk = 64;

} // namespace

std::uint64_t monotonic_ns() noexcept {
    timespec ts{};
    ::clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<std::uint64_t>(ts.tv_sec) * kNsPerSec +
           static_cast<std::uint64_t>(ts.tv_nsec);
}

bool terminal_size(int fd, int &w, int &h) noexcept {
    winsize ws{};
    if (::ioctl(fd, TIOCGWINSZ, &ws) != 0) return false;
    /* A pty that has never been sized reports 0x0. Treating that as a real
     * measurement would resize the framebuffer to nothing. */
    if (ws.ws_col == 0 || ws.ws_row == 0) return false;
    w = static_cast<int>(ws.ws_col);
    h = static_cast<int>(ws.ws_row);
    return true;
}

const char *loop_exit_str(LoopExit e) noexcept {
    switch (e) {
    case LoopExit::Quit:         return "quit";
    case LoopExit::FrameLimit:   return "frame limit reached";
    case LoopExit::TooSmall:     return "terminal is too small";
    case LoopExit::WriteFailed:  return "the terminal went away";
    case LoopExit::ResizeFailed: return "could not allocate for the new size";
    }
    return "unknown";
}

/* ------------------------------------------------------------------ */
/* LoopApp                                                             */
/* ------------------------------------------------------------------ */

LoopApp::~LoopApp() = default;

unsigned LoopApp::drain() { return 0; }
bool     LoopApp::update(std::uint64_t) { return false; }
bool     LoopApp::key(char) { return false; }
bool     LoopApp::animating() const { return false; }
void     LoopApp::resized(int, int) {}

/* ------------------------------------------------------------------ */
/* EventLoop                                                           */
/* ------------------------------------------------------------------ */

EventLoop::EventLoop(const LoopConfig &cfg) noexcept : cfg_(cfg) {
    const unsigned fps = cfg_.target_fps == 0 ? 1u : cfg_.target_fps;
    period_ns_ = kNsPerSec / fps;
    r_.set_color_mode(cfg_.color);
}

bool EventLoop::resize(int w, int h) {
    if (!fb_.resize(w, h)) return false;
    r_.reserve(w, h);
    return true;
}

bool EventLoop::wait_for_deadline(LoopApp &app, std::uint64_t deadline,
                                  bool &dirty) {
    for (;;) {
        if (quit_requested()) { exit_ = LoopExit::Quit; return false; }

        const std::uint64_t now = monotonic_ns();
        if (now >= deadline) return true;
        const std::uint64_t left = deadline - now;

        if (!input_open_) {
            /* poll() on a closed or exhausted stdin returns immediately and
             * forever, so polling it would turn the idle case -- the one this
             * loop exists to make cheap -- into a spin at 100% of a core.
             * Sleep out the frame instead. A signal cuts the sleep short and is
             * picked up at the top of the next frame. */
            const timespec ts{static_cast<time_t>(left / kNsPerSec),
                              static_cast<long>(left % kNsPerSec)};
            ::nanosleep(&ts, nullptr);
            return true;
        }

        /* Rounded up. A timeout of 0 would return instantly and busy-wait out
         * the sub-millisecond remainder of the frame. */
        const int timeout_ms =
            static_cast<int>((left + 999999ull) / (1000ull * 1000ull));

        pollfd p{};
        p.fd     = cfg_.in_fd;
        p.events = POLLIN;

        const int rc = ::poll(&p, 1, timeout_ms);
        if (rc < 0) {
            if (errno == EINTR) {
                /* SIGWINCH, or a graceful signal. poll(2) is never restarted,
                 * which is what we want: end the wait so the top of the next
                 * frame sees the flag immediately. */
                return true;
            }
            input_open_ = false;
            continue;
        }
        if (rc == 0) return true; /* the deadline, the common case */

        if ((p.revents & POLLIN) != 0) {
            char          buf[kInputChunk];
            const ssize_t n = ::read(cfg_.in_fd, buf, sizeof buf);
            if (n > 0) {
                for (ssize_t i = 0; i < n; ++i) {
                    if (app.key(buf[i])) dirty = true;
                }
                /* `q` reaches quit through request_quit(), the same flag the
                 * signal handlers set, so there is one exit condition rather
                 * than two that can disagree. */
                if (quit_requested()) { exit_ = LoopExit::Quit; return false; }
            } else if (n == 0) {
                input_open_ = false; /* EOF */
            } else if (errno != EINTR && errno != EAGAIN) {
                input_open_ = false;
            }
        } else if ((p.revents & (POLLHUP | POLLERR | POLLNVAL)) != 0) {
            input_open_ = false;
        }
    }
}

LoopExit EventLoop::run(LoopApp &app) {
    const SizeFn measure = cfg_.size_fn != nullptr ? cfg_.size_fn : terminal_size;

    /* The first frame measures the terminal through the same path a SIGWINCH
     * takes, so there is one sizing code path rather than two. */
    request_resize();

    bool dirty        = true;
    bool full_repaint = true;

    fps_mark_ns_    = monotonic_ns();
    fps_mark_drawn_ = 0;

    for (;;) {
        const std::uint64_t frame_start = monotonic_ns();
        const std::uint64_t deadline    = frame_start + period_ns_;

        if (quit_requested()) return LoopExit::Quit;

        /* Checked before the increment, so the count reflects frames that ran
         * rather than including the one that noticed the limit. */
        if (cfg_.max_frames != 0 && stats_.frames >= cfg_.max_frames) {
            return LoopExit::FrameLimit;
        }
        ++stats_.frames;

        /* --- resize ---------------------------------------------------- */
        /* This is the defined point. Buffers are only ever reallocated here,
         * between one completed frame and the next, never while a frame is
         * being drawn or written. A SIGWINCH arriving mid-frame sets a flag and
         * waits its turn, which is what keeps a resize from tearing. */
        if (take_resize_request()) {
            /* Seeded with the current size, so a failed measurement leaves the
             * loop drawing at the size that was working rather than at zero. */
            int w = fb_.width();
            int h = fb_.height();
            if (!measure(cfg_.out_fd, w, h) && fb_.empty()) {
                /* Failed, and there is no previous size to keep. 80x24 is what
                 * every terminal claims to be when it cannot say. */
                w = 80;
                h = 24;
            }
            if (w < cfg_.min_width || h < cfg_.min_height) {
                return LoopExit::TooSmall;
            }
            if (w != fb_.width() || h != fb_.height()) {
                if (!resize(w, h)) return LoopExit::ResizeFailed;
                app.resized(w, h);
                ++stats_.repaints;
                /* resize() cleared the front buffer, so it no longer describes
                 * what is on screen; every cell has to go out. */
                full_repaint = true;
                dirty        = true;
            }
        }

        /* --- drain ----------------------------------------------------- */
        const unsigned n  = app.drain();
        const std::uint64_t t_drained = monotonic_ns();
        stats_.events += n;
        if (n != 0) dirty = true;

        /* --- update ---------------------------------------------------- */
        if (app.update(t_drained)) dirty = true;
        const std::uint64_t t_updated = monotonic_ns();
        if (app.animating()) dirty = true;

        FrameTiming t{};
        t.drain_ns  = t_drained - frame_start;
        t.update_ns = t_updated - t_drained;

        /* --- draw, diff, write ----------------------------------------- */
        /* Skipping all three is the whole idle story. Nothing arrived, no key
         * was pressed, nothing is animating: the screen is already correct, so
         * the cheapest correct frame is no frame. */
        if (dirty) {
            app.draw(fb_, stats_);
            const std::uint64_t t_drawn = monotonic_ns();

            const std::size_t bytes = r_.render(fb_, full_repaint);
            const std::uint64_t t_diffed = monotonic_ns();

            if (bytes != 0) {
                if (!r_.flush(cfg_.out_fd, cfg_.writer)) {
                    return LoopExit::WriteFailed;
                }
                ++stats_.writes;
                stats_.bytes_written += bytes;
            }
            const std::uint64_t t_written = monotonic_ns();

            fb_.swap();
            ++stats_.drawn;
            dirty        = false;
            full_repaint = false;

            t.draw_ns  = t_drawn - t_updated;
            t.diff_ns  = t_diffed - t_drawn;
            t.write_ns = t_written - t_diffed;
        } else {
            ++stats_.skipped;
        }

        t.total_ns  = monotonic_ns() - frame_start;
        stats_.last = t;
        if (t.total_ns > stats_.worst.total_ns) stats_.worst = t;
        /* An overrun means the work outran the frame, so the deadline has
         * already passed and the wait below is a no-op. Counting them is how
         * M4.6's "under 1 ms per frame" claim gets checked in the field rather
         * than only on the machine it was measured on. */
        if (t.total_ns > period_ns_) ++stats_.overruns;

        /* --- fps, over a one-second window ----------------------------- */
        const std::uint64_t since_mark = t_updated - fps_mark_ns_;
        if (since_mark >= kNsPerSec) {
            stats_.fps = static_cast<double>(stats_.drawn - fps_mark_drawn_) *
                         static_cast<double>(kNsPerSec) /
                         static_cast<double>(since_mark);
            fps_mark_ns_    = t_updated;
            fps_mark_drawn_ = stats_.drawn;
        }

        /* --- wait ------------------------------------------------------ */
        if (!wait_for_deadline(app, deadline, dirty)) return exit_;
    }
}

} // namespace hv
