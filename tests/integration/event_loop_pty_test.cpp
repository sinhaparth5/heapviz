/* heapviz - SIGWINCH through a real terminal (M4.5).
 *
 * Copyright (C) 2026 Parth Sinha
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * unit/event_loop_test.cpp drives the resize path through a substituted
 * measurement function, which proves the loop reacts correctly but says
 * nothing about whether the signal ever arrives. Three things have to line up
 * for that, and none of them are visible in-process:
 *
 *   - the handler has to be installed (it is, by TerminalGuard, not by the
 *     loop -- so a loop run without a guard would never see a resize),
 *   - SIGWINCH has to interrupt the poll(2) the loop is parked in,
 *   - ioctl(TIOCGWINSZ) on the terminal fd has to report the new size.
 *
 * So the child here runs the real loop on a real pty, and the parent resizes
 * that pty from outside with TIOCSWINSZ, exactly as a terminal emulator does
 * when the window is dragged. The child reports what it observed down a pipe,
 * which stays clean whatever escape sequences land on the pty.
 */

#include "tui/event_loop.h"
#include "tui/terminal.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <poll.h>
#include <pty.h>
#include <string>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

constexpr int kStartCols = 40;
constexpr int kStartRows = 12;
constexpr int kGrownCols = 70;
constexpr int kGrownRows = 20;

int g_failures = 0;

void check(bool ok, const char *what) {
    if (!ok) { std::fprintf(stderr, "  FAIL %s\n", what); ++g_failures; }
}

/* ------------------------------------------------------------------ */
/* Child                                                               */
/* ------------------------------------------------------------------ */

/* The frame bytes are thrown away rather than written to the pty. What is
 * under test is the signal and the ioctl; the wire format has its own test,
 * and not filling the pty buffer removes a way for this one to deadlock. */
ssize_t discard_write(int, const void *, std::size_t n) {
    return static_cast<ssize_t>(n);
}

class PtyApp final : public hv::LoopApp {
public:
    int resizes = 0;
    int last_w  = 0;
    int last_h  = 0;

    void resized(int w, int h) override {
        last_w = w;
        last_h = h;
        ++resizes;
    }

    /* The first resize is the loop measuring the terminal on startup; the
     * second is the one the parent caused. Stop once it has been seen, so a
     * signal that never arrives shows up as a timeout rather than as a pass. */
    bool update(std::uint64_t) override {
        if (resizes >= 2) hv::request_quit();
        return false;
    }

    void draw(hv::Framebuffer &fb, const hv::LoopStats &) override {
        fb.clear();
        fb.text(0, 0, "heapviz", hv::kDefaultFg, hv::kDefaultBg);
    }
};

[[noreturn]] void child_body(int report_fd) {
    /* TerminalGuard is what installs the SIGWINCH handler. A loop running
     * without one would poll forever and never learn the terminal changed,
     * which is a coupling worth having a test notice. */
    hv::TerminalGuard guard;
    if (guard.enter(STDOUT_FILENO) != hv::TermStatus::Ok) _exit(20);

    hv::LoopConfig cfg;
    cfg.in_fd      = STDIN_FILENO;
    cfg.out_fd     = STDOUT_FILENO; /* the real ioctl(TIOCGWINSZ) target */
    cfg.target_fps = 60;
    cfg.max_frames = 600; /* a 10 s ceiling, so a lost signal ends the run */
    cfg.writer     = discard_write;
    /* size_fn deliberately left null: the point is the production path. */

    PtyApp        app;
    hv::EventLoop loop(cfg);
    const hv::LoopExit why = loop.run(app);

    guard.restore();

    char       buf[256];
    const int  n = std::snprintf(buf, sizeof buf, "%d %d %d %llu %zu %zu %d\n",
                                 app.last_w, app.last_h, app.resizes,
                                 static_cast<unsigned long long>(loop.stats().repaints),
                                 loop.renderer().cells_emitted(),
                                 loop.renderer().cells_examined(),
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

/* Waits for the child's one line, draining the pty master as it goes. The
 * master has to be read even though the child discards its frames: entering
 * and leaving the alternate screen puts a few dozen bytes on it, and an
 * unread pty is a place for a test to hang. */
std::string collect_report(int master, int report_fd, int timeout_ms) {
    std::string  line;
    const int    slice = 100;
    int          waited = 0;

    while (waited < timeout_ms) {
        pollfd fds[2] = {};
        fds[0].fd = master;      fds[0].events = POLLIN;
        fds[1].fd = report_fd;   fds[1].events = POLLIN;

        const int rc = ::poll(fds, 2, slice);
        if (rc < 0) {
            if (errno == EINTR) continue;
            break;
        }
        waited += slice;

        if ((fds[0].revents & POLLIN) != 0) {
            char          scratch[4096];
            const ssize_t got = ::read(master, scratch, sizeof scratch);
            (void)got; /* drained purely so the pty cannot fill and block */
        }
        if ((fds[1].revents & POLLIN) != 0) {
            char          buf[256];
            const ssize_t n = ::read(report_fd, buf, sizeof buf);
            if (n <= 0) break; /* child exited without reporting */
            line.append(buf, static_cast<std::size_t>(n));
            if (line.find('\n') != std::string::npos) return line;
        }
        if ((fds[1].revents & (POLLHUP | POLLERR)) != 0 && line.empty()) break;
    }
    return line;
}

} // namespace

int main() {
    int report[2];
    if (::pipe(report) != 0) { std::perror("pipe"); return 1; }

    winsize ws{};
    ws.ws_col = kStartCols;
    ws.ws_row = kStartRows;

    int         master = -1;
    const pid_t pid    = ::forkpty(&master, nullptr, nullptr, &ws);
    if (pid < 0) { std::perror("forkpty"); return 1; }

    if (pid == 0) {
        ::close(report[0]);
        child_body(report[1]);
    }
    ::close(report[1]);

    /* Let the child install its handler and park in poll(). Resizing before
     * that would test nothing: the loop's own startup measurement would pick
     * the new size up and the signal would never matter. */
    const timespec settle{0, 400 * 1000 * 1000};
    ::nanosleep(&settle, nullptr);

    ws.ws_col = kGrownCols;
    ws.ws_row = kGrownRows;
    if (::ioctl(master, TIOCSWINSZ, &ws) != 0) {
        std::perror("TIOCSWINSZ");
        ++g_failures;
    }

    const std::string report_line = collect_report(master, report[0], 10000);

    int status = 0;
    ::waitpid(pid, &status, 0);
    ::close(report[0]);
    ::close(master);

    if (report_line.empty()) {
        std::fprintf(stderr,
                     "  FAIL the child never reported: SIGWINCH did not reach "
                     "the loop (child status %d)\n", status);
        std::fprintf(stderr, "event_loop_pty_test: 1 failure(s)\n");
        return 1;
    }

    int         w = 0, h = 0, resizes = 0, why = -1;
    unsigned    repaints = 0;
    std::size_t emitted = 0, examined = 0;
    if (std::sscanf(report_line.c_str(), "%d %d %d %u %zu %zu %d",
                    &w, &h, &resizes, &repaints, &emitted, &examined, &why) != 7) {
        std::fprintf(stderr, "  FAIL unparseable report: %s\n",
                     report_line.c_str());
        return 1;
    }

    check(resizes == 2, "the loop saw the startup measurement and the SIGWINCH");
    check(w == kGrownCols && h == kGrownRows,
          "ioctl(TIOCGWINSZ) reported the terminal's new size");
    check(repaints == 2, "both size changes were counted as full repaints");
    check(examined == static_cast<std::size_t>(kGrownCols) *
                      static_cast<std::size_t>(kGrownRows),
          "the frame after the resize covered the whole new grid");
    check(emitted == examined,
          "every cell was repainted, so the grown region cannot hold stale pixels");
    check(why == static_cast<int>(hv::LoopExit::Quit),
          "the loop exited through the normal quit path");

    if (g_failures != 0) {
        std::fprintf(stderr, "event_loop_pty_test: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("event_loop_pty_test: a real SIGWINCH resizes and repaints\n");
    return 0;
}
