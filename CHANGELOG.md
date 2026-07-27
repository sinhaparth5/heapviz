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
- README notes `reset` / `stty sane` for the one case nothing can guard
  against, `kill -9`.

### Changed

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
