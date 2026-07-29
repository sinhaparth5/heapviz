# Changelog

All notable changes to **heapviz** are documented here.

Format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).
Versioning follows [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## How to use this file

- Add every user-visible change to `[Unreleased]` **in the same commit that makes
  the change**. Reconstructing a changelog from `git log` at release time
  produces a list of commits, not a changelog.
- Write entries for someone using heapviz, not for someone reading the diff.
  "Fixed a crash when the target process exits mid-frame", not "null check in
  `MapsParser::refresh`".
- Categories, in this order: **Added**, **Changed**, **Deprecated**, **Removed**,
  **Fixed**, **Security**. Omit empty ones.
- Reference issues/PRs as `(#12)` where one exists.

### Two things in this project need extra care

**The shared-memory ABI.** `libheapviz.so` and the `heapviz` binary are compiled
separately and may be at different versions on a user's machine. Any change to
`Event`, `RingHeader`, or `HEAPVIZ_ABI_VERSION` is a breaking change. Call
it out under **Changed** with the words "ABI break", state the old and new
`HEAPVIZ_ABI_VERSION`, and note that both halves must be rebuilt.

**Keybindings.** Rebinding or removing a key is a breaking change to muscle
memory. It goes under **Changed** with the old and new binding both named.

### Releasing

1. Move `[Unreleased]` entries into a new `## [X.Y.Z] - YYYY-MM-DD` section.
2. Add the compare link at the bottom of this file.
3. Tag `vX.Y.Z`.

Pre-1.0, the minor version carries breaking changes; `0.x` makes no stability
promise about the ABI or the CLI surface.

---

## [Unreleased]

### Added

- Working `LD_PRELOAD` interceptor: `malloc`, `free`, `calloc`, `realloc`,
  `posix_memalign`, `aligned_alloc`, `memalign`, `valloc`, and `pvalloc`, plus
  everything reaching them indirectly (`strdup`, `asprintf`, `getline`, C++
  `operator new`). Adds 31 ns per allocator call against a 50 ns budget.
- Lock-free multi-producer ring buffer shared between the target process and
  heapviz, with per-slot commit and lap-parity publication.
- `HEAPVIZ_CAPACITY` (ring size, power of two), `HEAPVIZ_DISABLE`, and
  `HEAPVIZ_WAIT_MS` (hold the target in its constructor until a consumer
  attaches, so short-lived programs do not lose their telemetry).
- `examples/churn.c`: configurable workload with steady, bursty, fragmenting,
  mmap, and mixed modes, plus a thread count and rate limit.
- Four more tests: multi-producer ring stress (10M events through a 4096-slot
  ring), end-to-end interception across a process boundary, transitive coverage
  of indirect allocation paths, and an overhead budget check.
- Terminal handling: heapviz enters raw mode and the alternate screen, and
  gives the terminal back on every exit path it can reach, including `SIGINT`,
  `SIGTERM`, `SIGHUP`, a crash, and an uncaught exception. A crash still dies
  as a crash, with its core dump and exit status intact. Ctrl-C keeps working
  conventionally and exits the same way `q` will.
- `heapviz --term-check`, a development aid that exercises the terminal layer
  against a real terminal.
- Double-buffered cell grid behind the renderer: 16-byte cells, a clipped
  drawing API (text, lines, boxes, fills), and UTF-8 decoding. Buffers are
  sized when the terminal resizes and reused for every frame after that, so
  drawing a frame allocates nothing.
- Differential renderer: only cells that changed are redrawn, runs of one
  colour cost a single colour sequence, and consecutive cells need no cursor
  motion. Each frame reaches the terminal in one write, and a frame where
  nothing changed costs no output at all. TrueColor throughout.
- Frame pacing: heapviz redraws at most 60 times a second and, when nothing has
  changed, does not redraw at all. An idle heapviz sleeps between frames rather
  than spinning, so watching a quiet process costs a fraction of a percent of a
  core. Keystrokes are still picked up within one frame.
- Resizing the terminal reflows the display without tearing or leaving debris
  behind, and heapviz keeps running with stdin closed or redirected from
  `/dev/null`.
- `heapviz --term-check --debug-timing` shows where each frame's time went,
  split into draining the ring, updating the model, drawing, diffing and
  writing, alongside the frame rate and a count of frames that overran their
  budget. Inside `--term-check`, `a` toggles continuous animation and `t`
  toggles the overlay.
- `heapviz --cleanup` removes rings left behind by targets that were killed
  with `SIGKILL`, which skips the interceptor's own cleanup. Rings belonging to
  a process that is still running are never touched, so it is safe to run
  during a profiling session.
- Colour now adapts to the terminal instead of assuming 24-bit. heapviz reads
  `COLORTERM` and `TERM` at startup and picks TrueColor, the 256-colour
  palette, or 16 colours; the display stays usable over an SSH session to a
  console that would previously have shown escape codes as text. Run
  `COLORTERM= TERM=linux heapviz --term-check` to see a fallback on a terminal
  that does not need one.
- `--no-unicode` draws with `#`, `=`, `.` and ASCII borders, for terminals
  whose font has no block-drawing characters.
- heapviz refuses to start on a terminal smaller than 80x24, naming both the
  size it found and the size it needs, rather than drawing something illegible.
  A terminal that is resized *below* that while heapviz is running is not a
  reason to quit; the display only stops if it shrinks past the point where a
  frame can be drawn at all.
- Heat colours age on their own. A fresh allocation pulses bright green for a
  fifth of a second and then settles to blue over the next four fifths; a free
  flashes red and fades to the colour of unallocated space over two seconds.
  Long-lived memory sits at blue, dimmer where a cell is only partly used, so a
  sparsely occupied region reads as darker than a packed one. The fades are
  interpolated perceptually, which is what keeps green-to-blue from passing
  through a muddy grey on the way. `heapviz --term-check` now shows both fades
  laid out as time across the screen.
- The heap map is drawn. Each cell stands for a span of addresses, shaded by
  how much of it is in use and coloured by what happened there most recently.
  Down the left is an address gutter, labelling every row with its offset from
  the start of the heap; above it a legend names the current granularity — the
  live figure, so it follows the window as you resize it. On a terminal too
  narrow for the gutter the map keeps the columns instead. If the address range
  is wider than any granularity can cover, the legend says the top of it is not
  being shown rather than displaying part of the heap as though it were all of
  it. Pressing `a` in `heapviz --term-check` now churns a synthetic 4 MiB heap
  through the real map, which is the only way to see M3 until attaching lands.
- README notes `reset` / `stty sane` for the one case nothing can guard
  against, `kill -9`.
- heapviz can now read a target's memory map, which is what tells it where the
  heap actually is rather than inferring bounds from the addresses it happens
  to have seen. It distinguishes the main `[heap]` from thread arenas and from
  ordinary mmap'd memory, so the header bar can name the arenas a program is
  allocating from, and it re-reads the map twice a second and immediately
  whenever an allocation lands outside the range it knew about — so a heap that
  grows mid-frame is on screen in the next one. A target that exits while being
  watched is reported as exited, with its last known map kept so the display
  can still be read. None of this is visible until attaching lands.

### Changed

- **ABI break: `HEAPVIZ_ABI_VERSION` 2 to 3.** Both halves must be rebuilt
  together. Only one heapviz can watch a target at a time, and a second one now
  says so and exits instead of attaching. Previously both would attach and each
  would silently see roughly half the events, because they shared one read
  cursor with nothing arbitrating it. The ring header's `consumer_attached`
  flag became `consumer_pid`, claimed with a compare-and-swap; same offset and
  width, different meaning.

- **ABI break: `HEAPVIZ_ABI_VERSION` 1 to 2.** Both `libheapviz.so` and the
  `heapviz` binary must be rebuilt together. The v1 ring assumed a single
  producer, which corrupts on any threaded target since every thread calling
  `malloc` is a producer. v2 claims slots with a CAS loop and publishes each
  slot through two flag bits in `HvEvent.op` (bit 7 commit, bit 6 lap parity;
  the opcode now occupies bits 0-5). `HvRingHeader` gained `capacity_log2` at
  offset 56 and `consumer_attached` at offset 212. `HvEvent` and `HvRingHeader`
  keep their 32- and 256-byte sizes.

- Shared memory ABI (`src/common/heapviz_abi.h`) at `HEAPVIZ_ABI_VERSION` 1: a
  32-byte `HvEvent` packet and a 256-byte `HvRingHeader` laid out across four
  cache lines, with `head` and `tail` deliberately on separate lines. Compiles
  under both C11 and C++20; every field offset is pinned by a static assert.
- CMake build producing `heapviz` (C++20 TUI) and `libheapviz.so` (C11
  interceptor), with `debug`, `release`, and `asan` presets.
- Test suite (5 tests): cross-language ABI layout comparison, a C11-writes /
  C++20-reads shared memory round-trip, a libstdc++ absence check on the
  interceptor, a dlopen smoke test, and a TUI version check.
- `examples/hello_alloc.c`, a small workload covering the brk path, the mmap
  path, and a realloc growth chain.
- `ROADMAP.md`: milestone breakdown and progress tracker derived from
  `heap-doc.md`.
- `CHANGELOG.md`: this file.
- Project logo and icon (`assets/logo.svg`, `assets/icon.svg`, with PNG
  exports), built from the TUI's own colour palette.
- Full `README.md`: goals, feature overview, architecture diagram,
  requirements, keybindings, and known limitations.
- `assets/how-it-works.png` architecture diagram, replacing the ASCII drawing
  that was previously inline in the README.

---

## Planned releases

Not yet shipped. Listed so the roadmap and the release history line up. Move
these into real version sections as they land, and delete them from here.

### 0.1.0: first usable release

Target scope: [ROADMAP.md](ROADMAP.md) milestones M0–M6.

- Zero-overhead `LD_PRELOAD` interceptor over `malloc`/`free`/`calloc`/
  `realloc`/`posix_memalign` (M1)
- Lock-free SPSC shared-memory telemetry ring (M1)
- Sparse page-bucketed spatial heap map with heatmap aging (M3)
- 60 FPS double-buffered differential ANSI renderer (M4)
- Vim-key cursor, chunk inspector, telemetry metrics, fragmentation analysis,
  snapshot/leak diff (M5)
- TrueColor theme with 256-colour and ASCII fallbacks (M4, M6)

### 0.2.0: candidates

- `/proc` chunk-header enrichment via `process_vm_readv` (M2.2), if not pulled
  into 0.1.0
- Multi-arena visualisation and arena switcher (roadmap decision D3)
- Light theme (M6.1)

---

[Unreleased]: https://github.com/sinhaparth5/heapviz/commits/master
