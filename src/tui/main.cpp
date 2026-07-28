/* heapviz - terminal heap profiler, consumer side.
 *
 * Copyright (C) 2026 Parth Sinha
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Attach/lifecycle is M2.3, so this binary cannot yet show a heap. What it can
 * do is drive the terminal layer end to end: raw mode and the alternate screen
 * (M4.1), a clipped double-buffered cell grid (M4.2), a differential ANSI
 * streamer that puts one write(2) on the wire per frame (M4.3), and a
 * frame-paced event loop that idles at roughly no CPU (M4.5).
 */

#include "common/heapviz_abi.h"
#include "tui/capabilities.h"
#include "tui/event_loop.h"
#include "tui/framebuffer.h"
#include "tui/heat_color.h"
#include "tui/map_view.h"
#include "tui/renderer.h"
#include "tui/shm_cleanup.h"
#include "tui/terminal.h"

#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <unistd.h>
#include <vector>

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

constexpr hv::Rgb kInk    = 0x00D8D8D8;
constexpr hv::Rgb kMuted  = 0x00707880;
constexpr hv::Rgb kPanel  = 0x00101418;
constexpr hv::Rgb kAccent = 0x0058C7F3;
constexpr hv::Rgb kWarn   = 0x00F3B158;

/* A synthetic heap for --term-check.
 *
 * M2.3 is what connects the map to a real target; until then there is no way to
 * look at M3 on a real terminal at all, and "the gutter labels are aligned" is
 * not a thing a unit test can tell you. This churns a 4 MiB address space
 * through the same Grid, HeatMap and MapView the real thing will use, so what is
 * on screen is the shipped code path with a fake event source rather than a
 * drawing of what it might look like.
 *
 * It also gives M4.6 its subject: heavy churn through a full-screen map is the
 * workload the 1 ms frame budget is supposed to survive. */
class DemoHeap {
public:
    static constexpr std::uint64_t kBase = 0x55A0000000ull;
    static constexpr std::uint64_t kSpan = 4u << 20;
    static constexpr std::size_t   kMaxLive = 4096;

    DemoHeap() {
        live_.reserve(kMaxLive); /* once, so churning allocates nothing */
        grid_.set_bounds(kBase, kBase + kSpan);
    }

    void fit(hv::Rect area) {
        hv::fit_grid(grid_, area);
        map_.configure(grid_);
        /* A granularity change invalidates every aggregate, so replay what is
         * live rather than showing an empty map until the next allocation. */
        for (const Live &c : live_)
            map_.on_alloc(c.addr, c.size, c.usable, now_ms_);
    }

    /* One frame's worth of allocator traffic. Returns true if anything moved. */
    bool churn(std::uint32_t now_ms, unsigned ops) {
        now_ms_ = now_ms;
        for (unsigned i = 0; i < ops; ++i) {
            const bool freeing = !live_.empty() &&
                                 (live_.size() >= kMaxLive || (next() & 3u) == 0);
            if (freeing) {
                const std::size_t k = next() % live_.size();
                const Live c = live_[k];
                map_.on_free(c.addr, c.size, c.usable, now_ms);
                live_[k] = live_.back();
                live_.pop_back();
            } else {
                const auto size = static_cast<std::uint32_t>(32 + (next() % 2000));
                const std::uint32_t usable = (size + 31u) & ~31u;
                const std::uint64_t addr =
                    kBase + ((next() % (kSpan - usable)) & ~std::uint64_t{15});
                map_.on_alloc(addr, size, usable, now_ms);
                live_.push_back(Live{addr, size, usable});
            }
        }
        return ops != 0;
    }

    void seed(unsigned n) { churn(0, n); }

    const hv::HeatMap &map() const noexcept { return map_; }

private:
    std::uint64_t next() noexcept {
        rng_ += 0x9E3779B97F4A7C15ull;
        std::uint64_t z = rng_;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    }

    struct Live { std::uint64_t addr; std::uint32_t size; std::uint32_t usable; };

    hv::Grid          grid_;
    hv::HeatMap       map_;
    std::vector<Live> live_;
    std::uint64_t     rng_    = 0x243F6A8885A308D3ull;
    std::uint32_t     now_ms_ = 0;
};

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
        if (c >= 32 && c < 127 && keys_.size() < 32) keys_.push_back(c);
        return true;
    }

    void resized(int w, int h) override { heap_.fit(map_area(w, h)); }

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

        view_.draw(fb, map_area(w, h), heap_.map(), now_ms_);

        fb.text(3, h - 2, "q or Ctrl-C to leave. Your shell should come back "
                          "exactly as it was.", kInk, kPanel);
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
    DemoHeap         heap_;
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

} // namespace

int main(int argc, char **argv) {
    /* Order-independent, because `--term-check --no-unicode` and
     * `--no-unicode --term-check` are both things a person will type. */
    bool timing      = false;
    bool force_ascii = false;
    for (int i = 1; i < argc; ++i) {
        const std::string_view a{argv[i]};
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
