/* heapviz - differential ANSI streamer checks (M4.3).
 *
 * Copyright (C) 2026 Parth Sinha
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The renderer's whole value is in what it does NOT emit, so most of these
 * tests count sequences rather than look for them: a correct renderer and a
 * naive one both put the right pixels on screen, and differ only in how many
 * bytes it took. A regression here is silent and shows up as a frame rate.
 *
 * Separating "produce the bytes" from "write the bytes" is what makes this
 * testable without a terminal: render() fills a buffer we can inspect exactly.
 */

#include "tui/renderer.h"
#include "tui/framebuffer.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cerrno>
#include <cstring>
#include <new>
#include <string>
#include <sys/wait.h>
#include <unistd.h>

#ifndef __SANITIZE_ADDRESS__
#define HV_COUNT_ALLOCS 1
namespace { std::atomic<long> g_allocs{0}; }
void *operator new(std::size_t n) {
    g_allocs.fetch_add(1, std::memory_order_relaxed);
    void *p = std::malloc(n != 0 ? n : 1);
    if (p == nullptr) throw std::bad_alloc();
    return p;
}
void *operator new[](std::size_t n) { return ::operator new(n); }
void  operator delete(void *p) noexcept { std::free(p); }
void  operator delete[](void *p) noexcept { std::free(p); }
void  operator delete(void *p, std::size_t) noexcept { std::free(p); }
void  operator delete[](void *p, std::size_t) noexcept { std::free(p); }
#endif

namespace {

using hv::Cell;
using hv::Framebuffer;
using hv::Renderer;

int g_failures = 0;

void check(bool ok, const char *what) {
    if (!ok) { std::fprintf(stderr, "  FAIL %s\n", what); ++g_failures; }
}

std::string out_of(const Renderer &r) {
    return std::string(r.data(), r.size());
}

int count(const std::string &hay, const std::string &needle) {
    if (needle.empty()) return 0;
    int n = 0;
    for (std::size_t i = hay.find(needle); i != std::string::npos;
         i = hay.find(needle, i + needle.size())) {
        ++n;
    }
    return n;
}

/* Cursor moves are the only sequences that end in 'H', and the glyphs used
 * below are chosen to contain no ASCII 'H', so counting the character counts
 * the moves. */
int cursor_moves(const std::string &s) { return count(s, "H"); }

constexpr hv::Rgb kFg = 0x00FF8000;
constexpr hv::Rgb kBg = 0x00102030;
constexpr char32_t kBlock = U'█'; /* 3 UTF-8 bytes, no ASCII inside */

/* Draws `f` into the back buffer, promotes it to front, leaving a cleared
 * back buffer: the state the next frame starts from. */
template <typename F>
void commit_frame(Framebuffer &fb, F &&f) {
    f(fb);
    fb.swap();
}

void test_idle_frame_costs_nothing() {
    Framebuffer fb;
    fb.resize(20, 5);
    Renderer r;
    r.reserve(20, 5);

    check(r.render(fb) == 0, "idle: an unchanged frame produces no bytes");
    check(r.size() == 0, "idle: the output buffer is empty");
    check(r.cells_emitted() == 0, "idle: no cells emitted");
    check(r.cells_examined() == 100, "idle: but every cell was examined");

    /* And an idle frame must not even reach the terminal. */
    check(r.flush(-1), "idle: flushing nothing does not touch the fd");
}

void test_single_cell_wire_format() {
    Framebuffer fb;
    fb.resize(10, 3);
    Renderer r;
    r.reserve(10, 3);

    fb.put(0, 0, Cell{U'A', kFg, kBg, 0});
    r.render(fb);

    const std::string expect =
        "\033[1;1H"              /* ANSI coordinates are 1-based */
        "\033[0m"                /* pen starts unknown, so reset first */
        "\033[38;2;255;128;0m"   /* TrueColor foreground */
        "\033[48;2;16;32;48m"    /* TrueColor background */
        "A"
        "\033[0m";               /* frame epilogue leaves no colour set */
    check(out_of(r) == expect, "wire format: exact byte sequence for one cell");
    check(r.cells_emitted() == 1, "wire format: emitted exactly one cell");
}

void test_pen_elision() {
    Framebuffer fb;
    fb.resize(20, 2);
    Renderer r;
    r.reserve(20, 2);

    for (int x = 0; x < 10; ++x) fb.put(x, 0, Cell{kBlock, kFg, kBg, 0});
    r.render(fb);
    const std::string s = out_of(r);

    check(r.cells_emitted() == 10, "pen: emitted ten cells");
    check(count(s, "38;2;") == 1, "pen: one foreground sequence for ten cells");
    check(count(s, "48;2;") == 1, "pen: one background sequence for ten cells");

    /* Alternating colours cannot be elided, and must not be. */
    fb.resize(20, 2);
    for (int x = 0; x < 10; ++x) {
        const hv::Rgb fg = (x % 2 == 0) ? 0x00FF0000u : 0x0000FF00u;
        fb.put(x, 0, Cell{kBlock, fg, kBg, 0});
    }
    r.render(fb);
    check(count(out_of(r), "38;2;") == 10,
          "pen: alternating colours emit one sequence each");
}

/* M4.4. The mode changes the bytes on the wire and nothing else: the diff, the
 * cursor elision and the epilogue are all mode-independent, so these check the
 * colour sequences and leave the rest to the tests above. */
void test_color_mode_wire_formats() {
    Framebuffer fb;
    fb.resize(10, 3);
    Renderer r;
    r.reserve(10, 3);

    check(r.color_mode() == hv::ColorMode::TrueColor,
          "mode: TrueColor is the default, as it was before M4.4");

    r.set_color_mode(hv::ColorMode::Cube256);
    fb.put(0, 0, Cell{U'A', 0x00FF0000u, 0x00000000u, 0});
    r.render(fb);
    check(out_of(r) ==
              "\033[1;1H" "\033[0m" "\033[38;5;196m" "\033[48;5;16m" "A" "\033[0m",
          "mode: 256-colour emits palette indices");
    check(count(out_of(r), "38;2;") == 0,
          "mode: 256-colour emits no 24-bit sequence");

    fb.resize(10, 3);
    r.set_color_mode(hv::ColorMode::Ansi16);
    fb.put(0, 0, Cell{U'A', 0x00FF0000u, 0x00000000u, 0});
    r.render(fb);
    check(out_of(r) ==
              "\033[1;1H" "\033[0m" "\033[91m" "\033[40m" "A" "\033[0m",
          "mode: 16-colour uses the bright range for 8..15");

    /* The first eight and the brights are different code ranges, not one range
     * with an offset, so both have to be exercised. */
    fb.resize(10, 3);
    fb.put(0, 0, Cell{U'A', 0x00800000u, 0x00008000u, 0});
    r.render(fb);
    check(out_of(r) ==
              "\033[1;1H" "\033[0m" "\033[31m" "\033[42m" "A" "\033[0m",
          "mode: 16-colour uses 30-37/40-47 for the first eight");
}

/* The elision has to key off the code that gets emitted, not off the Cell's
 * RGB. Quantising is many-to-one, so a renderer that compared the raw values
 * would re-emit an identical sequence for every cell in a gradient and hand a
 * 256-colour terminal more bytes per frame than a TrueColor one gets. */
void test_quantised_colours_still_elide() {
    Framebuffer fb;
    fb.resize(20, 2);
    Renderer r;
    r.reserve(20, 2);
    r.set_color_mode(hv::ColorMode::Cube256);

    /* Ten distinct reds, all near enough to quantise to cube index 196. */
    for (int x = 0; x < 10; ++x) {
        const auto fg = static_cast<hv::Rgb>(0x00FF0000u + static_cast<hv::Rgb>(x));
        fb.put(x, 0, Cell{kBlock, fg, kBg, 0});
    }
    r.render(fb);

    check(r.cells_emitted() == 10, "quantised pen: emitted ten cells");
    check(count(out_of(r), "38;5;") == 1,
          "quantised pen: ten near-identical reds share one sequence");
    check(count(out_of(r), "38;5;196m") == 1,
          "quantised pen: and it is the index they all quantise to");
}

void test_cursor_elision() {
    Framebuffer fb;
    fb.resize(20, 3);
    Renderer r;
    r.reserve(20, 3);

    for (int x = 2; x < 8; ++x) fb.put(x, 1, Cell{kBlock, kFg, kBg, 0});
    r.render(fb);
    check(cursor_moves(out_of(r)) == 1,
          "cursor: a consecutive run needs one move");

    /* A gap means the next cell is not where the cursor landed. */
    fb.resize(20, 3);
    fb.put(2, 1, Cell{kBlock, kFg, kBg, 0});
    fb.put(9, 1, Cell{kBlock, kFg, kBg, 0});
    r.render(fb);
    check(cursor_moves(out_of(r)) == 2, "cursor: a gap forces a second move");

    /* Crossing a row boundary needs a move, since whether the terminal wrapped
     * after the last column depends on its autowrap setting.
     *
     * Note this does not prove the cur_valid_ = false in render(): an
     * impossible column can never match a valid target, so a renderer that
     * skipped that line would emit the same two moves. See the comment
     * there. */
    fb.resize(4, 3);
    for (int x = 0; x < 4; ++x) fb.put(x, 0, Cell{kBlock, kFg, kBg, 0});
    fb.put(0, 1, Cell{kBlock, kFg, kBg, 0});
    r.render(fb);
    check(cursor_moves(out_of(r)) == 2, "cursor: a new row gets its own move");
}

void test_only_changed_cells() {
    Framebuffer fb;
    fb.resize(10, 3);
    Renderer r;
    r.reserve(10, 3);

    /* Frame 1: a full row. */
    commit_frame(fb, [](Framebuffer &b) {
        for (int x = 0; x < 10; ++x) b.put(x, 0, Cell{kBlock, kFg, kBg, 0});
    });

    /* Frame 2: the same row, one cell recoloured. */
    for (int x = 0; x < 10; ++x) fb.put(x, 0, Cell{kBlock, kFg, kBg, 0});
    fb.put(4, 0, Cell{kBlock, 0x0000FF00u, kBg, 0});

    r.render(fb);
    check(r.cells_emitted() == 1, "diff: only the cell that changed was emitted");
    check(r.cells_examined() == 30, "diff: the whole buffer was still examined");

    const std::string s = out_of(r);
    check(count(s, "\033[1;5H") == 1, "diff: moved straight to the changed cell");

    /* A full repaint ignores the front buffer entirely, which is what SIGWINCH
     * needs once the terminal has thrown away its contents. */
    r.render(fb, /*full_repaint=*/true);
    check(r.cells_emitted() == 30, "diff: a full repaint emits every cell");
}

void test_attributes() {
    Framebuffer fb;
    fb.resize(10, 2);
    Renderer r;
    r.reserve(10, 2);

    fb.put(0, 0, Cell{U'x', kFg, kBg, hv::kAttrBold | hv::kAttrUnderline});
    r.render(fb);
    const std::string s = out_of(r);
    check(count(s, "\033[1;4m") == 1, "attrs: bold and underline in one SGR");

    /* Dropping an attribute has no portable single code, so it goes through a
     * reset; the colours must then be re-emitted rather than assumed. */
    fb.resize(10, 2);
    fb.put(0, 0, Cell{U'x', kFg, kBg, hv::kAttrBold});
    fb.put(1, 0, Cell{U'y', kFg, kBg, 0});
    r.render(fb);
    const std::string s2 = out_of(r);
    check(count(s2, "38;2;255;128;0") == 2,
          "attrs: colours re-emitted after an attribute reset");
}

void test_utf8_encode() {
    std::vector<char> b;
    const auto enc = [&](char32_t cp) {
        b.clear();
        hv::utf8_encode(b, cp);
        return std::string(b.data(), b.size());
    };

    check(enc(U'A') == "A", "utf8: ascii is one byte");
    check(enc(U'é') == "\xC3\xA9", "utf8: two bytes");
    check(enc(U'█') == "\xE2\x96\x88", "utf8: three bytes, the block glyph");
    check(enc(0x1F4A9) == "\xF0\x9F\x92\xA9", "utf8: four bytes");
    check(enc(0xD800) == "\xEF\xBF\xBD", "utf8: a surrogate becomes U+FFFD");
    check(enc(0x110000) == "\xEF\xBF\xBD", "utf8: out of range becomes U+FFFD");

    /* A zeroed cell means "never drawn" and must reach the terminal as a
     * space, not as a NUL byte. */
    Framebuffer fb;
    fb.resize(4, 1);
    Renderer r;
    r.reserve(4, 1);
    fb.put(0, 0, Cell{0, kFg, kBg, 0});
    r.render(fb);
    check(out_of(r).find('\0') == std::string::npos,
          "utf8: a zero glyph does not put a NUL on the wire");
    check(count(out_of(r), " ") == 1, "utf8: a zero glyph renders as a space");
}

void test_integer_formatting() {
    Framebuffer fb;
    fb.resize(4, 1);
    Renderer r;
    r.reserve(4, 1);

    /* Boundary values for the hand-rolled digit conversion: 0 must not become
     * an empty string, 255 must not lose a digit. */
    fb.put(0, 0, Cell{U'x', 0x00000000u, 0x00FFFFFFu, 0});
    r.render(fb);
    const std::string s = out_of(r);
    check(count(s, "\033[38;2;0;0;0m") == 1, "int: zero renders as \"0\"");
    check(count(s, "\033[48;2;255;255;255m") == 1, "int: 255 renders in full");
}

void test_flush_partial_writes() {
    /* A pipe's buffer is 64 KB by default, so a larger payload forces write(2)
     * to return short. Ignoring that return value is the classic bug here and
     * would truncate every large frame. */
    Framebuffer fb;
    fb.resize(200, 50);
    Renderer r;
    r.reserve(200, 50);

    for (int y = 0; y < 50; ++y)
        for (int x = 0; x < 200; ++x)
            fb.put(x, y, Cell{kBlock, static_cast<hv::Rgb>(x * 7 + y), kBg, 0});

    const std::size_t expected = r.render(fb);
    check(expected > 64u * 1024u,
          "flush: the test payload is larger than a pipe buffer");

    int fds[2];
    if (pipe(fds) != 0) { std::perror("pipe"); std::exit(1); }
    std::fflush(nullptr);

    const pid_t pid = fork();
    if (pid < 0) { std::perror("fork"); std::exit(1); }
    if (pid == 0) {
        close(fds[1]);
        std::size_t total = 0;
        char buf[4096];
        for (;;) {
            const ssize_t n = read(fds[0], buf, sizeof(buf));
            if (n > 0) { total += static_cast<std::size_t>(n); continue; }
            if (n < 0 && errno == EINTR) continue;
            break;
        }
        _exit(total == expected ? 0 : 1);
    }
    close(fds[0]);
    const bool ok = r.flush(fds[1]);
    close(fds[1]);

    int status = 0;
    waitpid(pid, &status, 0);
    check(ok, "flush: reported success");
    check(WIFEXITED(status) && WEXITSTATUS(status) == 0,
          "flush: every byte arrived through the pipe");
}

/* State for the substitute writers below. Function pointers cannot capture, so
 * this is file-scope by necessity. */
std::string g_sink;
int         g_write_calls  = 0;
int         g_eintr_budget = 0;

ssize_t chunked_write(int, const void *buf, std::size_t n) {
    ++g_write_calls;
    if (g_eintr_budget > 0) {
        --g_eintr_budget;
        errno = EINTR;
        return -1;
    }
    /* Deliberately short: a real terminal does this when a signal lands
     * mid-write, having already transferred some bytes. */
    const std::size_t take = n < 1000u ? n : 1000u;
    g_sink.append(static_cast<const char *>(buf), take);
    return static_cast<ssize_t>(take);
}

ssize_t failing_write(int, const void *, std::size_t) {
    errno = EPIPE;
    return -1;
}

void test_flush_short_write_loop() {
    Framebuffer fb;
    fb.resize(60, 20);
    Renderer r;
    r.reserve(60, 20);
    for (int y = 0; y < 20; ++y)
        for (int x = 0; x < 60; ++x)
            fb.put(x, y, Cell{kBlock, static_cast<hv::Rgb>(x + y * 3), kBg, 0});

    const std::size_t total = r.render(fb);
    const std::string expect = out_of(r);

    g_sink.clear();
    g_write_calls  = 0;
    g_eintr_budget = 0;
    check(r.flush(1, chunked_write), "short write: flush reported success");
    check(g_write_calls > 1, "short write: the loop ran more than once");
    check(g_sink.size() == total, "short write: delivered every byte");
    check(g_sink == expect, "short write: delivered the bytes in order");

    /* EINTR must be retried, not treated as failure, and must not consume
     * bytes: an interrupted write transferred nothing. */
    g_sink.clear();
    g_write_calls  = 0;
    g_eintr_budget = 3;
    check(r.flush(1, chunked_write), "eintr: flush still succeeded");
    check(g_sink == expect, "eintr: retrying delivered the identical stream");

    /* A real error is not retried forever. */
    check(!r.flush(1, failing_write), "error: a failing write is reported");
}

void test_frame_epilogue() {
    Framebuffer fb;
    fb.resize(6, 2);
    Renderer r;
    r.reserve(6, 2);

    fb.put(0, 0, Cell{U'x', kFg, kBg, 0});
    r.render(fb);
    const std::string t = out_of(r);
    check(t.size() >= 4 && t.compare(t.size() - 4, 4, "\033[0m") == 0,
          "epilogue: the frame ends by clearing colour");
}

#ifdef HV_COUNT_ALLOCS
void test_render_allocates_nothing() {
    Framebuffer fb;
    fb.resize(200, 50);
    Renderer r;
    r.reserve(200, 50);

    /* One worst-case frame first, so any growth the buffer needed has already
     * happened before the counter starts. */
    for (int y = 0; y < 50; ++y)
        for (int x = 0; x < 200; ++x)
            fb.put(x, y, Cell{kBlock, static_cast<hv::Rgb>(x ^ y), kBg,
                              static_cast<std::uint8_t>((x & 1) ? hv::kAttrBold : 0)});
    r.render(fb, true);

    const char *buf_before = r.data();
    const long  before     = g_allocs.load(std::memory_order_relaxed);

    for (int frame = 0; frame < 20; ++frame) {
        for (int y = 0; y < 50; ++y)
            for (int x = 0; x < 200; ++x)
                fb.put(x, y, Cell{kBlock,
                                  static_cast<hv::Rgb>(x * frame + y), kBg,
                                  static_cast<std::uint8_t>((frame & 1) ? hv::kAttrBold : 0)});
        r.render(fb, true);
        fb.swap();
    }

    const long after = g_allocs.load(std::memory_order_relaxed);
    check(after == before, "render: twenty worst-case frames allocated nothing");
    check(r.data() == buf_before,
          "render: the output buffer was never reallocated");
}
#endif

} // namespace

int main() {
    test_idle_frame_costs_nothing();
    test_single_cell_wire_format();
    test_pen_elision();
    test_color_mode_wire_formats();
    test_quantised_colours_still_elide();
    test_cursor_elision();
    test_only_changed_cells();
    test_attributes();
    test_utf8_encode();
    test_integer_formatting();
    test_flush_partial_writes();
    test_flush_short_write_loop();
    test_frame_epilogue();
#ifdef HV_COUNT_ALLOCS
    test_render_allocates_nothing();
#endif

    if (g_failures != 0) {
        std::fprintf(stderr, "renderer_test: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("renderer_test: diffing, pen and cursor elision, UTF-8, and "
                "short-write handling all hold\n");
    return 0;
}
