/* heapviz - what a rapid resize puts on the wire (M4.6).
 *
 * Copyright (C) 2026 Parth Sinha
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The M4.6 box reads "no tearing, no flicker, no cursor artefacts during rapid
 * resize", and the last word on that is a human dragging a window --
 * `heapviz --term-check` exists for exactly that. But three of the four things
 * that make a resize *look* wrong are properties of the byte stream, and a byte
 * stream can be read by a test:
 *
 *   flicker          the screen being cleared between frames, so the terminal
 *                    shows blank for one refresh. ESC[2J belongs to entering
 *                    the alternate screen and must appear once, ever.
 *   cursor artefact  the cursor becoming visible and skating around the map as
 *                    cells are painted. It is hidden on entry and shown on
 *                    exit, once each, and never in between.
 *   tearing          a frame painted at one geometry landing on a terminal that
 *                    is now another. It shows up as cursor addressing outside
 *                    the grid, which wraps and shears the display, so no
 *                    ESC[row;colH may name a cell beyond the largest size the
 *                    terminal was ever given.
 *
 * What is left over for the human is whether the *result* looks right, which is
 * a judgement about pixels rather than about bytes.
 *
 * The invariant underneath all of this is ground rule #6: buffers are
 * reallocated at exactly one point, the top of a frame, before the drain. A
 * SIGWINCH lands whenever the kernel likes -- mid-draw, mid-diff, inside the
 * write(2) -- and this test is a machine arriving at the worst moment several
 * hundred times in a row. Unlike event_loop_pty_test, the child here writes its
 * frames to the pty for real, because a discarded frame proves nothing about
 * what a resize does to the wire.
 */

#include "tui/demo_heap.h"
#include "tui/event_loop.h"
#include "tui/map_view.h"
#include "tui/terminal.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <csignal>
#include <ctime>
#include <poll.h>
#include <pty.h>
#include <string>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

int g_failures = 0;

void check(bool ok, const char *what) {
    if (!ok) { std::fprintf(stderr, "  FAIL %s\n", what); ++g_failures; }
}

/* Sizes to cycle through. They straddle the loop's usable minimum in both
 * directions and change by a lot rather than by a column, because a resize that
 * only grows is the easy half: shrinking is what leaves the old frame's
 * bottom-right on screen if a buffer is reused at the wrong moment. */
struct Size { int w, h; };
constexpr Size kSizes[] = {
    {80, 24}, {200, 50}, {40, 10}, {132, 43}, {24, 8},
    {160, 45}, {60, 20}, {100, 30}, {30, 9}, {180, 48},
};
constexpr int kSizeCount = static_cast<int>(sizeof kSizes / sizeof kSizes[0]);

constexpr int kMaxCols = 200;
constexpr int kMaxRows = 50;
constexpr int kStorms     = 300; /* resizes to send */
constexpr int kStormGapMs =   8; /* twice a frame period, so signals land mid-work */
constexpr int kSettleMs   = 300; /* before the first one */

/* ------------------------------------------------------------------ */
/* Child                                                               */
/* ------------------------------------------------------------------ */

/* The real map on the real loop. Always animating, so every frame is a drawn
 * frame and the resize always lands on a loop that is doing something. */
class StormApp final : public hv::LoopApp {
public:
    int  resizes  = 0;
    bool oversize = false; /* a frame was drawn larger than the terminal */

    StormApp() : view_(hv::Capabilities{}) { heap_.seed(800); }

    void resized(int w, int h) override {
        w_ = w;
        h_ = h;
        ++resizes;
        heap_.fit(hv::Rect{0, 0, w, h});
    }

    bool update(std::uint64_t now_ns) override {
        if (start_ns_ == 0) start_ns_ = now_ns;
        now_ms_ = static_cast<std::uint32_t>((now_ns - start_ns_) / 1000000u);
        return heap_.churn(now_ms_, 200);
    }

    bool animating() const override { return true; }

    /* Quitting is the application's decision, not the loop's, so this has to be
     * here for the parent's 'q' to mean anything. */
    bool key(char c) override {
        if (c == 'q') { hv::request_quit(); return false; }
        return false;
    }

    void draw(hv::Framebuffer &fb, const hv::LoopStats &) override {
        /* The framebuffer is the loop's record of the geometry it last
         * measured; resized() is the app's. If they ever disagree, a frame is
         * being painted at a size nobody asked for. */
        if (fb.width() != w_ || fb.height() != h_) oversize = true;
        view_.draw(fb, hv::Rect{0, 0, fb.width(), fb.height()},
                   heap_.map(), now_ms_);
    }

private:
    hv::MapView   view_;
    hv::DemoHeap  heap_;
    std::uint64_t start_ns_ = 0;
    std::uint32_t now_ms_   = 0;
    int           w_ = 0, h_ = 0;
};

[[noreturn]] void child_body(int report_fd) {
    hv::TerminalGuard guard;
    if (guard.enter(STDOUT_FILENO) != hv::TermStatus::Ok) _exit(20);

    hv::LoopConfig cfg;
    cfg.in_fd      = STDIN_FILENO;
    cfg.out_fd     = STDOUT_FILENO;
    cfg.target_fps = 60;
    cfg.max_frames = 1200; /* 20 s ceiling; the parent quits it well before */

    /* The loop's own floor, left at its default. The parent deliberately drives
     * the terminal below the 80x24 startup minimum, which must not stop a
     * running session -- only breaching this does. */

    StormApp      app;
    hv::EventLoop loop(cfg);
    const hv::LoopExit why = loop.run(app);

    guard.restore();

    char      buf[256];
    const int n = std::snprintf(buf, sizeof buf, "%d %d %llu %llu %llu %d\n",
                                app.resizes, app.oversize ? 1 : 0,
                                static_cast<unsigned long long>(loop.stats().frames),
                                static_cast<unsigned long long>(loop.stats().repaints),
                                static_cast<unsigned long long>(loop.stats().bytes_written),
                                static_cast<int>(why));
    if (n > 0) {
        const ssize_t w = ::write(report_fd, buf, static_cast<std::size_t>(n));
        (void)w;
    }
    _exit(0);
}

/* ------------------------------------------------------------------ */
/* Parent                                                              */
/* ------------------------------------------------------------------ */

/* Reads the master until the child reports or the budget runs out, resizing as
 * it goes. Draining is not optional: the child writes real frames now, and a
 * full pty buffer would block it inside write(2) and turn this into a hang.
 *
 * The whole stream is kept -- a few MB -- because the assertions are about what
 * never appears in it, and a sampled stream cannot answer "never". */
std::string drive(int master, int report_fd, std::string &stream) {
    std::string line;
    int         storms    = 0;
    bool        quit_sent = false;

    /* Real elapsed time, not a count of poll() returns: the master is readable
     * almost continuously here, so poll comes straight back and a loop that
     * counted its own timeout would send the whole storm inside a microsecond
     * and the child would see two size changes rather than two hundred. */
    const std::uint64_t t0 = hv::monotonic_ns();
    const auto elapsed_ms  = [t0]() {
        return static_cast<int>((hv::monotonic_ns() - t0) / 1000000ull);
    };

    int next_storm_ms = kSettleMs;

    while (elapsed_ms() < 25000) {
        pollfd fds[2] = {};
        fds[0].fd = master;    fds[0].events = POLLIN;
        fds[1].fd = report_fd; fds[1].events = POLLIN;

        const int rc = ::poll(fds, 2, 5);
        if (rc < 0) {
            if (errno == EINTR) continue;
            break;
        }

        if ((fds[0].revents & POLLIN) != 0) {
            char          scratch[65536];
            const ssize_t got = ::read(master, scratch, sizeof scratch);
            if (got > 0) stream.append(scratch, static_cast<std::size_t>(got));
        }
        if ((fds[1].revents & POLLIN) != 0) {
            char          buf[256];
            const ssize_t n = ::read(report_fd, buf, sizeof buf);
            if (n <= 0) break;
            line.append(buf, static_cast<std::size_t>(n));
            if (line.find('\n') != std::string::npos) {
                /* Terminal restore precedes the report write in the child, but
                 * the pipe and pty are independent fds. Under instrumentation
                 * the pipe can become readable before the pty's final cursor-
                 * show bytes are scheduled to us. Drain that already-written
                 * tail before inspecting the stream. */
                for (int tail = 0; tail < 10; ++tail) {
                    pollfd pfd{master, POLLIN, 0};
                    if (::poll(&pfd, 1, 10) <= 0) break;
                    char scratch[4096];
                    const ssize_t got = ::read(master, scratch, sizeof scratch);
                    if (got > 0)
                        stream.append(scratch, static_cast<std::size_t>(got));
                    else
                        break;
                }
                return line;
            }
        }
        if ((fds[1].revents & (POLLHUP | POLLERR)) != 0 && line.empty()) break;

        /* Settle first: resizing before the guard has installed its handler
         * tests the loop's startup measurement, not SIGWINCH. */
        const int now = elapsed_ms();
        if (now < next_storm_ms) continue;

        if (storms < kStorms) {
            winsize ws{};
            ws.ws_col = static_cast<unsigned short>(kSizes[storms % kSizeCount].w);
            ws.ws_row = static_cast<unsigned short>(kSizes[storms % kSizeCount].h);
            if (::ioctl(master, TIOCSWINSZ, &ws) != 0) {
                std::perror("TIOCSWINSZ");
                ++g_failures;
                storms = kStorms;
            }
            ++storms;
            /* Faster than the frame period on purpose, so signals land inside
             * the draw and inside the write, not only inside the poll. */
            next_storm_ms = now + kStormGapMs;
        } else if (!quit_sent) {
            /* Back to something ordinary, then quit, so the last frames are
             * drawn at a size the loop is happy with. */
            winsize ws{};
            ws.ws_col = 100;
            ws.ws_row = 30;
            (void)::ioctl(master, TIOCSWINSZ, &ws);
            const ssize_t w = ::write(master, "q", 1);
            (void)w;
            quit_sent     = true;
            next_storm_ms = now + 5000; /* nothing left to do but drain */
        }
    }
    return line;
}

/* --- reading the stream ------------------------------------------------- */

std::size_t count_of(const std::string &s, const char *needle) {
    std::size_t n = 0;
    for (std::size_t i = s.find(needle); i != std::string::npos;
         i = s.find(needle, i + 1))
        ++n;
    return n;
}

/* The furthest cell any ESC[row;colH in the stream addressed. Rows and columns
 * are 1-based on the wire. A bare ESC[H (home) is 1;1 and is not interesting.
 *
 * This deliberately does not try to parse every SGR sequence around it: a
 * malformed CUP would simply not match, and the failure this is looking for --
 * a frame addressing row 50 of a 10-row terminal -- produces well-formed
 * sequences with wrong numbers in them. */
void furthest_addressed(const std::string &s, int &max_row, int &max_col) {
    max_row = 0;
    max_col = 0;
    for (std::size_t i = s.find("\033["); i != std::string::npos;
         i = s.find("\033[", i + 1)) {
        std::size_t j = i + 2;
        int row = 0, col = 0;
        bool have_row = false;
        while (j < s.size() && s[j] >= '0' && s[j] <= '9') {
            row = row * 10 + (s[j] - '0');
            ++j;
            have_row = true;
        }
        if (!have_row || j >= s.size() || s[j] != ';') continue;
        ++j;
        bool have_col = false;
        while (j < s.size() && s[j] >= '0' && s[j] <= '9') {
            col = col * 10 + (s[j] - '0');
            ++j;
            have_col = true;
        }
        if (!have_col || j >= s.size() || s[j] != 'H') continue;
        if (row > max_row) max_row = row;
        if (col > max_col) max_col = col;
    }
}

} // namespace

int main() {
    int report[2];
    if (::pipe(report) != 0) { std::perror("pipe"); return 1; }

    winsize ws{};
    ws.ws_col = kSizes[0].w;
    ws.ws_row = kSizes[0].h;

    int         master = -1;
    const pid_t pid    = ::forkpty(&master, nullptr, nullptr, &ws);
    if (pid < 0) { std::perror("forkpty"); return 1; }

    if (pid == 0) {
        ::close(report[0]);
        child_body(report[1]);
    }
    ::close(report[1]);

    std::string stream;
    stream.reserve(8u << 20);
    const std::string report_line = drive(master, report[0], stream);

    /* A child that never reported is a child that is stuck, most likely
     * blocked in write(2) on a pty nobody is draining any more. Kill it rather
     * than waiting on it, so the failure is a failure and not a hang. */
    if (report_line.empty()) ::kill(pid, SIGKILL);

    int status = 0;
    ::waitpid(pid, &status, 0);
    ::close(report[0]);
    ::close(master);

    if (report_line.empty()) {
        std::fprintf(stderr, "  FAIL the child never reported (status %d, "
                             "%zu bytes seen)\n", status, stream.size());
        return 1;
    }



    int                resizes = 0, oversize = 1, why = -1;
    unsigned long long frames = 0, repaints = 0, bytes = 0;
    if (std::sscanf(report_line.c_str(), "%d %d %llu %llu %llu %d",
                    &resizes, &oversize, &frames, &repaints, &bytes, &why) != 6) {
        std::fprintf(stderr, "  FAIL unparseable report: %s\n",
                     report_line.c_str());
        return 1;
    }

    int max_row = 0, max_col = 0;
    furthest_addressed(stream, max_row, max_col);

    std::printf("  %d resizes over %llu frames, %llu repaints, %llu bytes\n",
                resizes, frames, repaints, bytes);
    std::printf("  furthest cell addressed: row %d, col %d (terminal never "
                "exceeded %dx%d)\n", max_row, max_col, kMaxCols, kMaxRows);

    /* Something actually happened. Without this the checks below all pass
     * vacuously on a child that died before its first frame. */
    check(resizes >= 10, "the loop saw the storm");
    check(frames > 50, "the loop kept drawing through it");
    check(bytes > 0, "frames reached the pty");

    /* No tearing: nothing was ever addressed off the largest grid there has
     * been. A frame drawn against a stale framebuffer after a shrink is what
     * this catches, and it is the visible half of ground rule #6. */
    check(max_row <= kMaxRows, "no frame addressed a row past the terminal");
    check(max_col <= kMaxCols, "no frame addressed a column past the terminal");
    check(oversize == 0, "no frame was painted at a geometry nobody measured");

    /* No flicker: the screen is cleared once, on the way in. A clear anywhere
     * else is a blank refresh the user sees as a blink. */
    check(count_of(stream, "\033[2J") == 1,
          "the screen was cleared once, entering the alternate screen");

    /* No cursor artefacts: hidden on entry, shown on exit, nothing in
     * between. */
    check(count_of(stream, "\033[?25l") == 1, "the cursor was hidden once");
    check(count_of(stream, "\033[?25h") == 1, "the cursor was shown once");
    check(stream.rfind("\033[?25h") > stream.rfind("\033[2J"),
          "the cursor came back only at the end");

    /* Not checked here: that the frame after a resize emitted every cell.
     * event_loop_pty_test owns that, by comparing cells_emitted against
     * cells_examined on a single controlled resize. Asserting it from this side
     * would be asserting that two counters the loop increments together are
     * equal, which is true however the repaint flag is set -- a guard that
     * cannot fail. What this test adds is volume: the same path taken 300 times
     * with the signal landing wherever it lands. */
    check(repaints == static_cast<unsigned long long>(resizes),
          "no resize was silently dropped between the signal and the repaint");

    check(why == static_cast<int>(hv::LoopExit::Quit),
          "the loop survived the storm and exited through quit");

    if (g_failures != 0) {
        std::fprintf(stderr, "resize_storm_test: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("resize_storm_test: a rapid resize tears, flickers and blinks "
                "not at all\n");
    return 0;
}
