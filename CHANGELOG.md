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
