# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

heapviz is a Linux terminal heap-allocation profiler. It has two halves that are
compiled separately and talk over POSIX shared memory:

- `libheapviz.so`, an `LD_PRELOAD` interceptor written in C11, injected into the
  target process. It hooks the allocator and writes fixed-size events into a
  ring buffer.
- `heapviz`, a C++20 TUI that maps the same segment, drains the ring, and draws
  the heap.

`ROADMAP.md` is the executable plan and the only design document left: 212
checkboxes across M0 to M7, with a progress table at the top that must be kept
accurate. Read its "Ground rules" section before touching the interceptor or the
render path; those six invariants are what make the design work, and violating
one is a bug even when the tests pass.

`heap-doc.md` does not exist. It was the original design intent and was deleted
in `d3babdb` once its content had been absorbed into the roadmap, but
`ROADMAP.md` and `CHANGELOG.md` still refer to it in passing. Those references
are stale; do not go looking for the file, and do not add new ones.

## Build and test

```bash
cmake --preset debug            # or release, asan
cmake --build --preset debug
ctest --preset debug
```

Binaries land in `build/<preset>/`. The interceptor is `libheapviz.so`, the TUI
is `heapviz`, and `churn` / `hello_alloc` are workloads to point them at.

CMake target names are not the output names, so `--target heapviz` fails:

| Target | Produces | Notes |
|---|---|---|
| `heapviz_preload` | `libheapviz.so` | C11, never sanitized |
| `heapviz_tui` | `heapviz` | thin `main.cpp` over `heapviz_core` |
| `heapviz_core` | `libheapviz_core.a` | everything in `src/tui/` except `main.cpp` — terminal, framebuffer, renderer, loop, the map pipeline, cleanup. The tests link this so they exercise the shipped objects, not a recompile |

Run one test, or a subset:

```bash
ctest --preset release -R ring_mpsc --output-on-failure
ctest --preset release -R 'intercept|transitive' -V
```

Test targets are also plain executables and are usually faster to debug directly:

```bash
./build/debug/ring_mpsc_test 4 10000000           # producers, events
./build/release/intercept_test ./build/release/libheapviz.so ./build/release/churn
```

Exercise the interceptor by hand:

```bash
LD_PRELOAD=./build/release/libheapviz.so ./build/release/churn --threads 4 --seconds 5
```

Environment variables the interceptor reads: `HEAPVIZ_CAPACITY` (ring slots,
power of two, at least 1024), `HEAPVIZ_DISABLE`, and `HEAPVIZ_WAIT_MS` (block in
the constructor until a consumer claims the ring, so a short-lived target
cannot exit and unlink its segment before anyone attaches).

### What the `heapviz` binary can actually do today

Attach landed in M2.3 and the binary shows a real heap. Commands: `--version`
(`-V`), `--help` (`-h`), `--term-check`, `--cleanup`, `--pid`, `--instrument`,
`--`, and the two positional forms; modifiers are `--debug-timing`,
`--no-unicode`, `--no-animation` and `--theme dark|light`:

```bash
./build/debug/heapviz ./build/debug/churn --threads 1 --seconds 5   # positional launch
./build/debug/heapviz $(pgrep -n churn)              # positional attach; one consumer per target
./build/debug/heapviz -- ./build/debug/churn --seconds 5   # explicit forms, still supported
./build/debug/heapviz --pid $(pgrep -n churn)
./build/debug/heapviz --instrument ./some_tui         # terminal 1: exec the target itself
./build/debug/heapviz --version                      # pinned by the tui_version test
./build/debug/heapviz --term-check                   # drive M3 and M4 against a real terminal
./build/debug/heapviz --term-check --debug-timing    # per-phase frame budget overlay
./build/debug/heapviz --cleanup                      # reap rings left by SIGKILLed targets
./build/debug/heapviz --term-check --no-unicode      # ASCII glyph fallback
COLORTERM= TERM=linux ./build/debug/heapviz --term-check   # 16-colour fallback
```

**A bare argument is dispatched by its shape, and the shape is the whole rule.**
Digits only, and greater than zero, is an attach; anything else starts a command
and the rest of argv belongs to that command. An argument beginning with `-` that
matched nothing above is an unknown option and exits 2 — the old "not implemented
yet" catch-all is gone, because a positional command is now a legal thing to find
there. That leaves one collision worth knowing about: a program whose name is
digits cannot be launched positionally, which is what `--` is still for.
Modifiers are scanned across the whole argv so either order works, but the scan
stops at `--`, at `--instrument`, and at a positional command, since everything
after each of those belongs to the target. `--no-unicode` alone prints the usage
and exits 0, since a modifier on its own is not a command.

`tests/cmake/tui_short_forms.cmake` pins the dispatch by the error each form
reaches. It needs no pty because all three of its cases fail before terminal
setup, which is the only reason a CLI rule this central can be held by a CTest
script rather than by hand.

**A launched target does not share the terminal.** heapviz owns it for the
framebuffer, so `launch_target(..., TargetIo::Isolate)` gives the child
`/dev/null` on stdin and a private `/tmp/heapviz-target-<pid>.log` on stdout and
stderr, and `main` prints that path. Without it a target that writes to stdout
paints over the map, and a target that reads stdin competes for the keybindings.
The default is `TargetIo::Inherit` because `attach_test` and any other caller
that already owns its descriptors should keep them.

Two consequences of owning the child that are easy to get wrong. First, the
target's log is the only record of *why* it exited, so `HeapApp::set_launch` is
given the log path and argv0 at startup and `build_exit_note` composes one
sentence on the transition to `Exited` — the `--instrument` command when the
target died inside `kInteractiveExitMs`, its own last line otherwise. It belongs
on the footer and not only in the exit summary, because a reason behind a keypress
is a reason nobody reads: the report that started this was a user watching a
frozen map with no way to know there was anything to press. `hv::last_output_line`
is the one copy of the parsing, and it consumes whole CSI sequences rather than
bare ESC bytes — dropping ESC alone stops the sequence being obeyed but leaves
`[2J[H` in the middle of the sentence. Second, **heapviz is
the parent, so it must reap before it tests liveness.** `process_alive` is
`kill(pid, 0)`, which *succeeds* on an unreaped zombie, so `HeapApp::update` calls
`reap_if_exited` first; with the two in the other order a launched target that
skipped its destructors stayed Live forever. `attach_test`'s
`test_a_killed_target_is_noticed` is the guard, and it uses SIGKILL because a
target that exits cleanly sets `producer_exited` and never reaches the pid check
— which is why the older exit assertions held while this was broken.

That isolation is also why `--instrument` exists. An interactive target is
unusable with its stdin closed, so instead of forking it, heapviz `execvp`s it in
place with `LD_PRELOAD` and `HEAPVIZ_WAIT_MS` set, keeping all three descriptors,
and prints the pid to attach to from a second terminal. `exec_instrumented_target`
returns only on failure, and only an errno.

`--term-check` is the manual counterpart to the pty tests: it is the only way to
judge whether a resize *looked* right. `a` toggles animation so the idle path's
skipped-frame counter can be watched doing its job, `t` toggles the timing
overlay `--debug-timing` starts on, and `q` quits. The map's geometry is
deliberately fixed below row 19 rather than packed under the text, so toggling
the overlay cannot re-bucket the address space on a keypress. Since M3.1's legend
and gutter landed it also carries a synthetic 4 MiB heap driven through the real
`Grid`/`HeatMap`/`MapView`, so `a` is now also the map's churn switch. That heap
is `hv::DemoHeap` in `heapviz_core` rather than a local class, because
`frame_budget` and `resize_storm` measure the same object this drives — a
benchmark against a copy of the workload measures the copy. M5.1's cursor keys
(`hjkl`, `HJKL`, `g`/`G`, `n`/`N`) are live there too, and are the half of that
milestone a unit test cannot judge: whether `n` lands where the eye expected.
M5.2's inspector panel, M5.3's metrics panel and M5.4's fragmentation figure are
*not* there, because `DemoHeap` has no `ChunkTable`, no `RegionMap` and no ring
session, and giving it any of them would change what `frame_budget` and
`resize_storm` measure.

### Preset differences that matter

`asan` builds the TUI and tests only. ASan supplies its own malloc, so it cannot
share a process with an `LD_PRELOAD` malloc hook; every preload-driven test is
guarded by `if(NOT HEAPVIZ_ASAN)`. `interceptor_overhead` and `frame_budget`
additionally only run on optimised builds, because both assert a cost budget and
`-O0` blows through it: the interceptor costs 45-56 ns against its own 50 ns
budget, and the per-cell colour interpolation alone spends most of the frame's
1 ms. Both binaries still build everywhere, so they can be run by hand in debug
to read the numbers.

Expected test counts when everything passes: debug 33, release 35, asan 30.

`attach` is preload-driven, so it is skipped under ASan with the rest of them.
It launches three `churn` processes over its run, each of which creates a 32 MiB
segment; combined with `ring_mpsc` and the two benchmarks, a full `ctest` run
saturates every core for a couple of minutes. Cap the parallelism
(`cmake --build --preset release -- -j 4`) on a machine you are also using.

## Architecture

### The ABI is the contract

`src/common/heapviz_abi.h` is compiled by both halves, in C11 and in C++20. It
carries its own dual-language macro layer (`HV_ATOMIC`, `HV_ALIGNAS`, `HV_LOAD`,
`HV_CAS_WEAK`, and so on) so one source of truth serves both. Every field offset
in `HvEvent` (32 bytes) and `HvRingHeader` (256 bytes) is pinned by a static
assert, which turns layout drift into a compile error rather than silent
cross-process corruption.

Changing anything in that header is a wire-protocol change. Bump
`HEAPVIZ_ABI_VERSION`, and record it in `CHANGELOG.md` under **Changed** with
the words "ABI break", the old and new version numbers, and a note that both
halves must be rebuilt. Users can have mismatched halves installed.

`heapviz --version` prints the version and both struct sizes, and the
`tui_version` test asserts the exact string (`PASS_REGULAR_EXPRESSION` in
`tests/CMakeLists.txt`, currently `ABI v3, 32-byte events, 256-byte ring
header`). A version bump that skips that line fails CTest in a place that looks
unrelated to the header you edited.

`tests/unit/abi_layout_c.c` and `tests/unit/abi_layout_cxx.cpp` compile the
same dump header in both languages and require byte-identical output.

### The ring is multi-producer, not SPSC

The alt text on `README.md`'s "How it works" diagram still describes an SPSC
ring, and `CHANGELOG.md`'s oldest entry still calls it one. Both are stale:
every thread in the target that calls `malloc` is a producer. (`ROADMAP.md`'s
M1.6 said so too until it was corrected; its M1.8 notes carry the reasoning.)
`src/common/heapviz_ring.h` implements MPSC. Producers claim a slot with a CAS
on `head`, then publish that slot through two flag bits packed into
`HvEvent.op`:

- bit 7, commit: the slot is fully written.
- bit 6, lap parity: the commit belongs to the current lap, not the previous one.
- bits 0-5: the opcode.

The consumer stops draining at the first slot missing commit or carrying the
wrong parity. Commit alone is not enough, because a fully-committed event from
the previous lap looks valid; parity is what rejects it. Deleting the parity
check makes `ring_mpsc` fail immediately, which is the intended property.

`head` and `tail` sit on separate cache lines deliberately.

### The interceptor cannot allocate

`src/preload/preload.c` is C11 on purpose, not C++. A C++ shared object links
libstdc++, whose static initialisers allocate, and this library *is* the
allocator inside the target process. Keeping it C makes the guarantee structural
rather than a linker flag someone can drop; `preload_no_libstdcxx` enforces it,
and `preload_load` exists so that check cannot pass vacuously (`ldd` prints
"statically linked" for both a clean .so and a broken one).

Consequences to respect when editing that file:

- No `malloc`, `printf`, `dlerror`, or anything reaching them. Diagnostics go
  through `write(2)` and exits through `_exit`.
- `dlsym` itself calls `calloc`, so early allocations are served from a static
  bootstrap arena and freed pointers are checked against it before dispatch.
- Thread-locals use `__attribute__((tls_model("initial-exec")))`. Global-dynamic
  TLS allocates on first access inside a `dlopen`ed library.
- Ordering: allocating calls run the real function first and then emit. `free`
  emits *before* the real free, so another thread cannot reuse the address and
  publish its `Malloc` first. `realloc` emits `Free` then `Realloc`, and
  re-reports the original block on failure.

### Consumer attach

`src/tui/session.cpp` is the shipped consumer and `tests/support/ring_attach.h`
is the test-side twin of the same sequence: poll for the segment, map the
header alone, check magic then ABI version, read `capacity`, map the whole
ring, and claim it with `hv_ring_claim`. The order is the design, not a
convention — reading `capacity` before the magic matches is reading a field
that is not written yet, and the mapping length computed from it is garbage.
`Session` differs from the test helper only in giving each failure a name the
UI can print (`AttachStatus`), and in `kAttachTimeoutMs`: long enough for a
target still running its own static initialisers, short enough that a wrong
`--pid` is answered rather than waited on. The claim is exclusive because `tail`
is a single cursor: two consumers would each advance it past events the other
never saw, and both would display a plausible half of the stream. The producer publishes `magic` last with a release
store, so a non-matching magic means the constructor is still running and the
consumer should retry rather than fail.

The claim is a CAS on `consumer_pid`, not a flag, so a stale claim can be
attributed. Release it with `hv_ring_release` on a clean exit;
`hv_ring_break_claim` force-clears one, and the caller must confirm the owning
pid is gone first, because `heapviz_ring.h` is a shared header that cannot make
syscalls.

The launch path — `heapviz ./a.out` or `heapviz -- ./a.out` — has one failure the
attach cannot diagnose on its own. It
forks and execs with `LD_PRELOAD` injected, and if the exec fails there is no
target, no segment, and nothing to attach to — so the attach times out and
reports "target is not running libheapviz.so", which is a confident and
completely wrong description of "command not found". The close-on-exec pipe in
`launch_target` is how the parent learns which of the two happened.

### From a pid to a coordinate space

`--pid` and `--` walk four objects before the map sees anything, and each
answers a question the next one assumes:

| | Answers | |
|---|---|---|
| `Session` | is there a segment for this pid, is it this ABI, is it ours to read | `session.h` |
| `MapsScanner` | what shape is the address space — which regions could an allocation land in | `proc_maps.h` |
| `RegionMap` | where each region sits once they are laid end to end | `region_map.h` |
| `ChunkReader` | what the target's own chunk header says: overhead, and chunks allocated before attach | `chunk_reader.h` |

**The grid buckets flat offsets, not addresses.** A threaded target has a brk
heap at `0x5b…` and a 64 MiB-aligned arena per allocating thread somewhere in
`0x7f…`, and their union is about 23 TiB of which perhaps 40 MiB is memory: the
`Grid` clamps to 1 GiB cells, `covers_whole_span` goes false, and the display is
one occupied cell in a screenful of nothing. Picking a single region instead —
which is what M2.3 shipped — shows a threaded target's main arena, reliably the
empty one. So `RegionMap` packs the regions end to end in address order and
`HeapApp::repack` hands `Grid` the range `[0, total_bytes)`, which makes the
coordinate "how many heap bytes come before this one". Everything that shows a
user a number — the gutter, the inspector, the header — converts back through
`RegionMap::to_addr`. A place that forgets is the plausible-looking lie again,
and this time it has no visual symptom.

`MapsScanner` re-scans on a 500 ms timer, but the timer is not what keeps the
display correct: a heap that grows mid-frame is noticed by `note_address` when
an event lands outside the bounds, not by the tick that would have caught it
400 ms later. Its parser is hand-rolled because the format is fixed by the
kernel, and malformed lines are counted and skipped rather than failing the
scan — `/proc/<pid>/maps` is a seq_file generated while the target runs, so a
torn read must cost one region for 500 ms, never a blank display.

`ChunkReader` is allowed to fail. `process_vm_readv` needs the same uid, and
`ptrace_scope = 1` blocks it for a non-descendant even then, so `EPERM` is a
supported mode rather than an error path: overhead reads unavailable and the
session carries on with interceptor data. Reads are batched into one syscall
because a per-chunk read is a syscall in a loop over the live set. It never
calls `PTRACE_ATTACH`, which would stop the process being measured.

`HeapApp` is the real `LoopApp`; `DemoHeap` behind `--term-check` is the
synthetic one. They drive the same `Grid`/`HeatMap`/`MapView` and differ only in
what feeds it, which is the point: if attaching to a real target had needed
changes in `Grid` or `HeatMap`, one of them was holding an assumption about the
synthetic heap. Three things are true only of the real one, and they explain
code that otherwise reads as over-defensive:

- **The heap moves.** New bounds are a new granularity, so they go through
  `HeatMap::configure` + `rebuild`, never an incremental fold.
- **The past is not observable.** heapviz attaches to a process that already has
  a heap, so the first thing it sees may be a `free` of a chunk it never saw
  allocated. Every counter one of those could decrement saturates.
- **The target dies**, often mid-frame. The session freezes what it has rather
  than clearing it: the last state of a heap that has just exited is the most
  interesting frame of the session.

The drain is capped at `kMaxEventsPerFrame`, because absorbing a full 1 Mi-slot
ring in one frame at ~30 ns an event is a 30 ms stall, and a paced loop that
misses its deadline by thirty frames is indistinguishable from a hung one.

### Segments outlive processes

Segments are `/heapviz_shm_<pid>`, mode 0600, unlinked from the interceptor's
destructor — which `SIGKILL` skips, so killed targets leave rings in `/dev/shm`
sized by `HEAPVIZ_CAPACITY`. A few kills during development add up to real
tmpfs; `heapviz --cleanup` (`src/tui/shm_cleanup.cpp`) reaps only the segments
whose producer pid is gone, so it is safe while other targets are being
profiled. The reaper lives on the TUI side because `opendir`/`readdir` allocate,
which ground rule #1 forbids inside the hook.

### The map is a pipeline, and each stage owns one decision

Six objects in `src/tui/` turn a stream of events into a screenful of colour.
They are layered strictly downwards — each knows the ones above it and nothing
below — so the place to add something is wherever the decision it needs already
lives:

| | Owns | Costs |
|---|---|---|
| `Grid` | coordinate → cell. `cell_bytes` = next power of two ≥ `span / (cols*rows)`, clamped to [64 B, 1 GiB], so the mapping is a shift | a shift |
| `ChunkTable` | which allocations are live, by pointer. Robin Hood, Fibonacci hashing, bounded memory | 25 ns lookup |
| `HeatMap` | per-cell aggregates, folded in incrementally | ~30 ns/event |
| `HeatRamp` | (aggregate, now) → `Rgb`, in Oklab | 6.7 ns settled, 46 ns animating |
| `MapView` | what fits on screen, and the glyphs | ~25 ns/cell |
| `MapCursor` | which cell the user is pointing at, held as a *coordinate* so a resize reflows under it | one shift per query |

The coordinate the `Grid` buckets is a packed offset under a real target and a
plain address under `DemoHeap` — see "From a pid to a coordinate space". The
optional `RegionMap` that decides which hangs off `HeatMap`
(`set_regions`/`regions()`), and null there is not a degraded mode: with no
`RegionMap` the address *is* the coordinate, which is what `DemoHeap` and every
test written before M2.4 assume. `HeatMap::coordinate_of` converts on the way in
and returns false for an address in a region not scanned yet, so it is dropped
rather than landing in an unrelated cell; `MapView` reads the map back out
through `map.regions()` to label the gutter, and `ChunkInspector` is handed it
directly. The gutter labels an offset *within* the row's region rather than from
the top of the map, and marks the seams, because an offset that runs across a
seam between two arenas terabytes apart is arithmetically consistent and
describes nothing.

Three properties hold the whole thing together, and each has a test that fails
when it is broken:

**One function decides the granularity.** `Grid::configure` is the only code
that computes `cell_bytes`; `set_bounds` and `set_viewport` are two names for
it, so a resize and a growing heap cannot arrive at different answers. One level
up, `map_layout` in `map_view.cpp` is the only code that decides how many cells
fit — the drawing area minus the address gutter minus the legend row — and
`fit_grid` is how a caller applies it. A gutter that exists in the layout but
not in the draw does not shift the display by a column; it labels row 7 with the
address of row 6, which is a plausible-looking lie.

**Colour is a pure function of (aggregate, now), never stored.** Cells age
because the clock moved, not because anything swept them, so there are no timers
and nothing to keep in sync. The corollary is that the map must repaint every
cell every frame — which is affordable at 6.7 ns settled, and which M4.3's
differ turns back into zero bytes when the colours came out the same.

**A granularity change invalidates every aggregate.** `HeatMap::configure`
returns true when a rebuild is needed, and `rebuild(table)` recomputes from the
chunk table — the only correct response, and the only place the whole grid is
touched. Everything else is incremental.

Density is encoded twice, in the glyph *and* in the colour's brightness, because
M4.4 degrades colour and `--no-unicode` degrades glyphs independently: on a
16-colour terminal the shading carries it, and on a font with no block-drawing
characters the colour does.

`Grid::covers_whole_span()` is false when a span needs cells larger than 1 GiB.
The legend says so rather than showing part of the heap as though it were all of
it.

### The bottom block holds two panels that cannot see each other

`ChunkInspector` (M5.2) and `Metrics` (M5.3) share the six rows between the map
and the footer. `metrics_split` in `metrics.h` is the only code that decides
where the boundary is — it starts from the mockup's 60/40 and lets the two
panels' minimum widths override it, and returns 0 when both cannot be met, in
which case the inspector takes the block whole. `HeapApp::metrics_cols` is the
only caller.

Both panels draw through `src/tui/panel.h`, never through `Framebuffer` directly,
and the reason is not style: `Framebuffer::text` clips to the *screen*, so a
value longer than its panel is a perfectly legal write that lands in the
neighbour's labels. `panel_text` and `panel_text_right` clip to the rect and take
offsets relative to it, which is the form in which the bug cannot be written.
`panel.h` also owns the inset rule and `format_count`.

`Metrics` owns only the two figures nothing else keeps — the cumulative total
and the peak. Everything else on that panel is fed in from `HeapApp`, which has
already got the live set right; the recycled-address and saturating-decrement
reasoning in `HeapApp::apply` is delicate enough that a second copy of it would
be a second thing to keep correct. The peak is sampled per frame rather than
folded per event, because memory allocated and freed between two frames was
never on screen and a peak that counted it would report bytes the process may
never have held at once.

The fragmentation figure is fed in too, from `FragAnalyzer` (M5.4) on its own
4 Hz tick, through `set_fragmentation` and `set_largest_gap`. They are separate
setters because they have different availability, and that difference is the
design:

- **The percentage costs no sort.** `sum of gaps == (last end - first start) -
  sum of footprints`, so one linear pass over the chunk table gives it exactly,
  with no ordering and no allocation. It is available on every heap.
- **The largest hole cannot be had that way**, so it is the only part that pays
  for `std::sort`, and the only part with a cost bound: above `kFragMaxSorted`
  records it is not computed and `largest_gap_known` goes false, which makes the
  panel drop the field rather than print a zero reading as "no holes".

Two things there are load-bearing and easy to undo. Spans and gaps accumulate
**per region**, because one span across a threaded target's arenas is 23 TiB of
which 40 MiB is memory — 99.99% forever, on most real targets. And a chunk's
footprint is `usable + kChunkMinOverheadBytes`, because ptmalloc's in-use word
sits *below* the reported pointer: drop it and every allocation in the heap gains
an 8-byte hole in front of it, which is 25% on a heap of 32-byte requests that
has nothing wrong with it.

### The TUI is one thread on a frame deadline

`src/tui/event_loop.cpp` owns the only loop: drain the ring, update the model,
draw, diff, write, then `poll(2)` on stdin until the next deadline. There is no
render thread, which is what lets the ring stay single-consumer.

The application side is the abstract `LoopApp` (`drain` / `update` / `key` /
`animating` / `resized` / `draw`). Everything the loop touches outside the
process is a `LoopConfig` field — both file descriptors, `ioctl(TIOCGWINSZ)` as
`size_fn`, `write(2)` as `writer` — which is why
`tests/unit/event_loop_test.cpp` needs no terminal.

Four things there are easy to break by accident:

- **A frame that changes nothing must produce no bytes.** The draw, the diff and
  the write are all skipped, and `Renderer::render` omits even the trailing SGR
  reset. Idle cost is the feature, not an optimisation.
- **`SIGWINCH` is only handled while a `TerminalGuard` is active**, since that is
  what installs the handler. `tests/integration/event_loop_pty_test.cpp` exists
  because nothing in-process notices if that coupling is severed.
- **Buffers are reallocated at exactly one point**, the top of a frame, before
  the drain. A resize noticed anywhere else would move storage out from under a
  half-drawn frame.
- **Quitting is the application's decision, not the loop's.** The loop feeds
  every byte to `LoopApp::key` and nothing else; `q` means quit only because
  `TermCheckApp::key` calls `hv::request_quit()`. A `LoopApp` that forgets it
  runs to `max_frames` and looks like a hung test rather than a missing
  keybinding.

There are two `LoopApp`s and they share one keymap only in part. Both route
cursor keys through `cursor_move_for_key`, so `hjkl`, `HJKL`, `g`/`G` and
`n`/`N` behave identically in each — a binding added there lands in both, which
is the point of it being a free function. Beyond that they diverge:
`TermCheckApp` has `a` and `t`, and `HeapApp` has `Tab`, `Space`, `?`, `r` and
M5.5's `s`/`S`/`d`. `Tab` cycles the chunks sharing the cursor's cell (a cell is
a span of addresses and usually holds several, so without it the inspector could
only ever name the largest); `s` marks a snapshot, `d` diffs against it and `S`
drops it, and `d` before `s` is deliberately inert rather than an error.
`HeapApp::key` takes `q` first and unconditionally, because the cursor bindings
are vim's and vim has no `q` — a future mode that binds it would otherwise take
the one key the loop has no opinion about, and a heapviz that cannot be quit is
a terminal that has to be killed. A cursor move also refreshes the inspector
immediately rather than on the next 4 Hz tick: the panel is the answer to the
keypress, and a quarter-second lag reads as the tool being slow.

`Space` is not allowed to stop the consumer. While the display is paused,
`HeapApp::drain` removes events from the shared ring into `staged_` but reports
no visible change; `update` and heat aging stop. Resume applies staged events
before newer ring entries so an allocation/free pair cannot be reversed. `?`
is modal, and `r` resets cumulative telemetry plus `EventLoop` diagnostics
without clearing the live heap. Keep each mode's footer aligned with the keys
that `HeapApp::key` will actually accept.

## Naming and layout

One rule runs through the whole project: **`heapviz_` / `HEAPVIZ_` marks the
public surface, `hv_` / `HV_` marks internals.**

| | Public | Internal |
|---|---|---|
| C functions | `heapviz_preload_abi_version` (exported, `HV_EXPORT`) | `hv_ring_push`, `hv_emit` (`static` or hidden) |
| Macros | `HEAPVIZ_ABI_MAGIC`, `HEAPVIZ_ABI_VERSION` (wire contract) | `HV_OP_MASK`, `HV_CACHELINE` |
| Headers | `src/common/heapviz_abi.h`, `heapviz_ring.h` (vendorable) | `src/tui/renderer.h`, `terminal.h` |

So a `static` function must never wear `heapviz_`, and anything a separately
compiled binary depends on must never wear `hv_`. The C++ side sits inside
`namespace hv` and needs no prefix: types are `PascalCase` (`Framebuffer`,
`Cell`), functions and methods are `snake_case`, and constants are `kCamelCase`.
Include guards are `HEAPVIZ_<DIR>_<FILE>_H`.

`tests/` is grouped by kind, and the grouping is the answer to "where does my
new test go":

| Directory | Holds |
|---|---|
| `unit/` | In-process. No child processes, no `LD_PRELOAD`. Usually fast, but `ring_mpsc` and `frame_budget` are minutes-long stress and timing runs — the criterion is the process boundary, not the clock. |
| `integration/` | Crosses a process boundary: `fork`/`exec`, a pty, or the preload. |
| `fixtures/` | Programs that exist to be measured, not to assert. |
| `support/` | Shared helpers, no `main()`. Included as `"support/..."`. |
| `cmake/` | CTest driver scripts, each named after the test it drives. |

A test's CTest name is its file stem with any `_test` suffix removed, so
`ctest -R renderer` and `tests/unit/renderer_test.cpp` always correspond. Where
one test needs several programs they share a stem and take a role suffix
(`shm_roundtrip_writer` / `shm_roundtrip_reader`).

## Things that have bitten this codebase

Wall-clock time cannot tell sleeping from spinning. Every loop that claims to
idle cheaply hits its deadlines identically whether it slept or burned a core
getting there, so the pacing tests compare `getrusage` against elapsed time.
Two real bugs hid behind wall clock alone: polling an fd that is permanently
readable, and truncating the `poll` timeout to zero milliseconds (`poll` then
returns `0` at once, which the loop reads as "the deadline arrived", so frames
end early *and* spin).

A paced loop costs about 1.5x what the same work costs in a tight loop, and the
difference is not overhead anyone can delete. heapviz sleeps ~16 ms per frame,
and its working set — two 160 KB cell buffers, a 65 KB output buffer, 10,000
heat aggregates — does not survive that in cache, so every frame starts cold.
`frame_budget` measures 450 µs of work but 700 µs of CPU per frame for exactly
this reason. Benchmark the paced loop when the claim is about what heapviz
costs; benchmark a tight loop only when the question is which phase got slower.

Take the best of several runs, not the mean, when asserting a performance
ceiling. Every source of noise here is additive — another process on the core, a
migration, the cold-cache effect landing worse on one run — so the cheapest run
is the least contaminated, while a real regression is in every run and moves the
minimum too. Print the spread alongside: on a shared machine these vary by 25%,
and a wall-clock gate honest enough not to flake will not catch a regression
smaller than about 50%. That limit is worth stating in the test rather than
tightening the gate until it flakes.

`-O3` deletes allocations. GCC removes an alloc/write/free sequence whose
contents are never read, which silently emptied the mmap path in release builds
and made a test assert on behaviour that no longer existed. Workload code in
`examples/` writes through a `volatile` sink for this reason. If a test passes
in debug and fails in release, suspect this before suspecting the ring.

Strict `-std=c11` with `CMAKE_C_EXTENSIONS OFF` hides every POSIX declaration.
The `heapviz_abi` INTERFACE target carries `_POSIX_C_SOURCE=200809L` for
`shm_open`, `ftruncate`, `clock_gettime`, and `mmap`. `_GNU_SOURCE` is separate
and set per target where `RTLD_NEXT`, `malloc_usable_size`, `memalign`, or
`pvalloc` are needed. Note that `usleep` is gone in POSIX.1-2008; use
`nanosleep`.

Warnings are errors, and the set includes `-Wconversion` and `-Wsign-conversion`.

## Working practices for this repo

Update `ROADMAP.md` in the same change that completes a task, including the
per-milestone counts and the total. A tracker that has drifted from reality is
worse than none. Use `[-]` and write down the reasoning when cutting a task
rather than quietly ticking it.

Add user-visible changes to `[Unreleased]` in `CHANGELOG.md` in the commit that
makes them, written for someone using heapviz rather than someone reading the
diff. Keybinding changes count as breaking.

When adding a test that guards an invariant, verify it can fail. Break the thing
it protects, watch the test go red, then restore. Several guards here passed
vacuously until that was done.

Every `.c`, `.h`, and `.cpp` file in the repo opens with the same four-line
block, without exception — a one-line description naming the milestone that
introduced it, a blank comment line, `Copyright (C) 2026 Parth Sinha`, and
`SPDX-License-Identifier: GPL-3.0-or-later`. New files carry it too.

Commit subjects are `M<milestone>.<task>: <lowercase summary>` when the commit
completes a roadmap box (`M3.3: per-cell aggregation`), and the body explains
the decision rather than restating the diff.

Comments here explain *why*, at some length, and the existing files set the
density: `preload.c` and `event_loop.cpp` justify their ordering constraints in
prose. Match that. A comment that says what the next line does is noise; one
that says which bug the line prevents is the reason this codebase is
maintainable.
