/* heapviz - terminal state management (M4.1).
 *
 * Copyright (C) 2026 Parth Sinha
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Ground rule #4: the terminal is always restored. Every exit path -- clean
 * quit, SIGINT, SIGTERM, SIGSEGV, an uncaught exception -- must put termios,
 * the alternate screen, and the cursor back the way it found them. A profiler
 * that bricks the user's shell is worse than no profiler.
 *
 * Restoration therefore cannot live only in a destructor: a fatal signal never
 * unwinds. The saved state sits in file-scope storage so that signal handlers,
 * the atexit hook, and std::terminate can all reach it, and every one of those
 * paths funnels into the same idempotent restore.
 */

#ifndef HEAPVIZ_TUI_TERMINAL_H
#define HEAPVIZ_TUI_TERMINAL_H

namespace hv {

enum class TermStatus {
    Ok,
    NotATty,          /* stdout is a pipe or file; refuse rather than emit escapes */
    TcGetAttrFailed,
    TcSetAttrFailed,
    AlreadyActive,
};

/* Human-readable, for the error path. */
const char *term_status_str(TermStatus s) noexcept;

/* RAII guard over the terminal's global state.
 *
 * Only one may be active at a time. The state a signal handler needs cannot be
 * per-instance, so a second concurrent guard would have nothing coherent to
 * restore; enter() reports AlreadyActive rather than pretending otherwise.
 */
class TerminalGuard {
public:
    TerminalGuard() noexcept = default;
    ~TerminalGuard();

    TerminalGuard(const TerminalGuard &)            = delete;
    TerminalGuard &operator=(const TerminalGuard &) = delete;

    /* Saves termios, installs handlers, enters raw mode and the alternate
     * screen. On any failure the terminal is left exactly as it was. */
    TermStatus enter(int fd) noexcept;

    /* Idempotent, async-signal-safe, safe to call when not active. */
    void restore() noexcept;

    bool active() const noexcept;
};

/* Set by SIGINT / SIGTERM / SIGHUP. The event loop (M4.5) polls this and exits
 * through the normal teardown path, so Ctrl-C and `q` behave identically.
 *
 * Roadmap decision D4: ISIG stays enabled, so Ctrl-C remains a signal rather
 * than a keystroke we have to decode. */
bool quit_requested() noexcept;

/* Lets `q` and the signal path share one exit condition. */
void request_quit() noexcept;

/* SIGWINCH sets a flag rather than measuring the terminal in the handler:
 * ioctl(TIOCGWINSZ) is not on POSIX's async-signal-safe list, and resizing the
 * framebuffer from a handler would reallocate underneath a frame that is
 * halfway drawn. The loop consumes the flag at a defined point instead.
 *
 * Test-and-clear, so a resize is never processed twice and one that arrives
 * during a frame is picked up by the next. */
bool take_resize_request() noexcept;

/* Raises the same flag by hand. Lets a test drive the resize path without a
 * pty, and lets the loop force the first measurement on startup. */
void request_resize() noexcept;

} // namespace hv

#endif /* HEAPVIZ_TUI_TERMINAL_H */
