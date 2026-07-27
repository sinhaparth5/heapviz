/* heapviz - terminal heap profiler, consumer side.
 *
 * Copyright (C) 2026 Parth Sinha
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Attach/lifecycle is M2.3 and the framebuffer is M4.2, so this binary cannot
 * yet show a heap. What it can do is prove the terminal layer (M4.1): enter raw
 * mode and the alternate screen, and give the terminal back on every exit path.
 */

#include "common/heapviz_abi.h"
#include "tui/terminal.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string_view>
#include <unistd.h>

namespace {

constexpr std::string_view kVersion = "0.1.0-dev";

void print_version() {
    std::printf("heapviz %s (ABI v%u, %zu-byte events, %zu-byte ring header)\n",
                kVersion.data(), HEAPVIZ_ABI_VERSION,
                sizeof(HvEvent), sizeof(HvRingHeader));
}

void print_usage() {
    std::printf(
        "heapviz - live heap allocation visualiser\n"
        "\n"
        "usage:\n"
        "  heapviz -- <cmd> [args...]   launch a program under heapviz\n"
        "  heapviz --pid <pid>          attach to a running target\n"
        "  heapviz --version            print version and ABI details\n"
        "  heapviz --help               this message\n"
        "\n"
        "development aids:\n"
        "  heapviz --term-check         exercise raw mode and teardown (M4.1)\n"
        "\n"
        "Not yet implemented: attach is ROADMAP.md M2.3, the framebuffer and\n"
        "renderer are M4.2 and M4.3.\n");
}

void put(std::string_view s) {
    const char *p = s.data();
    std::size_t n = s.size();
    while (n > 0) {
        const ssize_t w = ::write(STDOUT_FILENO, p, n);
        if (w < 0) { if (errno == EINTR) continue; return; }
        p += w;
        n -= static_cast<std::size_t>(w);
    }
}

/* Hand-check for M4.1. Everything here is provisional: cursor positioning by
 * hand and a busy read loop are precisely what M4.2 and M4.5 replace. It exists
 * so the teardown paths can be exercised against a real terminal, which the pty
 * test cannot do for things like resize or an actual Ctrl-C from a keyboard. */
int term_check() {
    hv::TerminalGuard guard;
    const hv::TermStatus st = guard.enter(STDOUT_FILENO);
    if (st != hv::TermStatus::Ok) {
        std::fprintf(stderr, "heapviz: %s\n", hv::term_status_str(st));
        return 1;
    }

    put("\033[2;4Hheapviz terminal check (M4.1)\r\n");
    put("\033[4;4HRaw mode is on: keys arrive unbuffered and unechoed.\r\n");
    put("\033[5;4HISIG is kept, so Ctrl-C is a signal and exits cleanly.\r\n");
    put("\033[7;4HPress q or Ctrl-C to leave. The shell you return to should\r\n");
    put("\033[8;4Hlook exactly as it did before.\r\n");
    put("\033[10;4HKeys seen: ");

    while (!hv::quit_requested()) {
        char c = 0;
        const ssize_t n = ::read(STDIN_FILENO, &c, 1);
        if (n < 0) { if (errno == EINTR) continue; break; }
        if (n == 0) {
            /* VMIN=0 means read() returns immediately when no key is waiting.
             * usleep() is gone in POSIX.1-2008; M4.5 replaces this whole busy
             * loop with poll() on a frame deadline. */
            timespec ts{0, 10 * 1000 * 1000};
            nanosleep(&ts, nullptr);
            continue;
        }
        if (c == 'q') break;
        if (c >= 32 && c < 127) put(std::string_view(&c, 1));
    }

    guard.restore();
    std::printf("heapviz: terminal restored\n");
    return 0;
}

} // namespace

int main(int argc, char **argv) {
    if (argc >= 2) {
        const std::string_view arg{argv[1]};
        if (arg == "--version" || arg == "-V") { print_version(); return 0; }
        if (arg == "--help" || arg == "-h")    { print_usage();   return 0; }
        if (arg == "--term-check")             { return term_check(); }

        std::fprintf(stderr, "heapviz: not implemented yet: %s\n", argv[1]);
        std::fprintf(stderr, "heapviz: try --help\n");
        return 2;
    }
    print_usage();
    return 0;
}
