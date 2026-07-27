/* heapviz - terminal heap profiler, consumer side.
 *
 * Copyright (C) 2026 Parth Sinha
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Attach/lifecycle is M2.3, so this binary cannot yet show a heap. What it can
 * do is drive the terminal layer end to end: raw mode and the alternate screen
 * (M4.1), a clipped double-buffered cell grid (M4.2), and a differential ANSI
 * streamer that puts one write(2) on the wire per frame (M4.3).
 */

#include "common/heapviz_abi.h"
#include "tui/framebuffer.h"
#include "tui/renderer.h"
#include "tui/shm_cleanup.h"
#include "tui/terminal.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
#include <string_view>
#include <sys/ioctl.h>
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
        "  heapviz --cleanup            remove rings left by killed targets\n"
        "\n"
        "development aids:\n"
        "  heapviz --term-check         draw a frame and exercise teardown\n"
        "\n"
        "Not yet implemented: attaching to a target is ROADMAP.md M2.3, and the\n"
        "heap map itself is M3.\n");
}

/* SIGKILL skips the interceptor's destructor, so a killed target leaves its ring
 * behind. Segments whose producer is still running are never touched, so this is
 * safe to run while other targets are being profiled. */
int cleanup() {
    const auto segments = hv::list_segments();
    if (segments.empty()) {
        std::printf("heapviz: no segments found\n");
        return 0;
    }

    int live = 0;
    for (const auto &s : segments) {
        if (s.owner_alive) {
            ++live;
            std::printf("  keeping %-28s pid %-7d %6.1f MiB  (still running)\n",
                        s.name.c_str(), s.pid,
                        static_cast<double>(s.bytes) / (1024.0 * 1024.0));
        }
    }

    std::uint64_t freed = 0;
    const int removed = hv::reap_stale_segments(false, &freed);

    std::printf("heapviz: removed %d stale segment%s (%.1f MiB), kept %d live\n",
                removed, removed == 1 ? "" : "s",
                static_cast<double>(freed) / (1024.0 * 1024.0), live);
    return 0;
}

/* Hand-check for M4.1 through M4.3, drawn with the real framebuffer and
 * renderer. The busy-wait loop is provisional: M4.5 replaces it with poll() on
 * a frame deadline. It exists so the terminal layer can be exercised against a
 * real terminal, which the pty test cannot do for a resize or an actual Ctrl-C
 * from a keyboard. */
int term_check() {
    hv::TerminalGuard guard;
    const hv::TermStatus st = guard.enter(STDOUT_FILENO);
    if (st != hv::TermStatus::Ok) {
        std::fprintf(stderr, "heapviz: %s\n", hv::term_status_str(st));
        return 1;
    }

    winsize ws{};
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) != 0 || ws.ws_col == 0) {
        ws.ws_col = 80;
        ws.ws_row = 24;
    }
    const int w = ws.ws_col;
    const int h = ws.ws_row;

    hv::Framebuffer fb;
    hv::Renderer    r;
    if (!fb.resize(w, h)) {
        guard.restore();
        std::fprintf(stderr, "heapviz: terminal too large or too small\n");
        return 1;
    }
    r.reserve(w, h);

    constexpr hv::Rgb kInk    = 0x00D8D8D8;
    constexpr hv::Rgb kPanel  = 0x00101418;
    constexpr hv::Rgb kAccent = 0x0058C7F3;

    std::string keys;
    bool        dirty = true;

    while (!hv::quit_requested()) {
        if (dirty) {
            fb.clear(hv::Cell{U' ', kInk, kPanel, 0});
            fb.box(hv::Rect{0, 0, w, h}, hv::BoxStyle::Rounded, kAccent, kPanel);
            fb.text(3, 0, " heapviz terminal check ", kAccent, kPanel,
                    hv::kAttrBold);
            fb.text(3, 2, "Raw mode is on: keys arrive unbuffered, unechoed.",
                    kInk, kPanel);
            fb.text(3, 3, "ISIG is kept, so Ctrl-C exits the same way q does.",
                    kInk, kPanel);

            /* A TrueColor sweep, which is also the widest run of same-coloured
             * cells the pen elision will ever see. */
            for (int x = 3; x < w - 3; ++x) {
                const auto t = static_cast<hv::Rgb>((x * 255) / (w > 6 ? w - 6 : 1));
                fb.put(x, 5, hv::Cell{U'█', (t << 16) | (0x80u << 8) | (255u - t),
                                      kPanel, 0});
            }

            fb.text(3, 7, "Keys seen: ", kInk, kPanel);
            fb.text(14, 7, keys, kAccent, kPanel, hv::kAttrBold);
            fb.text(3, h - 2, "q or Ctrl-C to leave. Your shell should come "
                              "back exactly as it was.", kInk, kPanel);

            r.render(fb);
            r.flush(STDOUT_FILENO);
            fb.swap(hv::Cell{U' ', kInk, kPanel, 0});
            dirty = false;
        }

        char c = 0;
        const ssize_t n = ::read(STDIN_FILENO, &c, 1);
        if (n < 0) { if (errno == EINTR) continue; break; }
        if (n == 0) {
            /* VMIN=0 means read() returns immediately when no key is waiting. */
            timespec ts{0, 10 * 1000 * 1000};
            nanosleep(&ts, nullptr);
            continue;
        }
        if (c == 'q') break;
        if (c >= 32 && c < 127 && keys.size() < 40) keys.push_back(c);
        dirty = true;
    }

    guard.restore();
    std::printf("heapviz: terminal restored, %zu cells in the last frame\n",
                r.cells_examined());
    return 0;
}

} // namespace

int main(int argc, char **argv) {
    if (argc >= 2) {
        const std::string_view arg{argv[1]};
        if (arg == "--version" || arg == "-V") { print_version(); return 0; }
        if (arg == "--help" || arg == "-h")    { print_usage();   return 0; }
        if (arg == "--term-check")             { return term_check(); }
        if (arg == "--cleanup")                { return cleanup(); }

        std::fprintf(stderr, "heapviz: not implemented yet: %s\n", argv[1]);
        std::fprintf(stderr, "heapviz: try --help\n");
        return 2;
    }
    print_usage();
    return 0;
}
