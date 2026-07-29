/* heapviz - terminal heap profiler, consumer side.
 *
 * Copyright (C) 2026 Parth Sinha
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Argument handling and the two ways in: `--pid` attaches to a process already
 * running the interceptor, `-- <cmd>` launches one with it injected. Both end
 * up in `attach_and_run`, which is the whole session: claim the ring, drive the
 * event loop over a `HeapApp`, and give the terminal back on the way out.
 *
 * `--term-check` remains the hand-check for the terminal layer (M4.1 to M4.5)
 * and is the only thing left here that draws its own frames.
 */

#include "common/heapviz_abi.h"
#include "tui/capabilities.h"
#include "tui/demo_heap.h"
#include "tui/event_loop.h"
#include "tui/framebuffer.h"
#include "tui/heap_app.h"
#include "tui/heat_color.h"
#include "tui/map_view.h"
#include "tui/renderer.h"
#include "tui/session.h"
#include "tui/shm_cleanup.h"
#include "tui/terminal.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
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
        "  heapviz --cleanup            remove rings left by killed targets\n"
        "\n"
        "options:\n"
        "  --no-unicode                 ASCII glyphs, for terminals whose font\n"
        "                               has no block-drawing characters\n"
        "\n"
        "development aids:\n"
        "  heapviz --term-check         run the terminal engine on a test frame\n"
        "  heapviz --term-check --debug-timing\n"
        "                               ...with the per-phase frame budget shown\n"
        "\n"
        "Colour depth is detected from COLORTERM and TERM. To see a fallback,\n"
        "run e.g. `COLORTERM= TERM=linux heapviz --term-check`.\n"
        "\n"
        "A program that allocates from several threads has its memory spread\n"
        "across several arenas; heapviz shows one at a time and says how many\n"
        "it is not showing (ROADMAP.md M2.4).\n");
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

constexpr hv::Rgb kInk    = 0x00D8D8D8;
constexpr hv::Rgb kMuted  = 0x00707880;
constexpr hv::Rgb kPanel  = 0x00101418;
constexpr hv::Rgb kAccent = 0x0058C7F3;
constexpr hv::Rgb kWarn   = 0x00F3B158;

/* Hand-check for M4.1 through M4.5, drawn through the real framebuffer,
 * renderer and event loop. It exists so the terminal layer can be exercised
 * against a real terminal: a pty test can drive a resize, but it cannot tell
 * you whether the result flickered, and it cannot press Ctrl-C. */
class TermCheckApp final : public hv::LoopApp {
public:
    TermCheckApp(bool timing, hv::Capabilities caps)
        : caps_(caps), glyphs_(hv::glyphs_for(caps)), view_(caps),
          timing_(timing) {
        heap_.seed(600);
    }

    bool key(char c) override {
        if (c == 'q') { hv::request_quit(); return false; }
        if (c == 'a') { animate_ = !animate_; return true; }
        if (c == 't') { timing_ = !timing_; return true; }

        /* Before the key log, not after: M5.1's bindings are the ones a person
         * has to judge by feel -- whether `n` lands where the eye expected on a
         * sparse map -- and this is the only place they can be driven against a
         * real terminal. Logging them as well would be harmless; swallowing the
         * movement would not. */
        hv::CursorMove m{};
        if (hv::cursor_move_for_key(c, m)) return cursor_.move(heap_.map(), m);

        if (c >= 32 && c < 127 && keys_.size() < 32) keys_.push_back(c);
        return true;
    }

    void resized(int w, int h) override {
        heap_.fit(map_area(w, h));
        cursor_.refit(heap_.map().grid());
    }

    bool update(std::uint64_t now_ns) override {
        if (start_ns_ == 0) start_ns_ = now_ns;
        now_ms_ = static_cast<std::uint32_t>((now_ns - start_ns_) / 1000000u);
        return animate_ ? heap_.churn(now_ms_, 60) : false;
    }

    /* Off by default, so the skipped-frame counter shows the idle path doing
     * its job. Pressing `a` turns it on and the fps counter climbs to the frame
     * rate, which is the other half of the demonstration.
     *
     * The map's own fades keep it true for a couple of seconds after the churn
     * stops, which is the point: a frame skipped mid-fade would freeze the
     * colour until the next allocation happened to arrive. */
    bool animating() const override {
        return animate_ || hv::MapView::animating(heap_.map(), now_ms_);
    }

    void draw(hv::Framebuffer &fb, const hv::LoopStats &s) override {
        const int w = fb.width();
        const int h = fb.height();

        fb.clear(hv::Cell{U' ', kInk, kPanel, 0});
        fb.box(hv::Rect{0, 0, w, h}, glyphs_.box, kAccent, kPanel);
        fb.text(3, 0, " heapviz terminal check ", kAccent, kPanel, hv::kAttrBold);

        fb.text(3, 2, "Raw mode is on: keys arrive unbuffered, unechoed.",
                kInk, kPanel);
        fb.text(3, 3, "ISIG is kept, so Ctrl-C exits the same way q does.",
                kInk, kPanel);
        fb.text(3, 4, "Resize the window: the frame follows without tearing.",
                kInk, kPanel);

        /* A full-saturation sweep. It is both the widest run of distinct
         * colours the pen elision will ever face and the clearest way to see a
         * quantiser working: in 256-colour mode the gradient visibly steps, and
         * in 16-colour mode it collapses to a handful of bands. */
        for (int x = 3; x < w - 3; ++x) {
            const auto t = static_cast<hv::Rgb>((x * 255) / (w > 6 ? w - 6 : 1));
            fb.put(x, 6, hv::Cell{glyphs_.full,
                                  (t << 16) | (0x80u << 8) | (255u - t),
                                  kPanel, 0});
        }

        /* The density ramp, which is what --no-unicode actually changes. */
        int gx = 3;
        for (int rep = 0; rep < 6 && gx < w - 4; ++rep, ++gx)
            fb.put(gx, 7, hv::Cell{glyphs_.full, kAccent, kPanel, 0});
        for (int rep = 0; rep < 6 && gx < w - 4; ++rep, ++gx)
            fb.put(gx, 7, hv::Cell{glyphs_.medium, kAccent, kPanel, 0});
        for (int rep = 0; rep < 6 && gx < w - 4; ++rep, ++gx)
            fb.put(gx, 7, hv::Cell{glyphs_.light, kAccent, kPanel, 0});

        char caps[96];
        std::snprintf(caps, sizeof caps, "  %s, %s glyphs",
                      hv::color_mode_str(caps_.color),
                      caps_.unicode ? "Unicode" : "ASCII");
        fb.text(gx, 7, caps, kMuted, kPanel);

        fb.text(3, 8, "Keys seen: ", kInk, kPanel);
        fb.text(14, 8, keys_, kAccent, kPanel, hv::kAttrBold);

        draw_aging_ramps(fb, w);

        int row = 12;
        fb.text(3, row++, animate_ ? "a: animation ON  - every frame redraws"
                                   : "a: animation off - idle frames are skipped",
                animate_ ? kWarn : kMuted, kPanel);
        fb.text(3, row++, "t: toggle the timing overlay", kMuted, kPanel);

        if (timing_) {
            char line[160];

            std::snprintf(line, sizeof line,
                          "%6.1f fps   frames %-8llu drawn %-8llu skipped %-8llu"
                          " repaints %llu",
                          s.fps,
                          static_cast<unsigned long long>(s.frames),
                          static_cast<unsigned long long>(s.drawn),
                          static_cast<unsigned long long>(s.skipped),
                          static_cast<unsigned long long>(s.repaints));
            fb.text(3, ++row, line, kInk, kPanel);

            std::snprintf(line, sizeof line,
                          "last  drain %5.0fus update %5.0fus draw %5.0fus"
                          " diff %5.0fus write %5.0fus  = %6.0fus",
                          us(s.last.drain_ns), us(s.last.update_ns),
                          us(s.last.draw_ns), us(s.last.diff_ns),
                          us(s.last.write_ns), us(s.last.total_ns));
            fb.text(3, ++row, line, kMuted, kPanel);

            std::snprintf(line, sizeof line,
                          "worst drain %5.0fus update %5.0fus draw %5.0fus"
                          " diff %5.0fus write %5.0fus  = %6.0fus",
                          us(s.worst.drain_ns), us(s.worst.update_ns),
                          us(s.worst.draw_ns), us(s.worst.diff_ns),
                          us(s.worst.write_ns), us(s.worst.total_ns));
            fb.text(3, ++row, line, kMuted, kPanel);

            std::snprintf(line, sizeof line,
                          "writes %-8llu bytes %-10llu overruns %llu",
                          static_cast<unsigned long long>(s.writes),
                          static_cast<unsigned long long>(s.bytes_written),
                          static_cast<unsigned long long>(s.overruns));
            fb.text(3, ++row, line,
                    s.overruns != 0 ? kWarn : kMuted, kPanel);
        }

        const hv::Rect area = map_area(w, h);
        view_.draw(fb, area, heap_.map(), now_ms_);
        view_.draw_cursor(fb, area, heap_.map(), cursor_);

        fb.text(3, h - 2, "q or Ctrl-C to leave. hjkl/HJKL move the cursor, "
                          "g/G the ends, n/N the next chunk.", kInk, kPanel);
    }

private:
    static double us(std::uint64_t ns) { return static_cast<double>(ns) / 1000.0; }

    /* Fixed rather than packed under whatever the text above happens to end at,
     * because `t` toggles four rows of overlay and a map whose geometry moved
     * with it would re-bucket the whole address space on a keypress. */
    static hv::Rect map_area(int w, int h) noexcept {
        constexpr int kTop = 19; /* below the timing block, drawn or not */
        return hv::Rect{3, kTop, w - 6, h - 3 - kTop};
    }

    /* M3.4's two fades with time laid out across the screen, so the whole
     * timeline is visible at once. Watching one cell age tests your memory of
     * what the colour was a second ago; a ramp shows a discontinuity as a seam
     * you can point at, which is the only way to judge "smooth" by eye. */
    void draw_aging_ramps(hv::Framebuffer &fb, int w) {
        constexpr int kLabel = 16;
        const int span = w - kLabel - 3;
        if (span < 8) return;

        const hv::HeatTimings t{};
        const hv::HeatRamp    ramp{hv::kDefaultPalette, t};

        hv::CellAggregate live{};
        live.n_live        = 1;
        live.live_bytes    = 2048; /* half of the 4 KiB cell assumed below */
        live.last_alloc_ms = 0;

        hv::CellAggregate gone{};
        gone.last_free_ms = 0;

        const auto steps      = static_cast<std::uint32_t>(span);
        const auto alloc_span = t.malloc_pulse_ms + t.malloc_fade_ms;
        const auto free_span  = t.free_flash_ms + t.free_fade_ms;

        fb.text(3, 9,  "malloc  1.0s", kMuted, kPanel);
        fb.text(3, 10, "free    2.3s", kMuted, kPanel);

        for (int x = 0; x < span; ++x) {
            const auto i = static_cast<std::uint32_t>(x);
            fb.put(kLabel + x, 9,
                   hv::Cell{glyphs_.full,
                            ramp.color(live, 4096, alloc_span * i / steps),
                            kPanel, 0});
            fb.put(kLabel + x, 10,
                   hv::Cell{glyphs_.full,
                            ramp.color(gone, 4096, free_span * i / steps),
                            kPanel, 0});
        }
    }

    hv::Capabilities caps_;
    hv::GlyphSet     glyphs_;
    hv::MapView      view_;
    hv::MapCursor    cursor_;
    hv::DemoHeap     heap_;
    std::string   keys_;
    std::uint64_t start_ns_ = 0;
    std::uint32_t now_ms_   = 0;
    bool          animate_  = false;
    bool          timing_   = false;
};

int term_check(bool timing, bool force_ascii) {
    const hv::Capabilities caps = hv::detect_capabilities_from_env(force_ascii);

    /* Checked before the alternate screen is entered, so the refusal lands in
     * the user's scrollback rather than on a screen that is torn down the
     * moment it is printed. */
    int w = 0, h = 0;
    if (hv::terminal_size(STDOUT_FILENO, w, h) && !hv::size_is_usable(w, h)) {
        hv::report_too_small(STDERR_FILENO, w, h);
        return 1;
    }

    hv::TerminalGuard guard;
    const hv::TermStatus st = guard.enter(STDOUT_FILENO);
    if (st != hv::TermStatus::Ok) {
        std::fprintf(stderr, "heapviz: %s\n", hv::term_status_str(st));
        return 1;
    }

    hv::LoopConfig cfg;
    cfg.in_fd  = STDIN_FILENO;
    cfg.out_fd = STDOUT_FILENO;
    cfg.color  = caps.color;

    hv::EventLoop  loop(cfg);
    TermCheckApp   app(timing, caps);
    const hv::LoopExit why = loop.run(app);

    guard.restore();

    const hv::LoopStats &s = loop.stats();
    std::printf("heapviz: %s after %llu frames (%llu drawn, %llu skipped, "
                "%llu overruns), worst frame %.0f us\n",
                hv::loop_exit_str(why),
                static_cast<unsigned long long>(s.frames),
                static_cast<unsigned long long>(s.drawn),
                static_cast<unsigned long long>(s.skipped),
                static_cast<unsigned long long>(s.overruns),
                static_cast<double>(s.worst.total_ns) / 1000.0);
    return why == hv::LoopExit::Quit || why == hv::LoopExit::FrameLimit ? 0 : 1;
}

/* Runs an attached session to completion. Everything before the TerminalGuard
 * reports to the user's scrollback, because anything printed after it is torn
 * down with the alternate screen the moment the loop exits. */
int attach_and_run(int pid, bool launched, bool force_ascii) {
    hv::RingSession session;
    const hv::AttachStatus st = session.attach(pid);
    if (st != hv::AttachStatus::Ok) {
        std::fprintf(stderr, "heapviz: %s (pid %d)\n",
                     hv::attach_status_str(st), pid);
        if (st == hv::AttachStatus::NoSegment && !launched)
            std::fprintf(stderr,
                         "heapviz: start it with `heapviz -- <cmd>`, or "
                         "LD_PRELOAD libheapviz.so into it yourself\n");
        if (st == hv::AttachStatus::AlreadyWatched)
            std::fprintf(stderr, "heapviz: the other one is pid %u\n",
                         session.other_consumer());
        if (st == hv::AttachStatus::AbiMismatch)
            std::fprintf(stderr,
                         "heapviz: target speaks ABI v%u, this build speaks "
                         "v%u; rebuild both halves\n",
                         session.target_abi(), HEAPVIZ_ABI_VERSION);
        return 1;
    }

    const hv::Capabilities caps = hv::detect_capabilities_from_env(force_ascii);

    int w = 0, h = 0;
    if (hv::terminal_size(STDOUT_FILENO, w, h) && !hv::size_is_usable(w, h)) {
        hv::report_too_small(STDERR_FILENO, w, h);
        return 1;
    }

    hv::TerminalGuard guard;
    const hv::TermStatus ts = guard.enter(STDOUT_FILENO);
    if (ts != hv::TermStatus::Ok) {
        std::fprintf(stderr, "heapviz: %s\n", hv::term_status_str(ts));
        return 1;
    }

    hv::LoopConfig cfg;
    cfg.in_fd  = STDIN_FILENO;
    cfg.out_fd = STDOUT_FILENO;
    cfg.color  = caps.color;

    hv::EventLoop loop(cfg);
    hv::HeapApp   app(session, caps);
    const hv::LoopExit why = loop.run(app);

    guard.restore();

    /* Detached explicitly rather than left to the destructor: releasing the
     * claim is what lets the next heapviz attach, and a target that outlives
     * this one must not be left permanently unwatchable. */
    session.detach();

    const hv::LoopStats &s = loop.stats();
    std::printf("heapviz: %s after %llu events from pid %d (%llu frames, "
                "%llu drawn)\n",
                app.state() == hv::SessionState::Exited ? "target exited"
                                                        : hv::loop_exit_str(why),
                static_cast<unsigned long long>(app.events_seen()), pid,
                static_cast<unsigned long long>(s.frames),
                static_cast<unsigned long long>(s.drawn));
    return why == hv::LoopExit::Quit || why == hv::LoopExit::FrameLimit ? 0 : 1;
}

int launch_and_run(char **cmd, bool force_ascii) {
    const std::string preload = hv::find_preload();
    if (preload.empty()) {
        std::fprintf(stderr,
                     "heapviz: cannot find libheapviz.so next to this binary; "
                     "set HEAPVIZ_PRELOAD to its path\n");
        return 1;
    }

    /* The target blocks in its constructor until we claim the ring, so a
     * program that allocates and exits cannot finish -- and unlink its segment
     * -- before we have finished starting up. */
    const hv::Launch l = hv::launch_target(cmd, preload, hv::kAttachTimeoutMs);
    if (l.status != hv::LaunchStatus::Ok) {
        std::fprintf(stderr, "heapviz: %s: %s: %s\n",
                     hv::launch_status_str(l.status), cmd[0],
                     std::strerror(l.err));
        return 1;
    }

    return attach_and_run(l.pid, true, force_ascii);
}

} // namespace

int main(int argc, char **argv) {
    /* Order-independent, because `--term-check --no-unicode` and
     * `--no-unicode --term-check` are both things a person will type.
     *
     * The scan stops at `--`, after which the arguments belong to the target:
     * `heapviz -- ./app --no-unicode` is asking the app for ASCII, not heapviz,
     * and a scan that read the whole of argv would quietly answer for both. */
    bool timing      = false;
    bool force_ascii = false;
    for (int i = 1; i < argc; ++i) {
        const std::string_view a{argv[i]};
        if (a == "--") break;
        if (a == "--debug-timing") timing = true;
        if (a == "--no-unicode")   force_ascii = true;
    }

    if (argc >= 2) {
        const std::string_view arg{argv[1]};
        if (arg == "--version" || arg == "-V") { print_version(); return 0; }
        if (arg == "--help" || arg == "-h")    { print_usage();   return 0; }
        if (arg == "--term-check") {
            return term_check(timing, force_ascii);
        }
        if (arg == "--cleanup")                { return cleanup(); }
        if (arg == "--pid") {
            if (argc < 3) {
                std::fprintf(stderr, "heapviz: --pid needs a process id\n");
                return 2;
            }
            const int pid = std::atoi(argv[2]);
            if (pid <= 0) {
                std::fprintf(stderr, "heapviz: not a process id: %s\n", argv[2]);
                return 2;
            }
            return attach_and_run(pid, false, force_ascii);
        }
        if (arg == "--") {
            if (argc < 3) {
                std::fprintf(stderr, "heapviz: -- needs a command to run\n");
                return 2;
            }
            return launch_and_run(argv + 2, force_ascii);
        }
        if (arg == "--no-unicode") {
            /* A modifier on its own is not a command. */
            print_usage();
            return 0;
        }

        std::fprintf(stderr, "heapviz: not implemented yet: %s\n", argv[1]);
        std::fprintf(stderr, "heapviz: try --help\n");
        return 2;
    }
    print_usage();
    return 0;
}
