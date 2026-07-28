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

`heap-doc.md` is the design intent. `ROADMAP.md` is the executable plan: 209
checkboxes across M0 to M7, with a progress table at the top that must be kept
accurate. Read the roadmap's "Ground rules" section before touching the
interceptor or the render path; those six invariants are what make the design
work, and violating one is a bug even when the tests pass.

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
| `heapviz_core` | `libheapviz_core.a` | terminal/framebuffer/renderer/loop/cleanup; the tests link this so they exercise the shipped objects, not a recompile |

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

Attach (`--pid`, `-- <cmd>`) is M2.3 and unimplemented; those arguments exit 2.
The four that work:

```bash
./build/debug/heapviz --version                      # pinned by the tui_version test
./build/debug/heapviz --term-check                   # drive M4.1-M4.5 against a real terminal
./build/debug/heapviz --term-check --debug-timing    # per-phase frame budget overlay
./build/debug/heapviz --cleanup                      # reap rings left by SIGKILLed targets
```

`--term-check` is the manual counterpart to the pty tests: it is the only way to
see whether a resize flickered, and `a` toggles animation so the idle path's
skipped-frame counter can be watched doing its job.

### Preset differences that matter

`asan` builds the TUI and tests only. ASan supplies its own malloc, so it cannot
share a process with an `LD_PRELOAD` malloc hook; every preload-driven test is
guarded by `if(NOT HEAPVIZ_ASAN)`. `interceptor_overhead` additionally only runs
on optimised builds, because at `-O0` the interceptor costs 45-56 ns and
straddles its own 50 ns budget.

Expected test counts when everything passes: debug 15, release 16, asan 13.

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

The roadmap's M1.6 text still describes an SPSC ring, as does the alt text on
`README.md`'s "How it works" diagram. Both are stale: every thread in the target
that calls `malloc` is a producer.
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

`tests/support/ring_attach.h` is the reference implementation of the consumer
side and mirrors what the TUI will do in M2.3: poll for the segment, map the
header alone, check magic then ABI version, read `capacity`, map the whole
ring, and claim it with `hv_ring_claim`. The claim is exclusive because `tail`
is a single cursor: two consumers would each advance it past events the other
never saw, and both would display a plausible half of the stream. The producer publishes `magic` last with a release
store, so a non-matching magic means the constructor is still running and the
consumer should retry rather than fail.

The claim is a CAS on `consumer_pid`, not a flag, so a stale claim can be
attributed. Release it with `hv_ring_release` on a clean exit;
`hv_ring_break_claim` force-clears one, and the caller must confirm the owning
pid is gone first, because `heapviz_ring.h` is a shared header that cannot make
syscalls.

### Segments outlive processes

Segments are `/heapviz_shm_<pid>`, mode 0600, unlinked from the interceptor's
destructor — which `SIGKILL` skips, so killed targets leave rings in `/dev/shm`
sized by `HEAPVIZ_CAPACITY`. A few kills during development add up to real
tmpfs; `heapviz --cleanup` (`src/tui/shm_cleanup.cpp`) reaps only the segments
whose producer pid is gone, so it is safe while other targets are being
profiled. The reaper lives on the TUI side because `opendir`/`readdir` allocate,
which ground rule #1 forbids inside the hook.

### The TUI is one thread on a frame deadline

`src/tui/event_loop.cpp` owns the only loop: drain the ring, update the model,
draw, diff, write, then `poll(2)` on stdin until the next deadline. There is no
render thread, which is what lets the ring stay single-consumer.

The application side is the abstract `LoopApp` (`drain` / `update` / `key` /
`animating` / `resized` / `draw`); M3's heap map will be an implementation of
it. Everything the loop touches outside the process is a `LoopConfig` field —
both file descriptors, `ioctl(TIOCGWINSZ)` as `size_fn`, `write(2)` as
`writer` — which is why `tests/unit/event_loop_test.cpp` needs no terminal.

Three things there are easy to break by accident:

- **A frame that changes nothing must produce no bytes.** The draw, the diff and
  the write are all skipped, and `Renderer::render` omits even the trailing SGR
  reset. Idle cost is the feature, not an optimisation.
- **`SIGWINCH` is only handled while a `TerminalGuard` is active**, since that is
  what installs the handler. `tests/integration/event_loop_pty_test.cpp` exists
  because nothing in-process notices if that coupling is severed.
- **Buffers are reallocated at exactly one point**, the top of a frame, before
  the drain. A resize noticed anywhere else would move storage out from under a
  half-drawn frame.

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
| `unit/` | In-process and fast. No child processes, no `LD_PRELOAD`. |
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
