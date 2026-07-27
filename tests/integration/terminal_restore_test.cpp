/* heapviz - terminal setup/teardown checks (M4.1).
 *
 * Copyright (C) 2026 Parth Sinha
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Ground rule #4 ("the terminal is always restored") is a claim about global
 * state in a process that may be dying, so it cannot be tested by calling a
 * function and inspecting a return value. Instead each scenario runs in a child
 * attached to a real pty, and we check both halves of the promise from outside:
 * the bytes that actually reached the terminal, and the termios state left
 * behind afterwards.
 *
 * The interesting cases are the ones where no destructor runs -- a fatal signal
 * and an uncaught exception. Those are exactly the paths that strand a shell in
 * raw mode with an invisible cursor, and the only ones worth writing a pty
 * harness for.
 */

#include "tui/terminal.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <csignal>
#include <stdexcept>
#include <termios.h>
#include <unistd.h>
#include <pty.h>
#include <sys/wait.h>

namespace {

constexpr char kEnter[]   = "\033[?1049h\033[?25l\033[2J\033[H";
constexpr char kRestore[] = "\033[0m\033[?25h\033[?1049l";

enum class Scenario { Clean, Sigint, Fatal, Exception, ExceptionNoAbrt };

bool same_termios(const termios &a, const termios &b) {
    if (a.c_iflag != b.c_iflag || a.c_oflag != b.c_oflag ||
        a.c_cflag != b.c_cflag || a.c_lflag != b.c_lflag) {
        return false;
    }
    return std::memcmp(a.c_cc, b.c_cc, sizeof(a.c_cc)) == 0;
}

/* Runs inside the child, with `fd` wired to the pty slave. The return value
 * becomes the exit status, so each failure gets a distinguishable code. */
int child_body(Scenario s, int fd) {
    termios before{};
    if (tcgetattr(fd, &before) != 0) return 10;

    hv::TerminalGuard guard;
    if (guard.enter(fd) != hv::TermStatus::Ok) return 11;
    if (!guard.active())                       return 12;

    switch (s) {
    case Scenario::Clean:
        break;

    case Scenario::Sigint:
        /* SIGINT must be a request to stop, not a death. If this kills the
         * child, the graceful/fatal split is broken. */
        if (hv::quit_requested())  return 13;
        raise(SIGINT);
        if (!hv::quit_requested()) return 14;
        break;

    case Scenario::Fatal:
        raise(SIGSEGV);
        return 15; /* unreachable: the handler re-raises */

    case Scenario::Exception:
        throw std::runtime_error("uncaught");

    case Scenario::ExceptionNoAbrt:
        /* The plain Exception case cannot tell whether std::set_terminate or
         * the SIGABRT handler did the restoring, because std::terminate calls
         * abort() and both are installed. Dropping our SIGABRT handler first
         * leaves set_terminate as the only thing that can save the terminal,
         * which is also the real-world case this guards: some other library
         * owning SIGABRT. */
        signal(SIGABRT, SIG_DFL);
        throw std::runtime_error("uncaught, and SIGABRT is not ours");
    }

    guard.restore();
    if (guard.active()) return 18;
    guard.restore(); /* idempotent: a second call must not fault or re-emit */

    termios after{};
    if (tcgetattr(fd, &after) != 0)      return 16;
    if (!same_termios(before, after))    return 17;
    return 0;
}

struct Result {
    int         status = 0;
    std::string out;
};

Result run_scenario(Scenario s) {
    int master = -1, slave = -1;
    if (openpty(&master, &slave, nullptr, nullptr, nullptr) != 0) {
        std::perror("openpty");
        std::exit(1);
    }

    /* Do not let the child inherit and re-flush our buffered output into the
     * pty; it would show up as terminal bytes we never wrote. */
    std::fflush(nullptr);

    const pid_t pid = fork();
    if (pid < 0) { std::perror("fork"); std::exit(1); }

    if (pid == 0) {
        close(master);
        dup2(slave, STDIN_FILENO);
        dup2(slave, STDOUT_FILENO);
        if (slave > STDERR_FILENO) close(slave);
        /* exit(), not _exit(): the atexit restore hook is part of what we are
         * testing. */
        std::exit(child_body(s, STDOUT_FILENO));
    }

    close(slave); /* else the master never sees EOF */

    Result r;
    char buf[512];
    for (;;) {
        const ssize_t n = read(master, buf, sizeof(buf));
        if (n > 0) { r.out.append(buf, static_cast<std::size_t>(n)); continue; }
        /* On Linux a pty master returns EIO once the last slave closes. */
        if (n < 0 && errno == EINTR) continue;
        break;
    }
    close(master);
    waitpid(pid, &r.status, 0);
    return r;
}

int g_failures = 0;

void check(bool ok, const char *what) {
    if (!ok) { std::fprintf(stderr, "  FAIL %s\n", what); ++g_failures; }
}

/* The restore sequence must be present, and must come after the entry
 * sequence. Finding it first would mean we restored something we never set. */
void check_restored(const Result &r, const char *label) {
    const std::size_t enter_at   = r.out.find(kEnter);
    const std::size_t restore_at = r.out.rfind(kRestore);

    char msg[128];
    std::snprintf(msg, sizeof(msg), "%s: entered the alternate screen", label);
    check(enter_at != std::string::npos, msg);

    std::snprintf(msg, sizeof(msg), "%s: restored the terminal", label);
    check(restore_at != std::string::npos, msg);

    std::snprintf(msg, sizeof(msg), "%s: restore came after entry", label);
    check(enter_at != std::string::npos && restore_at != std::string::npos &&
          restore_at > enter_at, msg);
}

} // namespace

int main() {
    /* 1. Clean exit. Also checks termios came back byte-identical (child exit
     *    code 17 means it did not). */
    {
        const Result r = run_scenario(Scenario::Clean);
        check(WIFEXITED(r.status), "clean: exited rather than died");
        check(WIFEXITED(r.status) && WEXITSTATUS(r.status) == 0,
              "clean: termios restored exactly and restore() is idempotent");
        check_restored(r, "clean");
    }

    /* 2. SIGINT is a stop request, not a death (decision D4). */
    {
        const Result r = run_scenario(Scenario::Sigint);
        check(WIFEXITED(r.status), "sigint: survived, ISIG did not kill us");
        check(WIFEXITED(r.status) && WEXITSTATUS(r.status) == 0,
              "sigint: set the quit flag and restored cleanly");
        check_restored(r, "sigint");
    }

    /* 3. A fatal signal restores the terminal AND still dies as it would have.
     *    Swallowing the crash would hand the user a clean shell and hide a bug
     *    in the profiler. */
    {
        const Result r = run_scenario(Scenario::Fatal);
        check(WIFSIGNALED(r.status), "fatal: died by signal");
        check(WIFSIGNALED(r.status) && WTERMSIG(r.status) == SIGSEGV,
              "fatal: exit status is still SIGSEGV, crash not swallowed");
        check_restored(r, "fatal");
    }

    /* 4. An uncaught exception never unwinds to the destructor, so
     *    std::set_terminate is the only thing standing between the user and a
     *    wrecked shell. */
    {
        const Result r = run_scenario(Scenario::Exception);
        check(WIFSIGNALED(r.status), "exception: terminated");
        check(WIFSIGNALED(r.status) && WTERMSIG(r.status) == SIGABRT,
              "exception: aborted, as an uncaught exception should");
        check_restored(r, "exception");
    }

    /* 5. The same, with our SIGABRT handler removed, so std::set_terminate is
     *    the only remaining guard. Without this case, deleting the
     *    set_terminate call leaves the whole suite green. */
    {
        const Result r = run_scenario(Scenario::ExceptionNoAbrt);
        check(WIFSIGNALED(r.status) && WTERMSIG(r.status) == SIGABRT,
              "set_terminate: aborted");
        check_restored(r, "set_terminate");
    }

    /* 6. Not a terminal: refuse rather than spray escape codes into a pipe or
     *    a log file. Safe to run in-process precisely because it must not
     *    touch any terminal state. */
    {
        int fds[2];
        if (pipe(fds) != 0) { std::perror("pipe"); return 1; }
        hv::TerminalGuard guard;
        const hv::TermStatus st = guard.enter(fds[1]);
        check(st == hv::TermStatus::NotATty, "pipe: refused with NotATty");
        check(!guard.active(), "pipe: left inactive");

        /* Close the write end before reading: with a writer still open, an
         * empty pipe blocks rather than reporting EOF. */
        close(fds[1]);
        char probe[8];
        const ssize_t n = read(fds[0], probe, sizeof(probe));
        check(n == 0, "pipe: wrote nothing");
        close(fds[0]);
    }

    if (g_failures != 0) {
        std::fprintf(stderr, "terminal_test: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("terminal_test: terminal restored on every exit path (clean, "
                "SIGINT, SIGSEGV, uncaught exception, terminate)\n");
    return 0;
}
