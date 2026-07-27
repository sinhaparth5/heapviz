/* heapviz - terminal state management (M4.1).
 *
 * Copyright (C) 2026 Parth Sinha
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "tui/terminal.h"

#include <cerrno>
#include <csignal>
#include <cstddef>
#include <cstdlib>
#include <exception>
#include <termios.h>
#include <unistd.h>

namespace {

/* ------------------------------------------------------------------ */
/* File-scope state                                                    */
/*                                                                     */
/* A signal handler carries no context, so this cannot be a member of  */
/* TerminalGuard. It is the reason only one guard may be active.       */
/* ------------------------------------------------------------------ */

struct termios g_saved;                  /* valid whenever g_active != 0 */
volatile sig_atomic_t g_active   = 0;
volatile sig_atomic_t g_fd       = -1;
volatile sig_atomic_t g_quit     = 0;
volatile sig_atomic_t g_winch    = 0;
volatile sig_atomic_t g_handlers = 0;

/* Entry: alternate screen, hide cursor, clear, home.
 * Exit:  reset SGR, show cursor, leave alternate screen.
 *
 * The SGR reset comes first on the way out so that dying mid-frame, with a
 * background colour still set, cannot tint the shell the user returns to. */
constexpr char kEnter[]   = "\033[?1049h\033[?25l\033[2J\033[H";
constexpr char kRestore[] = "\033[0m\033[?25h\033[?1049l";

/* write(2) looped over partial writes and EINTR. Async-signal-safe: no stdio,
 * no allocation, no locks. */
void raw_write(int fd, const char *p, std::size_t n) noexcept {
    while (n > 0) {
        const ssize_t w = ::write(fd, p, n);
        if (w < 0) {
            if (errno == EINTR) continue;
            return; /* the terminal is gone; nothing useful left to do */
        }
        p += w;
        n -= static_cast<std::size_t>(w);
    }
}

/* The single restore path. Idempotent by way of the g_active flag, so the
 * destructor running after a signal handler already restored is a no-op.
 *
 * Exact reverse of entry: undo the screen first, then termios. */
void restore_now() noexcept {
    if (g_active == 0) return;
    g_active = 0;

    const int fd = static_cast<int>(g_fd);
    raw_write(fd, kRestore, sizeof(kRestore) - 1);
    ::tcsetattr(fd, TCSAFLUSH, &g_saved); /* async-signal-safe per POSIX */
}

/* SIGINT, SIGTERM, SIGHUP: the program is being asked to stop, not dying. Set
 * a flag and let the event loop tear down in its own time, so the exit path is
 * the same one `q` takes. */
extern "C" void hv_graceful_handler(int) noexcept {
    g_quit = 1;
}

/* SIGWINCH: the terminal changed shape. Everything the loop has to do about it
 * -- ioctl, reallocation, a full repaint -- is unsafe here, so the handler only
 * records that it happened. */
extern "C" void hv_winch_handler(int) noexcept {
    g_winch = 1;
}

/* SIGSEGV, SIGABRT, SIGBUS, SIGFPE, SIGILL: the process is going down. Restore
 * the terminal, then re-raise so the core dump and exit status are exactly what
 * they would have been without us. SA_RESETHAND has already put the default
 * disposition back, so raise() dies rather than recursing.
 *
 * Calling _exit() here instead would hand the user a readable shell and a
 * silently swallowed crash, which is a bad trade when the crash is ours. */
extern "C" void hv_fatal_handler(int sig) noexcept {
    restore_now();
    ::raise(sig);
}

void atexit_restore() noexcept {
    restore_now();
}

/* An uncaught exception unwinds nothing on the way to std::terminate, so the
 * guard's destructor never runs. */
[[noreturn]] void terminate_restore() noexcept {
    restore_now();
    std::abort();
}

/* Flags arrive unsigned because SA_RESETHAND is 0x80000000; sa_flags is a
 * signed int, so the cast is the only way to set it without tripping
 * -Wsign-conversion. */
void install_one(int sig, void (*handler)(int), unsigned extra_flags) noexcept {
    struct sigaction sa {};
    sa.sa_handler = handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = static_cast<int>(extra_flags);
    ::sigaction(sig, &sa, nullptr);
}

void install_handlers() noexcept {
    if (g_handlers != 0) return;
    g_handlers = 1;

    /* SA_RESTART so a signal does not turn every read() in the event loop into
     * an EINTR the caller has to think about. */
    install_one(SIGINT,  hv_graceful_handler, SA_RESTART);
    install_one(SIGTERM, hv_graceful_handler, SA_RESTART);
    install_one(SIGHUP,  hv_graceful_handler, SA_RESTART);

    /* Deliberately without SA_RESTART. poll(2) is never restarted either way,
     * so the loop still wakes promptly; the flag alone would be enough. */
    install_one(SIGWINCH, hv_winch_handler, 0);

    /* SA_RESETHAND makes the re-raise in hv_fatal_handler terminate us. */
    install_one(SIGSEGV, hv_fatal_handler, SA_RESETHAND);
    install_one(SIGABRT, hv_fatal_handler, SA_RESETHAND);
    install_one(SIGBUS,  hv_fatal_handler, SA_RESETHAND);
    install_one(SIGFPE,  hv_fatal_handler, SA_RESETHAND);
    install_one(SIGILL,  hv_fatal_handler, SA_RESETHAND);

    std::atexit(atexit_restore);
    std::set_terminate(terminate_restore);
}

} // namespace

namespace hv {

const char *term_status_str(TermStatus s) noexcept {
    switch (s) {
    case TermStatus::Ok:              return "ok";
    case TermStatus::NotATty:         return "stdout is not a terminal";
    case TermStatus::TcGetAttrFailed: return "could not read terminal attributes";
    case TermStatus::TcSetAttrFailed: return "could not set terminal attributes";
    case TermStatus::AlreadyActive:   return "terminal already in raw mode";
    }
    return "unknown";
}

TermStatus TerminalGuard::enter(int fd) noexcept {
    if (g_active != 0)        return TermStatus::AlreadyActive;
    if (::isatty(fd) == 0)    return TermStatus::NotATty;
    if (::tcgetattr(fd, &g_saved) != 0) return TermStatus::TcGetAttrFailed;

    /* Handlers first, and g_active set before the terminal is actually touched.
     * If a signal arrives in the middle of this function, restore_now() then
     * has everything it needs: g_saved is already populated, and re-applying it
     * to a terminal we have not yet modified is harmless. The alternative
     * ordering leaves a window where a signal strands the terminal in raw
     * mode. */
    install_handlers();
    g_fd     = fd;
    g_active = 1;

    struct termios raw = g_saved;

    /* ICANON off: bytes arrive without waiting for Enter.
     * ECHO off: keystrokes do not paint themselves over the frame.
     * IEXTEN off: no Ctrl-V literal-next swallowing our input.
     * ISIG stays ON (decision D4): Ctrl-C keeps working the way muscle memory
     * expects, and SIGINT runs the same teardown as `q`. */
    raw.c_lflag &= static_cast<tcflag_t>(~(ICANON | ECHO | IEXTEN));

    /* IXON off: Ctrl-S / Ctrl-Q are keys, not flow control.
     * ICRNL off: Ctrl-M stays 0x0D instead of arriving as 0x0A.
     * BRKINT, INPCK, ISTRIP, INLCR, IGNCR: the traditional raw-mode set. */
    raw.c_iflag &= static_cast<tcflag_t>(
        ~(IXON | ICRNL | INLCR | IGNCR | ISTRIP | INPCK | BRKINT));

    /* OPOST off: no LF -> CRLF rewriting. The renderer positions the cursor
     * explicitly, so any translation the kernel does is corruption. */
    raw.c_oflag &= static_cast<tcflag_t>(~OPOST);

    /* Non-blocking polls: read() returns 0 immediately when no key is waiting,
     * which is what the frame-paced loop in M4.5 wants. */
    raw.c_cc[VMIN]  = 0;
    raw.c_cc[VTIME] = 0;

    if (::tcsetattr(fd, TCSAFLUSH, &raw) != 0) {
        g_active = 0;
        return TermStatus::TcSetAttrFailed;
    }

    raw_write(fd, kEnter, sizeof(kEnter) - 1);
    return TermStatus::Ok;
}

void TerminalGuard::restore() noexcept { restore_now(); }

bool TerminalGuard::active() const noexcept { return g_active != 0; }

TerminalGuard::~TerminalGuard() { restore_now(); }

bool quit_requested() noexcept { return g_quit != 0; }

void request_quit() noexcept { g_quit = 1; }

/* An exchange rather than a read followed by a store. A SIGWINCH landing
 * between those two would be cleared without ever being seen, and since the
 * handler only ever sets the flag there is nothing to raise it again: the
 * window is small but it strands the display at the wrong size until the user
 * resizes a second time. */
bool take_resize_request() noexcept {
    return __atomic_exchange_n(&g_winch, 0, __ATOMIC_ACQ_REL) != 0;
}

void request_resize() noexcept { g_winch = 1; }

} // namespace hv
