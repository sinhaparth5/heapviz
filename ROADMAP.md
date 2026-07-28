# heapviz roadmap and progress tracker

Derived from `heap-doc.md`. That document is the *design intent*; this one is the
*executable plan*. Every box here is small enough to finish in one sitting and
concrete enough that "done" is not a judgement call.

**Status legend:** `[ ]` not started · `[~]` in progress · `[x]` done · `[!]` blocked · `[-]` cut

---

## 0. Progress at a glance

| # | Milestone | Scope | Status | Done |
|---|-----------|-------|--------|------|
| M0 | Scaffold & shared ABI | build, layout, IPC contract | `[x]` | 19 / 19 |
| M1 | Zero-overhead interceptor | `libheapviz.so` | `[x]` | 38 / 39 |
| M2 | Kernel & memory parsing | `/proc`, ptmalloc headers | `[ ]` | 0 / 17 |
| M3 | Sparse address representation | grid, hash table, aging | `[x]` | 24 / 24 |
| M4 | ANSI terminal engine | raw mode, double buffer, diff | `[x]` | 34 / 34 |
| M5 | Interactivity & analysis | cursor, frag, snapshots | `[ ]` | 0 / 33 |
| M6 | Visual polish | the *beautiful* part | `[ ]` | 0 / 20 |
| M7 | Hardening & release | perf, tests, docs, packaging | `[ ]` | 0 / 23 |

**Total: 115 / 209**

Update the counts when you tick boxes. If a count drifts from reality, the
tracker is worthless. Keep it honest.

---

## 1. Ground rules (apply to every milestone)

These are the invariants that make the design work. Violating one is a bug even
if the tests pass.

1. **The interceptor never calls `malloc`.** Not directly, not transitively, not
   through `printf`, not through `std::string`, not through `dlerror`. Any
   allocation inside the hook is a re-entrancy deadlock waiting for a
   multi-threaded target.
2. **The interceptor never blocks.** Ring buffer full ⇒ drop the event, bump a
   counter, return. The target process is a guest we do not get to inconvenience.
3. **The TUI never writes to stdout more than once per frame.** One `write(2)`.
   Partial writes are looped, not ignored.
4. **The terminal is always restored.** Every exit path (clean quit, `SIGINT`,
   `SIGTERM`, `SIGSEGV`, uncaught exception) restores termios, leaves the
   alternate screen, and un-hides the cursor. A profiler that bricks your shell
   is worse than no profiler.
5. **Nothing in the render path allocates.** Buffers are sized on resize, not
   per frame.
6. **The shared-memory layout is a versioned ABI.** Two separately compiled
   binaries read it. Treat any change to it like a wire protocol change.

---

## M0. Project scaffold & shared ABI

*Goal: two binaries that build, and a single header both of them agree on.*

Nothing in `heap-doc.md` covers this, but M1 cannot start without it.

### M0.1 Build system

- [x] `CMakeLists.txt` at root, `cmake_minimum_required(VERSION 3.20)`,
      `project(heapviz LANGUAGES C CXX)`, C++20 / C11.
- [x] Target `heapviz`, the TUI binary. Links `pthread`, `rt` (for `shm_open`
      on older glibc).
- [x] Target `heapviz_preload`: `SHARED`, output name `libheapviz.so`.
      Links `dl`, `pthread`, `rt`.
- [x] Preload target compiled with `-fPIC -fvisibility=hidden
      -fno-exceptions -fno-rtti -O2`. Only the interposed symbols are
      `__attribute__((visibility("default")))`.
- [x] Preload target must **not** link `libstdc++` (`set_target_properties(...
      LINKER_LANGUAGE C)` or link `-nodefaultlibs` + explicit libs). Verify with
      `ldd libheapviz.so`; libstdc++ pulls in static init that allocates.
- [x] `-Wall -Wextra -Werror` on both targets. Sanitizer preset
      (`-DHEAPVIZ_ASAN=ON`) for the TUI only. ASan and an LD_PRELOAD malloc
      hook in the same process do not coexist.
- [x] `CMakePresets.json` with `debug`, `release`, `asan` presets.

### M0.2 Source layout

- [x] `src/common/`: headers shared by both binaries. No `.cpp` files that
      link into the preload lib unless they honour ground rule #1.
- [x] `src/preload/`: interceptor.
- [x] `src/tui/`: renderer, parsers, analysis.
- [x] `examples/`: target programs to profile.
- [x] `tests/`: unit tests.

### M0.3 The shared ABI (`src/common/heapviz_abi.h`)

Get this file wrong and you get silent corruption across process boundaries,
so it is worth more care than anything else in M0.

- [x] `HEAPVIZ_ABI_MAGIC` (8-byte constant, e.g. `0x48505A5631000000` = "HPZV1")
      and `HEAPVIZ_ABI_VERSION`. Consumer refuses to attach on mismatch and says
      so in plain English.
- [x] `struct Event`, exactly 32 bytes, standard layout, no padding
      surprises:

      | Offset | Size | Field       | Notes                                    |
      |--------|------|-------------|------------------------------------------|
      | 0      | 8    | `timestamp` | `CLOCK_MONOTONIC` nanoseconds            |
      | 8      | 8    | `ptr`       | `uintptr_t`, user pointer                |
      | 16     | 8    | `size`      | requested bytes (0 for `free`)           |
      | 24     | 4    | `usable`    | `malloc_usable_size` result, truncated   |
      | 28     | 3    | `tid`       | low 24 bits of thread id                 |
      | 31     | 1    | `op`        | `enum Op : uint8_t`                      |

- [x] `static_assert(sizeof(Event) == 32)` and a `static_assert` on every
      field offset via `offsetof`. Both binaries compile this header, so a
      layout drift is a compile error rather than a runtime mystery.
- [x] `enum class Op : uint8_t { Malloc, Free, Calloc, Realloc, Memalign }`.
- [x] `struct RingHeader`, cache-line aware:
      - `alignas(64)` block: magic, version, pid, capacity (power of two),
        `event_size`, process start time, target `comm` string (16 bytes).
      - `alignas(64)` block: `std::atomic<uint64_t> head` (producer only).
      - `alignas(64)` block: `std::atomic<uint64_t> tail` (consumer only).
      - `alignas(64)` block: `std::atomic<uint64_t> dropped`, `total_events`.
      The three hot blocks sit on separate cache lines; head/tail false sharing
      is the classic SPSC performance bug.
- [x] `heapviz_shm_name(pid)` produces the canonical `/heapviz_shm_<pid>`
      string, used by both sides. One function, no string duplication.
- [x] Total mapping size helper: `sizeof(RingHeader) + capacity * sizeof(Event)`,
      rounded up to page size.

**Definition of done:** `cmake --build build` produces `heapviz` and
`libheapviz.so`; `ldd libheapviz.so` shows no `libstdc++`; a throwaway test
maps the shm region from both binaries and agrees on every offset.

**Done.** 5 tests green on the `debug`, `release`, and `asan` presets. Where the
implementation diverged from the plan above:

- **The interceptor is C11, not C++.** The plan called for `-fno-exceptions
  -fno-rtti`; those are C++ flags. Building `src/preload/` as C makes the
  "no libstdc++" guarantee structural rather than a linker flag someone can drop
  later. The `preload_no_libstdcxx` test enforces it either way.
- **`std::atomic<uint64_t>` became a dual-language macro.** Since the two halves
  are now C11 and C++20, `heapviz_abi.h` selects `_Atomic` or `std::atomic`. The
  `abi_layout_matches` test compiles the same layout dump both ways and requires
  byte-identical output, which is what proves the two agree.
- **`ptr` is `uint64_t`, not `uintptr_t`.** A wire format wants a fixed width
  independent of the compiling platform.
- **`enum class Op : uint8_t` became plain enum constants** plus a `uint8_t op`
  field, so C can name them.
- **`RingHeader` gained `flags` and `producer_exited`,** the latter required by
  M1.7. Header is 256 bytes across four cache lines.
- **`_POSIX_C_SOURCE=200809L` is set project-wide.** Extensions are off
  (`-std=c11`, not `gnu11`), which hides `shm_open`, `ftruncate`, and
  `clock_gettime` behind the feature-test macro.
- **`examples/hello_alloc.c` added** so `examples/` holds something real. The
  full configurable workload is still M1.8.
- **Extra test `preload_loads`** dlopens the `.so` and checks the version probe.
  Without it, `preload_no_libstdcxx` passes vacuously: `ldd` prints
  "statically linked" for a clean dependency-free library and for a broken one
  alike.

Both guards were mutation-tested: widening an `HvEvent` field fires the static
asserts, and perturbing one writer value fails `shm_roundtrip`.

---

## M1. Zero-overhead interceptor (`libheapviz.so`)

*Goal: hook memory calls without slowing, deadlocking, or crashing the target.*

### M1.1 The bootstrap problem (do this first; it is the hard part)

`dlsym(RTLD_NEXT, "malloc")` calls `calloc` internally in glibc on first use.
Your hook is therefore called *before it knows what the real malloc is*. Every
naive LD_PRELOAD malloc hook dies here.

- [x] Static bump-allocator arena: `static char g_bootstrap[64 * 1024];` plus an
      atomic offset. Serves allocations while `g_real_malloc == nullptr`.
- [x] `heapviz_is_bootstrap_ptr(p)`: pointer-range check against the arena.
- [x] `free()` / `realloc()` must call that check first and no-op (or
      bump-copy) for bootstrap pointers. Passing them to the real `free` is an
      instant abort.
- [x] Bump allocator honours 16-byte alignment.
- [x] Arena exhaustion is a hard, loud failure (`write(2)` to stderr + `_exit`),
      not silent corruption. 64 KB is generous; if it exhausts, something is
      wrong.

### M1.2 Symbol resolution

- [x] `resolve_symbols()`: one-shot, guarded by `std::atomic_flag` or
      `pthread_once`, resolving: `malloc`, `free`, `calloc`, `realloc`,
      `posix_memalign`, `aligned_alloc`, `memalign`, `valloc`, `pvalloc`,
      `malloc_usable_size`.
- [x] Store as plain function pointers in file-scope statics, not `std::function`.
- [x] `__attribute__((constructor(101)))` init hook, which runs before most
      user constructors, so the ring is live for early allocations.
- [x] Handle `dlsym` returning `NULL` for optional symbols (`pvalloc` is not
      everywhere) without aborting.

### M1.3 Re-entrancy guard

- [x] `static __thread bool g_in_hook __attribute__((tls_model("initial-exec")));`
      Not `thread_local` on a non-trivial type: dynamic TLS allocates on
      first access in a dlopen'd library, and you are inside malloc.
- [x] RAII-free guard (plain set/clear around the telemetry block) since the
      preload lib is built `-fno-exceptions`.
- [x] Guard covers telemetry only. The pass-through to the real allocator
      happens outside it, so a guarded call still returns correct memory.

### M1.4 Interposed functions

Each one: call real function → if `!g_in_hook`, set guard, emit event, clear
guard → return. Never the other order; the pointer must exist before it is
reported.

- [x] `malloc(size)`
- [x] `free(ptr)`: bootstrap check, `NULL` check, emit `Op::Free`
- [x] `calloc(n, size)`: overflow check on `n * size`; must work during
      bootstrap
- [x] `realloc(ptr, size)`: see open decision D1 below
- [x] `posix_memalign(out, align, size)`
- [x] `aligned_alloc(align, size)`, `memalign`, `valloc`, `pvalloc`
- [x] Confirm `strdup`/`asprintf`/`getline` are covered transitively (they call
      the interposed `malloc` through the PLT). Write a test, do not assume.

### M1.5 Event emission

- [x] `clock_gettime(CLOCK_MONOTONIC, ...)`: vDSO, no syscall. Benchmark it;
      if it exceeds ~25 ns, fall back to `CLOCK_MONOTONIC_COARSE` or TSC.
- [x] `malloc_usable_size(ptr)` inline. Gives the real allocator overhead at
      no cost, inside the process, with no ptrace and no `/proc` read. It
      skips most of M2.2.
- [x] Thread id via cached `__thread` `gettid()` result. Never call `gettid()`
      per event.
- [x] Single 32-byte struct write into the ring slot; no memcpy of parts.

### M1.6 SPSC lock-free ring buffer

- [x] Capacity is a compile-time-configurable power of two (default 1 MiB
      events = 32 MiB). Index with `& (capacity - 1)`, never `%`.
- [x] Producer: `head` loaded `relaxed` (we are the only writer), `tail` loaded
      `acquire`, slot written, `head` stored `release`. The release store is
      what publishes the payload. Get this wrong and the consumer reads torn
      data on ARM.
- [-] Producer caches the last-seen `tail` in a local and only re-reads the
      atomic when the cached value says "full". **Cut.** The optimisation
      assumes one producer with somewhere to keep the cache. With a CAS claim
      loop the cache would have to be thread-local, inside a header the consumer
      also compiles, and a stale value causes false "full" readings that drop
      events for no reason. Measured overhead is 31 ns against a 50 ns budget,
      so it buys nothing we need. Revisit if the budget ever gets tight.
- [x] Full ⇒ `dropped.fetch_add(1, relaxed)` and return. Never spin, never
      block.
- [x] Consumer: `tail` `relaxed`, `head` `acquire`, read, `tail` `release`.
- [x] Consumer drains in batches (read up to N events per frame) rather than one
      at a time.

### M1.7 Shared memory setup

- [x] `shm_open("/heapviz_shm_<pid>", O_CREAT | O_EXCL | O_RDWR, 0600)`.
      `O_EXCL` so a stale segment is detected rather than silently reused.
- [x] On `EEXIST`: unlink the stale segment and retry once, then give up loudly.
- [x] `ftruncate` to the computed size, `mmap(PROT_READ | PROT_WRITE,
      MAP_SHARED)`, then `close(fd)`; the mapping outlives the descriptor.
- [x] Write `RingHeader` fields, then publish `magic` last with a release
      store. The magic is the "region is ready" flag.
- [x] `__attribute__((destructor))` + `atexit` ⇒ `shm_unlink`. Set a "producer
      exited" flag in the header first so the TUI can report it.
- [x] `pthread_atfork` child handler: the child has a new PID and inherits the
      parent's mapping. Either detach (simplest, ship this) or create a fresh
      segment. Document which.

### M1.8 Verification

- [x] `examples/churn.c`: configurable allocation workload: steady rate, bursty,
      fragmenting (alloc many, free every other), large-mmap path (>128 KB
      triggers `mmap` not `brk`), and multi-threaded.
- [x] Standalone `tests/ring_test`: two threads, 10M events, assert zero loss
      and correct ordering under TSan.
- [x] `tests/integration/interceptor_overhead_test.c`: same workload with and without `LD_PRELOAD`,
      report ns/alloc delta. Target: under 50 ns added per allocation. Record
      the measured number here: `______`.
- [x] Run the churn example under `LD_PRELOAD` for 60 s with no consumer
      attached. Must not crash, must not grow memory, `dropped` climbs.

**Definition of done:** `LD_PRELOAD=./libheapviz.so ./examples/churn` runs
clean under a multi-threaded workload, `dropped == 0` when a consumer is
draining, overhead is measured and recorded.

**Done.** 9 tests green on `debug` and `release`, 6 on `asan` (the preload-driven
tests cannot run there; ASan supplies its own malloc). Measured on GCC 15.2,
x86-64:

| Measurement | Result | Budget |
|---|---|---|
| Added per allocator call | **31.13 ns** | 50 ns |
| Baseline malloc/free | 3.29 ns/call | — |
| 4-thread e2e capture | 3.9M events, 0 dropped | — |
| 60 s, no consumer, ~118M calls | exit 0, RSS flat at 42 MB | no growth |

**The ring is multi-producer, not SPSC.** This is the one real departure from the
plan. M1.6 says to load `head` relaxed "because we are the only writer", but every
thread in the target that calls `malloc` is a producer, and two threads claiming
the same `head` overwrite each other's slot. Since M1.8 requires a clean run
under a multi-threaded workload, SPSC was not implementable as written.

The replacement claims slots with a CAS loop and publishes each slot through two
flag bits in the `op` byte (commit + lap parity), which keeps `HvEvent` at 32
bytes. `heapviz_ring.h` documents the ordering. This bumped the ABI to **v2**.
Removing the parity check makes `ring_mpsc` fail immediately with stale
previous-lap events, so the bit is load-bearing rather than decorative.

Other deviations:

- **`HEAPVIZ_WAIT_MS` added** (with a `consumer_attached` field at offset 212).
  A short-lived target finishes and unlinks its segment before anyone can
  attach, which made the release-build tests fail and would have made
  `heapviz -- ./prog` miss every early allocation. The constructor now optionally
  waits for a consumer. M2.3 needs this anyway.
- **`HEAPVIZ_CAPACITY` and `HEAPVIZ_DISABLE`** env knobs, read once in the
  constructor.
- **`free` emits before calling the real `free`.** Reporting afterwards lets
  another thread reuse the address and publish its `Malloc` first, so the
  consumer would see two live allocations at one address.
- **`realloc` emits `Free` then `Realloc`,** and re-reports the original block if
  `realloc` fails, so a failed call does not look like a death.
- **Tests found two bugs in my own test code, not the interceptor:** `getline`
  reuses its buffer (so counting calls overcounts allocations), and `-O3` deletes
  an alloc/write/free sequence whose contents are never read, which silently
  removed the entire mmap path from release builds.

---

## M2. Kernel & memory parsing

*Goal: know where the heap lives and what the allocator's real footprint is.*

### M2.1 Virtual memory segment scanner

- [ ] `/proc/<pid>/maps` line parser: `start-end perms offset dev inode path`.
      Hand-rolled, no `sscanf` in the hot path (it is called on a timer).
- [ ] Classify regions: `[heap]` (brk), anonymous `rw-p` (mmap'd chunks),
      `[stack]`, file-backed (ignore for the map).
- [ ] Track the union bounds `heap_start` / `heap_end` shown in the header bar.
- [ ] Re-scan trigger: (a) every 500 ms, and (b) immediately when an event
      arrives with `ptr` outside current bounds. Condition (b) is what keeps the
      display correct when the heap grows mid-frame.
- [ ] Handle the target exiting mid-read: `open` returns `ENOENT`, or a read
      returns `ESRCH`. Switch the UI to a "target exited" state rather than
      dying.
- [ ] Detect the active arena: main arena (from `[heap]`) vs. thread arenas
      (64 MiB-aligned mmap regions). The header bar shows `Active Arena: Main`,
      so the multi-arena case needs at least a correct label.

### M2.2 Chunk metadata inspection

Note: M1.5's `malloc_usable_size` already gives you real size for free. This
section is for the overhead visualisation (the yellow `====` markers) and
for inspecting chunks the interceptor never saw.

- [ ] `process_vm_readv()` reader with a scatter/gather batch interface: one
      syscall for many chunk headers, not one per chunk.
- [ ] ptmalloc chunk decode: header sits at `ptr - 2 * sizeof(size_t)`.
      `prev_size` then `size`. Real size = `size & ~0x7`. Flags in the low 3
      bits: `PREV_INUSE = 1`, `IS_MMAPPED = 2`, `NON_MAIN_ARENA = 4`.
- [ ] Sanity-check decoded sizes (16-byte aligned, ≥ 32, ≤ region span) and
      discard implausible ones. Reading a raced-on header gives garbage; garbage
      must not reach the renderer.
- [ ] Overhead = `chunk_size - requested_size`. This drives the yellow markers.
- [ ] Permission handling: `process_vm_readv` needs same-uid or
      `CAP_SYS_PTRACE`, and `/proc/sys/kernel/yama/ptrace_scope = 1` blocks
      non-descendant attach. On `EPERM`, degrade to interceptor-only data and
      show a one-line hint in the UI (`ptrace denied: overhead unavailable`)
      instead of failing.
- [ ] Never `PTRACE_ATTACH` in the steady path; it stops the target. If ptrace
      is ever needed, it is opt-in behind a flag.

### M2.3 Attach lifecycle

- [ ] `heapviz --pid <pid>`: attach to a running process with the preload
      already active.
- [ ] `heapviz -- <cmd> <args...>`: fork/exec the target with `LD_PRELOAD`
      injected. Most users will want this path.
- [ ] Wait-for-ready: poll for the shm magic with a timeout and a clear error
      ("target is not running libheapviz.so").
- [ ] Detach cleanly on `q`: unmap, leave the target running and unharmed.
- [ ] Handle target death while attached: freeze the last frame, banner the
      status, keep the UI interactive so the user can still inspect.

**Definition of done:** `heapviz -- ./examples/churn` launches, attaches, shows
live bounds, and survives the target exiting.

---

## M3. Sparse address space representation

*Goal: map a 64-bit address space onto a few thousand cells, instantly, in
bounded memory.*

### M3.1 Grid bucketizer

- [x] `cell_bytes = next_pow2(ceil(span / (cols * rows)))`, clamped to
      `[64 B, 1 GiB]`. Power of two so address→cell is a shift, not a divide.
- [x] `cell_index(addr) = (addr - base) >> log2_cell_bytes`.
- [x] Recompute on resize (`SIGWINCH`) and on heap-bounds change. Both paths go
      through one function so they cannot diverge.
- [x] Display the current granularity in the legend. The mockup shows
      `(1 cell = 256 B)`, and it must be the live value, not a constant.
      `MapView::draw_legend` reads it off the grid every frame; the test
      resizes three times and requires the string to move.
- [x] Left gutter labels: address offset from `heap_start` per row, auto-unit
      (B / KB / MB) with consistent width. Drawn by `MapView::draw` and checked
      row by row against `Grid::offset_of_row`, so a map one row out of step
      fails on every row rather than on none.
- [x] Guard `span == 0` and `cols * rows == 0` (1-column terminal): no
      div-by-zero, no negative shift.

#### M3.1 completion notes

**Both recompute paths are one function, structurally.** `set_viewport` and
`set_bounds` are two-line wrappers over `configure`, which is the only code that
computes a granularity. The failure this prevents is quiet: addresses mapped
into one grid while the gutter labels describe another, which looks like a
plausible heap map that is simply wrong. The test asserts that arriving at a
geometry by resizing and by changing bounds produces byte-identical state.

**The ceiling division is written as quotient-plus-remainder.** The obvious
`(span + cells - 1) / cells` overflows exactly when the span gets interesting,
and a 64-bit span is not hypothetical: the gap between a brk heap at `0x55...`
and an mmap region at `0x7f...` is about 47 bits, which is what the target
looks like before M2 can separate the regions.

**A span too wide to cover is reported, not hidden.** With `cell_bytes` clamped
at 1 GiB, a viewport of 10000 cells reaches 10 TiB; a full 47-bit span does not
fit, so the top of it falls off the grid. `covers_whole_span` exists so the UI
can say that, per the README's commitment that a profiler which quietly lies
about what it missed is worse than none. M2 makes it rare by tracking regions
separately rather than spanning the gap between them.

**Degenerate inputs are normal operation, not error handling.** An empty span
(before the first event is drained) and a zero-cell viewport (a window being
dragged narrow) both reach `configure` in ordinary use. It leaves the grid
invalid with a legal shift still in place, so a caller that ignores the return
value gets safe answers rather than a shift by a negative amount.

Verified by `tests/unit/grid_test.cpp`, which round-trips every cell of a grid
rather than spot-checking indices: a shift that is one too small maps both ends
of every cell correctly and still collides with the neighbour, so only checking
the boundary catches it. Six mutations tried, six caught: dropping the ceiling,
dropping the degenerate guard, dropping `index_of`'s upper bound, letting
`set_viewport` diverge from `configure`, un-fixing the gutter width, and making
`covers_whole_span` always agree.

#### M3.1 completion notes: the legend and gutter, once there was a map

The last two boxes waited on somewhere to draw them. `src/tui/map_view.{h,cpp}`
is that place: the legend row, the address gutter and the cells, over the Grid,
HeatMap and HeatRamp that M3.1 through M3.4 built.

**The layout is the granularity, one level up.** M3.1's whole argument is that
one function computes the granularity so two callers cannot disagree. Drawing
reintroduces the hazard, because the number of cells is the rectangle *minus*
the gutter and the legend — a layout decision made in one place and consumed in
two. `map_layout` is therefore the only code that decides what fits, `fit_grid`
is the only supported way to size a grid for an area, and `draw` reads the same
layout back rather than recomputing it. A gutter that appeared in one and not
the other would not misalign the display by a column; it would label row 7 with
the address of row 6, which is a plausible-looking lie.

**Glyph carries density, colour carries state, deliberately twice.** A packed
cell is a full block and a brighter blue. The redundancy is the point: M4.4
degrades colour to a 6×6×6 cube or to sixteen fixed values and `--no-unicode`
degrades the blocks to `#`, `=`, `.`, but the two degrade independently, so
each terminal keeps one working encoding of fill. The thresholds are 25% and
66% rather than evenly spaced, because on a real heap the useful distinction is
"holds something" against "packed", and an even split puts both boundaries
where almost no cell sits.

**Every cell, every frame, no cleverness.** `draw` repaints the whole map with
no attempt to touch only what changed; M4.3's differ turns that back into no
output when nothing moved, and it does so by comparing cells rather than by
trusting the drawing code. Measured on a 200×50 map (9408 cells, `-O2`):

| | per frame |
|---|---|
| draw, every cell settled | 157 µs |
| draw, a third of the map mid-fade | 319 µs |
| `MapView::animating` walking every cell | 46 µs |

against a 1 ms budget. `animating` early-outs on the first moving cell, so its
46 µs is the *idle* cost — the case where it has to prove nothing is moving —
which is 0.28% of a core at 60 Hz and consistent with M4.5's claim.

**`--term-check` grows a synthetic heap.** M2.3 is what connects the map to a
real target, and until then there is no way to look at M3 on a terminal at all;
"the gutter labels are aligned" is not something a unit test reports. `a` now
churns a 4 MiB address space through the shipped Grid, HeatMap and MapView, so
what is on screen is the real path with a fake event source. It is also M4.6's
subject: heavy churn through a full-screen map is the workload the frame budget
is supposed to survive.

Verified by `tests/unit/map_view_test.cpp`, which reads the framebuffer back
rather than asserting on the layout struct — checking the arithmetic without
checking that the arithmetic is what gets used is how a display ends up one row
out of step with its own labels. Fifteen mutations tried, fifteen caught,
including three that needed a test written for them: swapping the two density
thresholds (monotone, and leaves the middle shade unreachable), drawing a map
wider than its area (invisible while every test sizes the grid correctly), and
letting the legend spill past its rectangle (invisible while every legend is
drawn full-width).

### M3.2 Chunk tracking hash table

- [x] Open-addressing Robin Hood table, `uintptr_t` key → chunk record.
- [x] Record: `size` (requested), `usable`, `alloc_ts`, `free_ts`, `tid`,
      `state`. Keep it ≤ 32 bytes so a probe touches one cache line.
- [x] Hash: Fibonacci mixing, `(ptr * 0x9E3779B97F4A7C15) >> (64 - log2_cap)`.
      Pointers are 16-byte aligned, so the low bits are worthless; this fixes
      that. Do not use `ptr % capacity`.
- [x] Robin Hood insert: on probe, if our displacement exceeds the incumbent's,
      swap and continue. Keeps the max probe length low.
- [x] Backward-shift deletion, no tombstones. Tombstone tables degrade badly
      under the alloc/free churn this tool exists to watch.
- [x] Grow at 0.85 load factor, doubling, with rehash.
- [x] Bounded memory policy: cap live entries; when the cap is hit, evict the
      oldest *freed* records first (they are only needed for the fade
      animation). Never evict live allocations silently; surface it.
- [x] Benchmark: 1M inserts + 1M lookups + 1M deletes, report ns/op.
      Target < 30 ns lookup. Record: `25 ns lookup, 117 ns insert, 46 ns erase
      (release, quiet machine; max probe 1-3 at 0.76 load)`.

#### M3.2 completion notes

**Timestamps are milliseconds, not the event's nanoseconds.** That is what makes
the record fit in 32 bytes: two 64-bit timestamps alone would eat half the
budget. Aging (M3.4) works in units of 200-2000 ms, so a millisecond is already
finer than anything that will be displayed, and 32 bits of them is 49 days of
session.

**The eviction queue is a cache, not an index, and that distinction was a bug
first.** The first version cleared the queue when the cap changed and then asked
it what to evict, so lowering a cap onto an already-full table evicted nothing.
The queue can also be legitimately empty (the cap was set after the frees
happened) or full of stale hints (keys since erased, or recycled and now live
again). Eviction therefore rebuilds it from a scan of the table when it comes up
dry, instead of concluding there is nothing to evict. The scan is O(capacity)
and runs off the steady path.

**Nothing live is ever evicted, and a stale hint is where that nearly broke.**
A key enters the queue when freed, but the allocator recycles addresses
constantly, so by the time the hint is popped the record may be live again. Only
the record's own state can say; the queue gives no sign it has gone stale.

**The benchmark asserts a ratio, not the 30 ns figure.** The absolute number is
printed and recorded above, and on a quiet machine reads about 25 ns. It cannot
be the assertion: the same unchanged code measured 25 ns idle and 36 ns with
other work running, and a 20% margin does not survive a shared runner. A
benchmark that fails for reasons unrelated to the change under test teaches
people to re-run it. The test therefore measures a `std::map` over the same keys
in the same process under the same load and requires the table to beat it by 3x;
it comes in at 5.3-5.6x, and that ratio held steady across runs whose absolute
numbers moved 30%.

Verified by `tests/unit/chunk_table_test.cpp`, which replays 56000 mixed
operations against a shadow `std::map`: a backward shift that drops the wrong
element leaves records occupying slots but unreachable, so the table reports the
right size and quietly cannot find things.

Seven mutations tried, six caught, one kept deliberately. Three of the six
initially survived and each exposed a real gap in the test rather than in the
code:

- Replacing the hash with `ptr & mask` survived because the first key generator
  spaced pointers 16 bytes apart, which masking maps to evenly spaced slots with
  room between them. The pattern that separates a real hash from that one is
  glibc's *thread arenas*: the same allocation offset appearing at addresses
  that differ only in high bits. There is now a test for it.
- Letting eviction take live records survived because no key in the bounded test
  was ever freed and then reallocated, so "skip live" never mattered.
- Degrading `find` to a linear scan survived the correctness tests entirely and
  is what the `std::map` ratio was added to catch.

The one kept without coverage is the Robin Hood early-out in `find`. With this
hash the worst probe is 1-3 slots, so walking to the next empty slot instead
costs the same 26 ns a miss already costs, and the mutation is invisible. It
earns its place when the load factor climbs or the key distribution degrades,
which is when nobody is watching a benchmark. `src/tui/chunk_table.cpp` says so
at the line.

### M3.3 Cell aggregation

A cell covers many bytes and therefore many chunks. It needs one colour.

- [x] Per-cell aggregate: `live_bytes`, `n_live`, `last_alloc_ts`,
      `last_free_ts`, `overhead_bytes`, `dominant_state`.
- [x] Precedence when a cell holds mixed states (this decides what the user
      sees): recent free (red flash) > recent malloc (green pulse) > live
      (blue) > overhead marker (yellow) > empty (dark gray).
- [x] Incremental update on each event. Never rebuild the whole grid per frame.
- [x] Full rebuild only on granularity change.

#### M3.3 completion notes

**`dominant_state` is a function, not a field.** The plan lists it among the
stored aggregates, but storing it would make it a cached value that goes stale
as the clock moves: a cell stops flashing red because 300 ms passed, not because
anything updated it. Keeping it computed means there is no per-cell animation
state to sweep or keep in sync, which is precisely what M3.4 needs to age
colours without timers. `cell_state(aggregate, timings, now)` is a free function
so the precedence table can be tested directly.

**One precedence level is currently unreachable through the event path.**
`Overhead` needs a cell holding chunk-header bytes and no payload. Through
`on_alloc`/`on_free` alone, overhead always arrives attached to a live
allocation, which outranks it; the level only becomes reachable when M2.2
decodes real ptmalloc headers and can attribute header bytes to the cell they
physically occupy. Until then `overhead_bytes` is `usable - size`, which is the
best proxy available. Splitting the rule out as a free function is what lets the
level be tested at all today.

**A recent free outranks a newer malloc, deliberately.** The roadmap fixes the
order and the test pins the awkward case explicitly: a free 100 ms ago beats a
malloc 50 ms ago. Memory going away is what people watch for; an allocation
landing is constant background. Worth revisiting in M6 if it reads badly on a
real workload, but it is the specified behaviour and not an accident.

**The incremental and rebuild paths disagree on recycled addresses, and that is
structural.** The chunk table holds one record per address, so an address freed
and then handed straight back no longer records that a free ever happened. The
incremental path saw it and keeps the flash; a rebuild cannot know. Same
category as an evicted freed record under the memory cap. Both are stated as
tests rather than left to be discovered.

Verified by `tests/unit/heatmap_test.cpp`, whose central check replays 40000
events incrementally and requires the result to equal a rebuild from the chunk
table, cell for cell. An accumulator that drifts is otherwise invisible: every
number stays plausible, nothing asserts, and the display is simply wrong.

Six mutations tried, five caught, one deleted rather than kept. The survivor was
an explicit `now < stamp` guard against a timestamp from the future: no mutation
of it could be made to fail, because the subtraction is unsigned and already
wraps to ~2^32, which fails the window comparison on its own. It was a second
spelling of what the arithmetic does, so it was removed rather than documented
as untested. The property it was protecting is still pinned by a test.

### M3.4 Heatmap aging

- [x] Colour is a pure function of `(state, now - timestamp)`, computed at
      render time. No animation state machine, no timers per cell.
- [x] Malloc: bright green pulse for 200 ms (triangle wave on value/brightness),
      then lerp green → blue over 800 ms.
- [x] Free: flash red for 300 ms, then lerp red → dark gray over 2000 ms.
- [x] Long-lived allocations sit at solid blue with brightness scaled by
      `live_bytes / cell_bytes` (fill density), so a half-used cell reads dimmer.
- [x] All durations in one `constexpr` struct so the feel can be tuned in one
      place.
- [x] Lerp in a perceptually reasonable space (Oklab, or at minimum
      gamma-correct sRGB). Naive RGB lerp between green and blue passes through
      a muddy gray and looks cheap. The difference is visible.

**Definition of done:** 1M live allocations render at a stable granularity with
bounded RSS; colours age smoothly with no popping.

#### M3.4 completion notes

Measured: **6.7 ns** per settled cell and **46 ns** per animating one — 67 µs
and 460 µs respectively for a whole 200×50 map, against M4's 1 ms frame.

**Cost is a design constraint here, not an afterthought, and it changed the
API.** Colour is computed per cell per frame: 10000 cells at 60 Hz leaves about
100 ns per cell for everything the frame does. The first version was a free
function taking the palette as sRGB, and converted inside — three cube roots and
three `pow()` calls per cell, **206 ns**, 2.1 ms for one map, twice the entire
budget spent on arithmetic whose inputs are five constants. Three changes took
it to 46 ns:

- `HeatRamp` holds the palette pre-converted and computes every ramp in Oklab,
  so exactly one conversion back to sRGB happens per cell rather than several
  round trips. That is why this is an object you build once and not a function
  you call with a palette.
- The settled colours are tabulated against a quantised density at
  construction, so a cell that is not animating — most cells, most frames —
  costs a divide and a load. 6.7 ns against 32 ns for the same cell computed.
- sRGB encoding is a guessed index plus a correction rather than three `pow()`
  calls, which was 32 ns of the remaining 84.

**Quantising density is only free if the steps cannot be seen.** A map is a
field of adjacent cells at slightly different densities, so a step above the
threshold of perception is contour banding across the whole display, not one
cell being slightly off. 128 steps puts the largest gap at ΔE 0.0031, which is
the floor — one 8-bit code per channel, the smallest step sRGB can express.
Halving to 64 doubles it to 0.0060 and `heat_color_test` fails, which is the
test doing its job rather than a limit worth relaxing.

**Every stage ends where the next begins, and that is the whole of "no
popping".** The pulse is a triangle wave that returns to plain green, so the
fade after it starts from the colour the pulse ended on; the free fade
interpolates towards whatever the cell would otherwise be, so a cell that still
holds live chunks does not jump back to blue when its flash ends. The test walks
every timeline a millisecond at a time and requires consecutive colours to be
perceptually adjacent, which catches a seam at any boundary including ones added
later.

**`cell_animating` exists because the frame budget above assumes it.** It is the
predicate that says whether a cell's colour is still moving, and it is what lets
`LoopApp::animating` skip a frame and the map draw skip a cell. It is asserted
to agree with `color` — a cell reported settled while its colour is still
changing would freeze mid-fade until something else forced a repaint.

Verified by `tests/unit/heat_color_test.cpp`. Fourteen mutations tried, twelve
caught. The two survivors are both in the encoder's index function and neither
is a behaviour change: correctness there belongs to the correction loop, which
walks to the code the thresholds define from whatever index it starts at, so
replacing the loop with a single `if` or indexing linearly instead of by square
root leaves every correctness test passing and only costs time. Both are named
at the line in `src/tui/heat_color.cpp`. Three mutations initially survived and
each exposed a real gap: the density steps had no test at all until banding was
measured, `cell_animating` was untested against the colour it predicts, and the
first cost assertion was written before the warm-up pass and was measuring a
CPU still at its idle clock.

**Two numbers are asserted, both about 3× the measured figure.** The lesson from
M3.2 stands: an absolute timing bound with a 20% margin fails for reasons
unrelated to the change under test and teaches people to re-run the suite. These
are not targets, they are the line past which the per-cell path has grown a
conversion or an allocation again — 150 ns animating, 25 ns settled, against
46 and 6.7. Dropping the density table alone takes the settled figure to 32 ns
and fails.

**Not done here:** nothing draws a map yet, so this is exercised by
`--term-check`, which lays the two fades out as time across the screen. Watching
one cell age tests your memory of what the colour was a second ago; a ramp shows
a discontinuity as a seam you can point at.

---

## M4. High-performance ANSI terminal engine

*Goal: 60 FPS with sub-millisecond CPU per frame and zero tearing.*

### M4.1 Terminal setup / teardown

- [x] `tcgetattr` to save original termios; store in a file-scope static so
      signal handlers can reach it.
- [x] Raw mode: clear `ICANON | ECHO | ISIG` (keep `ISIG` if you want Ctrl-C to
      work normally; decide and document), clear `IXON | ICRNL`, set
      `VMIN = 0`, `VTIME = 0`.
- [x] Enter alternate screen `\033[?1049h`, hide cursor `\033[?25l`, clear
      `\033[2J`.
- [x] Teardown in exact reverse order, wrapped in an RAII guard.
- [x] `atexit` handler + handlers for `SIGINT`, `SIGTERM`, `SIGHUP`, `SIGSEGV`,
      `SIGABRT`. Handlers must be async-signal-safe: set a flag, or if dying,
      `write(2)` the restore sequence directly and `_exit`. No `printf`.
- [x] `std::set_terminate` also restores, so an uncaught exception does not
      leave a wrecked terminal.
- [x] Verify: `kill -9` cannot be caught, so also document
      `reset` / `stty sane` as the user escape hatch in the README.

#### M4.1 completion notes

**D4 resolved: `ISIG` stays enabled.** Ctrl-C therefore remains a signal rather
than a keystroke to decode, and `SIGINT` sets the same quit flag `q` does, so
both exits run one teardown path. `IEXTEN` is also cleared (Ctrl-V must not
swallow the next byte) and `OPOST` is cleared, because the renderer positions
the cursor itself and any LF→CRLF rewriting by the kernel is corruption.

**Fatal signals re-raise instead of `_exit`.** The roadmap suggested `_exit`
after restoring. That would hand the user a clean shell and silently swallow a
crash in the profiler, losing the core dump and the exit status. Handlers are
installed with `SA_RESETHAND`, so `raise(sig)` after restoring dies exactly as
it would have without us.

**State is set before the terminal is touched.** `g_active` goes up right after
`tcgetattr` succeeds, not after `tcsetattr`. The other ordering leaves a window
where a signal arriving mid-`enter()` strands the terminal in raw mode.

Verified by `tests/integration/terminal_restore_test.cpp`, which allocates a pty and runs each exit
path in a child attached to it, checking both the bytes that reached the
terminal and the termios left behind. All five guards were mutation-tested.
One mutation (removing `std::set_terminate`) initially went undetected, because
`std::terminate` calls `abort()` and the `SIGABRT` handler was already
restoring; a scenario that drops our `SIGABRT` handler first now isolates it.

### M4.2 Framebuffer

- [x] `struct Cell { char32_t glyph; uint32_t fg; uint32_t bg; uint8_t attrs; }`,
      kept to 16 bytes, `attrs` for bold/dim/underline.
- [x] `FrontBuffer` and `BackBuffer`, flat `std::vector<Cell>` of `w * h`,
      allocated once per resize.
- [x] Drawing API on the back buffer: `put(x, y, cell)`, `text(x, y, str, fg,
      bg)`, `hline`, `vline`, `box(rect, style)`, `fill(rect, cell)`.
- [x] All drawing clipped to the buffer. An off-by-one on a 40-column terminal
      must not be a heap overflow in the tool that watches for heap overflows.
- [x] `swap()` after flush; clear the new back buffer to the "empty cell".

#### M4.2 completion notes

**`Cell` carries three padding bytes, so cells are never compared with
memcmp.** Padding is indeterminate, and two visually identical cells differing
in it would make the M4.3 diff redraw cells that did not change. `operator==`
compares the four fields; field offsets are pinned by static asserts.

**Clipping is centralised.** `put()` bounds-checks, and `fill`/`hline`/`vline`/
`box` clip through `clip_rect`, which does its arithmetic in 64-bit so a rect
starting near `INT_MAX` cannot wrap into something that overlaps the screen.
`box` additionally clamps its edge loops to the visible span: `put` would clip
anyway, but a rect two million columns wide would otherwise spin through two
million rejected calls.

`text()` takes UTF-8 and decodes to `char32_t` (M4.3 has the encoder for the
reverse trip). Malformed input yields U+FFFD and always consumes at least one
byte, so a decoding bug shows on screen rather than hanging the renderer.
Every code point is assumed one column wide, which holds for ASCII and the
block-drawing glyphs the design uses.

Verified by `tests/unit/framebuffer_test.cpp`, which counts global `operator new`
calls across ten frames of drawing and requires zero (ground rule #5). Eight
mutations were tried; three initially went undetected and each exposed a real
gap in the test rather than in the code: the overflow case used origin 0 where
nothing wraps, a no-op resize was never checked for reuse, and the swap-clears
check needed two frames because after one the recycled buffer is still empty
from `resize()`.

### M4.3 Differential ANSI streamer

This is the performance core. Each item below is worth milliseconds.

- [x] Cell-by-cell compare Front vs Back; skip unchanged.
- [x] Pen state tracking: remember the last emitted fg/bg/attrs and only
      emit an SGR sequence when it actually changes. A naive renderer emits
      ~20 bytes of colour per cell; this cuts output by an order of magnitude.
- [x] Cursor position tracking: only emit `\033[Y;XH` when the next changed
      cell is not immediately after the last written one.
- [x] TrueColor: `\033[38;2;R;G;Bm` (fg), `\033[48;2;R;G;Bm` (bg).
- [x] Integer-to-ASCII by hand into the output buffer. No `snprintf` per cell.
- [x] UTF-8 encoder for `char32_t` (the block glyphs are 3 bytes each).
- [x] Output buffer is a single pre-sized `std::vector<char>`, reserved once,
      `.clear()`ed per frame; capacity is retained, so no allocation.
- [x] One `write(STDOUT_FILENO, buf, len)` per frame, looped on partial writes
      and `EINTR`.
- [x] Reset SGR (`\033[0m`) at end of frame so a crash mid-scroll does not tint
      the user's shell.

#### M4.3 completion notes

**Producing the bytes is separated from writing them.** `render()` fills a
buffer, `flush()` performs the single write. That is what makes the wire format
testable without a terminal: the tests assert on exact byte sequences rather
than on what appeared on a screen.

**An idle frame emits nothing at all, not even the SGR reset.** The epilogue is
skipped when no cell changed, so "nothing happened" costs zero bytes and zero
syscalls, which M4.5's idle-CPU goal depends on.

**Attribute changes go through a full reset.** There is no portable code to
turn off just one attribute, so any change to `attrs` emits `\033[0m` and
re-applies colour. Colour-only changes, the common case in a heatmap, skip
that.

**`flush()` takes an optional writer.** A blocking fd transfers everything
unless a signal lands mid-write, so the partial-write loop cannot be reached
reliably from outside the process. The seam lets a test supply a writer that
returns short and one that returns `EINTR`. This was added because the first
attempt at the test, a payload larger than a pipe buffer, could not fail: a
blocking pipe write delivers every byte, so the mutation that deleted the loop
still passed.

Nine mutations tried. Two initially survived. One was a genuine test gap (the
short-write loop above). The other, dropping `cur_valid_ = false` past the last
column, is semantically equivalent today and is documented as such in
`src/tui/renderer.cpp`: an impossible column can never match a valid target, so the
renderer emits a move either way. The line is kept as a guard for when
`move_to()` learns relative motion, and the test says plainly that it does not
prove it.

### M4.4 Capability detection & fallback

- [x] Detect TrueColor: `COLORTERM` ∈ {`truecolor`, `24bit`}. Fall back to the
      256-colour cube (`\033[38;5;Nm`) with an RGB→cube quantiser.
- [x] Fall back further to 16 colours if `TERM` suggests it; the tool should
      still be *usable* over a bad SSH session even if not beautiful.
- [x] `--no-unicode` flag → ASCII glyph set (`#`, `=`, `.`) for terminals with
      broken block-character fonts.
- [x] Refuse to start below a minimum size (e.g. 80×24) with a readable message
      rather than rendering garbage.

#### M4.4 completion notes

**Detection is a pure function of two strings, not a reader of the
environment.** `detect_capabilities(colorterm, term, force_ascii)` takes what it
decides from, and only `detect_capabilities_from_env` calls `getenv`. `setenv`
is not thread-safe and its effect outlives the test that used it, so a rule
that could only be tested by installing an environment would be a rule that
disturbs every other test in the binary. The whole detection table is covered
without touching the process's own environment.

**Nothing queries the terminal.** DA1 and XTGETTCAP would mean writing a query
and reading a reply that may never arrive, on the same device that is the
user's keyboard. The timeout would be a visible startup delay on exactly the
slow links this fallback exists for.

**An unrecognised `TERM` defaults to 256 colours, not 16.** This is the one
judgement call in the table and it goes against "conservative wins": a great
many terminals still report a bare `TERM=xterm` while supporting 256 colours,
so falling back to 16 there would degrade the common case to protect a rare
one. 16-colour mode is opt-in from `TERM` positively saying so.

**The pen is held resolved, not as the `Cell`'s RGB.** Quantising is
many-to-one, so a gradient of near-identical reds becomes one palette index.
Comparing the raw values, as M4.3's renderer did, would emit an identical
sequence for every cell of that gradient — handing a 256-colour terminal *more*
bytes per frame than a TrueColor one gets, which is precisely backwards. Driven
over a pty, the term-check sweep costs 161 colour sequences at 24-bit and 79 in
256-colour mode.

**The grey ramp is consulted, not just the cube.** The 6×6×6 cube's grey
diagonal steps by 40 where the 24-entry ramp steps by 10, so quantising a grey
panel to the cube alone bands it visibly. The quantiser computes both and takes
the smaller error, and never returns an index below 16: 0–15 are whatever the
user's theme redefined them to, so their appearance is not predictable from
inside the process.

**Two different minimum sizes, deliberately.** `size_is_usable` (80×24) refuses
to *start*; `LoopConfig::min_width/min_height` (20×6) is the geometry below
which a frame cannot be drawn at all and a running session is torn down.
Between them the display is cramped, which is the user's business — a mid-drag
resize should not kill their session.

Verified by `tests/unit/capabilities_test.cpp`, which round-trips all 240
non-system palette entries rather than asserting against a table of indices (a
table only proves the code still agrees with what it printed the day it was
written), and by four new cases in `tests/unit/renderer_test.cpp` for the wire
formats. Four mutations tried, four caught: comparing raw RGB in the pen,
dropping the grey ramp, defaulting unknown terminals to 16, and collapsing the
bright ANSI range onto the base one.

### M4.5 Event loop & frame pacing

- [x] Single-threaded loop: drain ring → update model → draw → diff → write →
      sleep-to-deadline.
- [x] `poll()` on stdin with a timeout computed as `next_frame_deadline - now`.
      Input stays responsive and the process idles at ~0% CPU when nothing
      changes.
- [x] Skip the draw entirely when no events arrived, no key was pressed, and no
      cell has a live animation. Idle CPU is a feature.
- [x] Frame budget instrumentation: measure drain / update / draw / diff / write
      separately; surface as the FPS counter (and a `--debug-timing` overlay).
- [x] `SIGWINCH` handler sets a `volatile sig_atomic_t` flag; the loop calls
      `ioctl(TIOCGWINSZ)`, reallocates buffers, recomputes granularity, forces a
      full repaint.
- [x] Resize during a frame must not tear or crash; buffers are only swapped at
      a defined point in the loop.

#### M4.5 completion notes

**The application is an interface, not a callback soup.** `LoopApp` has
`drain`, `update`, `key`, `animating`, `resized` and `draw`; everything except
`draw` has a do-nothing default. That is what makes the loop testable without a
ring, a terminal or a heap map, none of which exist yet. M3 supplies the real
implementation.

**Everything the loop touches the outside world through is a parameter**: the
two file descriptors, `ioctl(TIOCGWINSZ)` (`LoopConfig::size_fn`) and `write(2)`
(`LoopConfig::writer`). `unit/event_loop_test.cpp` therefore needs no pty and no
child process, which is why it lives in `unit/` despite being mostly wall-clock
measurement.

**Waiting is not idling, and only CPU time tells them apart.** Two of the
scenarios compare `getrusage` against elapsed wall time. A loop that polls an
exhausted stdin, or one that truncates its `poll` timeout to zero milliseconds,
hits every deadline on schedule and burns a core doing it — the wall clock reads
identical in both cases. This was not hypothetical: `/dev/null` and a pipe with
no writers report exhaustion differently (a zero-byte `read` versus `POLLHUP`),
are handled by different branches, and the first version of the test exercised
only one of them.

**Rounding the poll timeout up is load-bearing.** `poll(2)` with a timeout of
zero returns immediately with `rc == 0`, which the loop cannot distinguish from
a real timeout, so a truncating conversion ends every frame roughly a
millisecond early — a 20% overshoot at 200 fps that no correctness assertion
would notice.

**There is deliberately no idle back-off.** Stretching the frame period after a
quiet spell was considered and dropped: a skipped frame costs a `poll` and two
atomic loads, so 60 a second is already under a tenth of a percent of a core,
and backing off would trade first-event latency for a saving too small to
measure.

**`SIGWINCH` is only handled if a `TerminalGuard` is active**, because that is
what installs the handler. The coupling is real and easy to break, so
`integration/event_loop_pty_test.cpp` runs the loop on a real pty and resizes it
from outside with `TIOCSWINSZ`; a loop that forgot the guard, or a signal that
never interrupted `poll`, both show up as the child never reporting.

**A resize repaints every cell rather than diffing.** `Framebuffer::resize`
clears both buffers, so a plain diff would emit only the non-blank cells and
leave the previous frame's remains in whatever part of the screen just appeared.
Both resize tests assert `cells_emitted() == cells_examined() == w * h`, which is
the only formulation that fails when `full_repaint` is dropped.

Ten mutations tried, ten caught. Three initially survived, all of them test
gaps: the `POLLHUP` branch was never reached, the `read`-returns-zero branch was
never reached, and the pacing bound was loose enough to accept a frame that
ended early.

**Not done here:** the minimum-size check stops the loop with `LoopExit::TooSmall`
rather than drawing a message. M4.4 owns the readable-refusal UI; the loop's job
is only to stop before it renders into six columns.

### M4.6 Verification

- [x] Frame time budget met: under 1 ms CPU per frame at 60 FPS on a 200×50
      terminal with heavy churn. Record: `700 µs CPU/frame, 450 µs of work`.
- [x] `strace -c` shows one `write` per frame, no per-cell syscalls.
- [x] Visual: no tearing, no flicker, no cursor artefacts during rapid resize.

#### M4.6 completion notes: measuring a loop that is mostly asleep

The workload is the one M3.1 built: `DemoHeap` churning a 4 MiB address space
through the real `Grid`, `HeatMap` and `MapView`. It moved out of `main.cpp`
into `heapviz_core` (`src/tui/demo_heap.{h,cpp}`) for this, so the number below
comes from the same object `--term-check` runs rather than from a copy of it.
1200 allocator operations per frame is 72k events a second, and the fade windows
are 1–2 s long, so every one of the 10,000 cells is mid-fade on every frame:
the heat ramp's expensive path, taken 10,000 times, 60 times a second.

`tests/unit/frame_budget_test.cpp`, on this machine (release, WSL2, load ~1.7):

| | µs |
|---|---|
| update (fold 1200 events into the map) | 28 |
| draw (10,000 cells, every one animating) | 245 |
| diff (renderer, ~65 KB on the wire) | 177 |
| **frame's work** | **450** |
| **CPU per drawn frame, paced at 60 FPS** | **700** |

Two things in that table are worth reading rather than skimming.

**The 250 µs between the work and the CPU is the cost of waiting.** The loop
sleeps 16 ms per frame, and the working set — two 160 KB cell buffers, a 65 KB
output buffer, 10,000 heat aggregates — does not survive that in cache. Every
frame starts cold. That is not overhead anyone can remove; it is what running at
60 Hz on an otherwise idle core costs, and it is the reason the budget is
measured on the paced loop and not on a tight benchmark loop.

**The budget is met with about 30% to spare, not 15x.** The frame *period* is
16.6 ms and the frame's work is 450 µs, which reads like enormous headroom, but
the budget is 1 ms for a reason that has nothing to do with the deadline:
heapviz is stealing that time from the process it is measuring. At 200×50 with
everything animating, the margin against the real budget is thin, and the thing
that would eat it is more per-cell work in `draw` or `render`. M5's cursor and
M6's polish both add per-cell work.

**Why best-of-N.** Every source of noise here is additive — another process
taking the core, a migration, the cold-cache effect above landing worse on one
run — so the cheapest of five paced runs is the one least contaminated by things
that are not heapviz, while a regression is present in every run and moves the
minimum too. The spread is printed alongside: on this box the five runs land
between 700 and 1210 µs, and a spread wider than that is a statement about the
machine, not about the code.

**`strace` was not available on this machine, and is not what was used.** What
strace would be looking for is that a frame reaches the terminal in one call
rather than one per cell, and that decision is made in `Renderer::flush`, which
takes its `write(2)` as a parameter. `frame_budget_test` counts invocations of
that parameter: exactly 180 calls for 180 drawn frames, ~65 KB each. A per-row
or per-cell write would show up as a multiple of 50 or of 10,000, which is the
same signal `strace -c` prints. The caveat strace would also show is kept
visible by reporting bytes per frame — `flush` retries on a short write, so a
tty that accepts less than a whole frame costs more than one call.

**The visual box, and what a test can take from it.** Three of the four things
that make a resize look wrong are properties of the byte stream, so
`tests/integration/resize_storm_test.cpp` drives 300 resizes through a real pty
in 2.4 seconds — roughly one per frame, landing inside the draw, the diff and
the `write(2)` — and reads back the 10 MB of frames that come out:

- *flicker* is the screen being cleared between frames. `ESC[2J` belongs to
  entering the alternate screen and appears exactly once, ever.
- *cursor artefacts* are the cursor becoming visible and skating across the map.
  It is hidden once on entry, shown once on exit, and never in between.
- *tearing* is a frame painted at one geometry landing on a terminal that is now
  another. No `ESC[row;colH` in the whole stream addresses a cell outside the
  largest grid there has been, and no frame was painted at a size the loop had
  not measured.

What is left for a human is whether the result *looks* right, which is a
judgement about pixels: `heapviz --term-check`, `a` to churn, then drag the
window. That is what the box's word "visual" means and no test replaces it.

**One check was written, found to be vacuous, and rewritten.** "Every resize
produced a full repaint" compared two counters the loop increments on the same
line, so it held however the repaint flag was set — setting `full_repaint =
false` on resize left it green. That property belongs to
`event_loop_pty_test`, which compares `cells_emitted` against `cells_examined`
on a single controlled resize; the storm's version now asserts the thing the
storm is actually for, that no resize is dropped between the signal and the
repaint, and a mutation that drops one in four turns it red.

Seven mutations, six caught and one kept as a documented limit:

| Mutation | Caught by |
|---|---|
| the cursor is never hidden | `resize_storm` |
| a full repaint clears the screen first | `resize_storm` |
| the framebuffer is not resized on a shrink | `resize_storm` |
| one in four resizes is dropped | `resize_storm` |
| the frame is written 512 bytes at a time | `frame_budget` |
| every cell's colour is computed eight times | `frame_budget` |
| every cell's colour is computed *twice* | **nothing** — see below |

Computing the colour twice costs ~130 µs on a 450 µs frame, and no honest gate
on a machine whose best-of-3 already varies by 25% catches a 30% regression.
That is a limit of a wall-clock benchmark, not a hole in the test: the budget
check is a budget check, and it fails when the budget is missed. A per-phase
regression detector tight enough to catch 30% would need a quiet machine, which
is M7's problem, not M4's.

---

## M5. Interactivity, analysis & profiling

### M5.1 Spatial cursor

- [ ] `h` / `j` / `k` / `l` move one cell.
- [ ] `H` / `J` / `K` / `L` move one screen-ish jump (or 10 cells).
- [ ] `g` / `G` jump to heap start / end.
- [ ] `n` / `N` jump to next / previous non-empty cell. Essential on a sparse
      map where most cells are dark.
- [ ] Cursor rendered as the cyan box from the mockup, drawn over the map
      without destroying the underlying cell colours.
- [ ] Clamp to grid bounds; cursor survives a resize (clamp to new bounds, keep
      the address if possible rather than the coordinate).

### M5.2 Chunk inspector panel

Fields, matching the mockup exactly:

- [ ] `Address`: full 64-bit pointer, `0x%016lx`.
- [ ] `User Size`: requested bytes, with a human-readable unit in parens.
- [ ] `Real Size`: usable/chunk size, with the header overhead called out
      (`1,040 bytes (16B header)`).
- [ ] `Status`: `ACTIVE (ptmalloc)` / `FREED` / `MMAPPED` / `UNALLOCATED`.
- [ ] `Lifetime`: seconds since alloc for live chunks, alloc→free duration for
      dead ones.
- [ ] Cell → chunk reverse lookup. When a cell holds several chunks, show the
      largest and a `+N more` affordance; `Tab` cycles through them.
- [ ] Empty state that reads as intentional, not broken.

### M5.3 Telemetry metrics panel

- [ ] `Total Allocated`: cumulative bytes.
- [ ] `Active Chunks`: live count, thousands-separated.
- [ ] `Heap Fragmentation`: percentage plus the `[Low]` / `[Med]` / `[High]`
      badge, colour-coded.
- [ ] `Peak Memory`: high-water mark of live bytes.
- [ ] `Telemetry Ring`: `(head - tail) / capacity` as a percentage. Turns amber
      past 50%, red past 80%.
- [ ] Dropped events counter. Not in the mockup, but every other number on the
      panel is wrong by an unknown amount once events start dropping. Put it next
      to the ring metric, red and impossible to miss when non-zero.

### M5.4 Fragmentation analysis

- [ ] Walk live chunks in address order (keep a sorted index, or sort per
      analysis tick; do not sort per frame).
- [ ] `fragmentation = total_gap_bytes / (max_addr - min_addr)` where gaps are
      the spaces between consecutive live chunks.
- [ ] Also compute largest free gap. "Can I still allocate 1 MB" is the
      question users actually have.
- [ ] Thresholds for the badge: Low < 15%, Med < 40%, High ≥ 40%. Tune against
      the churn example and record the reasoning.
- [ ] Recompute on a timer (e.g. 4 Hz), not per frame.

### M5.5 Snapshot & leak detection

- [ ] `[s]` snapshot: record the set of live pointers + a timestamp.
- [ ] `[d]` diff mode: highlight allocations made after the snapshot that are
      still live. These are leak candidates.
- [ ] Diff-mode cells use a distinct colour (magenta) so the mode is
      unmistakable, plus a banner in the header.
- [ ] `[S]` clears the snapshot.
- [ ] Snapshot summary line: `N chunks / M bytes leaked since <time>`.
- [ ] Bounded snapshot memory; refuse (with a message) rather than OOM on a
      target with tens of millions of live chunks.

### M5.6 Other controls

- [ ] `[Space]` pause. Critical detail: pausing must freeze *rendering and
      model updates*, but the consumer has to keep draining the ring. Stop
      draining and the target's ring fills and starts dropping events, so
      pausing would corrupt the data you resume into. Drain into a staging queue.
- [ ] `[q]` quit, `[?]` help overlay, `[r]` reset stats.
- [ ] Footer control bar always reflects the current mode's real bindings.

---

## M6. Visual polish (the beautiful part)

The mockup is the spec. This milestone is what separates "works" from the
screenshot.

### M6.1 Design tokens

Extracted from the mockup; define once in `src/tui/theme.h`, reference nowhere
else as literals.

| Token | RGB | Use |
|-------|-----|-----|
| `frame` | `#C87828` | outer border, panel rules |
| `title` | `#7FE08A` | `heapviz v0.1`, section headers |
| `accent` | `#F5A623` | PID, numeric highlights |
| `malloc` | `#4EC94E` | fresh allocation |
| `freed` | `#E01B24` | recently freed |
| `active` | `#3584E4` | long-lived allocation |
| `overhead` | `#F6D32D` | chunk header markers |
| `unalloc` | `#2A2A2A` | empty address space |
| `cursor` | `#33D7E8` | inspector cursor |
| `text` | `#D8D8D8` | body text |
| `dim` | `#7A7A7A` | labels, units |
| `bg` | `#0C0C0C` | canvas |

- [ ] Theme struct + these values.
- [ ] `--theme` flag with at least a light variant (some people profile in a
      light terminal; the whole thing inverts badly right now).
- [ ] Contrast check: every text colour ≥ 4.5:1 against `bg`.

### M6.2 Layout geometry

- [ ] Row 1: title bar, with `heapviz v0.1 [PID: N - ./cmd]` left, FPS badge right.
- [ ] Row 2: `Heap Address Range: 0x... - 0x... | Active Arena: Main`.
- [ ] Row 3: centred `SPATIAL HEAP MAP` section title with rules either side.
- [ ] Row 4: legend row, with the live cell-granularity value.
- [ ] Rows 5..N-8: the map, with a 7-column address gutter.
- [ ] Bottom block: two panels side by side, `[ CHUNK INSPECTOR ]` (≈60%) and
      `[ TELEMETRY METRICS ]` (≈40%).
- [ ] Last row: controls bar.
- [ ] All of the above expressed as a constraint-solved layout, not hardcoded
      row numbers. It has to survive any terminal size.
- [ ] Graceful degradation: below ~100 columns, stack the two panels; below
      ~30 rows, collapse the legend.

### M6.3 Chrome details

- [ ] Box-drawing charset: `─ │ ┌ ┐ └ ┘ ├ ┤ ┬ ┴ ┼` for panels; the mockup uses
      a bracketed-label style `[ CHUNK INSPECTOR ]` inset into the top rule.
- [ ] Half-block vertical resolution: render two grid rows per terminal row
      using `▀` with fg = top cell, bg = bottom cell. Doubles the map
      resolution at no cost, and is the biggest visual upgrade available.
- [ ] Density ramp `░ ▒ ▓ █` for partial cell fill.
- [ ] Right-aligned numeric columns with thousands separators; units in `dim`.
- [ ] Monospace-safe: every glyph used must be single-width. Test with a wide
      CJK glyph nearby to confirm no column drift.

### M6.4 Motion

- [ ] Pulse easing is a smooth curve (ease-out), not linear. Linear pulses look
      mechanical.
- [ ] FPS counter is smoothed over ~30 frames. A jittering number reads as
      instability even when the tool is fine.
- [ ] `--no-animation` flag for screenshots, recordings, and CI.

---

## M7. Hardening & release

### M7.1 Tests

- [ ] Ring buffer under TSan (two threads, 10M events, zero loss).
- [ ] Hash table property test: random insert/lookup/delete against
      `std::unordered_map` as the oracle.
- [ ] Bucketizer edge cases: 1-byte span, 2^47 span, 1-column terminal.
- [ ] ANSI diff streamer golden tests: given Front/Back pairs, assert the exact
      byte sequence emitted. This is where regressions hide.
- [ ] maps-parser fixtures from real `/proc/*/maps` dumps, including
      multi-arena and 60+ region cases.
- [ ] Terminal restore test: send each fatal signal, assert termios is restored.

### M7.2 Robustness

- [ ] Target forks / execs: no crash, defined behaviour, documented.
- [ ] Target with 10M live allocations: bounded memory, degraded but alive.
- [ ] Target that never allocates: no divide-by-zero, sensible empty state.
- [ ] Consumer starts before / after / twice against the same target.
- [ ] Consumer killed mid-run: target keeps running, unharmed.
- [ ] Run the TUI itself under valgrind and ASan; it must be clean.

### M7.3 Performance gates

- [ ] Interceptor overhead < 50 ns/alloc. Measured: `______`
- [ ] TUI CPU < 1 ms/frame at 60 FPS. Measured: `______`
- [ ] TUI RSS < 100 MB at 1M live chunks. Measured: `______`
- [ ] Zero dropped events at 1M allocs/sec. Measured: `______`

### M7.4 Docs & packaging

- [ ] `README.md`: what it is, the screenshot, install, quickstart
      (`heapviz -- ./your_app`), keybindings table, limitations.
- [ ] `CLAUDE.md`, written after M0 lands so the build commands and layout are
      real rather than aspirational.
- [ ] Troubleshooting: ptrace_scope, `LD_PRELOAD` with setuid binaries (silently
      ignored; document it), static binaries (no dynamic linking, so no hooks;
      document it), musl vs glibc.
- [ ] GPL-3.0 headers on source files (the repo is GPL-3.0).
- [ ] `CHANGELOG.md` kept current: entries land in `[Unreleased]` in the same
      commit as the change, not reconstructed at release time. ABI-version bumps
      and keybinding changes are flagged as breaking.
- [ ] CI: build + test on Ubuntu, gcc and clang.
- [ ] `man` page or `--help` that covers the whole CLI surface.

---

## 2. Open decisions

These need an answer before the milestone that depends on them. None block
starting M0.

**D1. `realloc` representation.** A 32-byte packet cannot hold both old and new
pointer alongside size and timestamp. Options: (a) emit two events, `Free(old)`
then `Malloc(new)`, which keeps the packet at 32 bytes, costs one extra slot, and
loses the "this was a realloc" relationship; (b) widen the packet to 40 or 48
bytes for everything. **Recommendation: (a).** Realloc is a minority of traffic
and the fixed 32-byte packet keeps the ring math a shift.

**D2. Chunk overhead source.** `malloc_usable_size` in the interceptor (free,
exact, in-process) versus `process_vm_readv` on chunk headers (works for chunks
the interceptor missed, needs ptrace permission). **Recommendation: use
`malloc_usable_size` as the primary and treat M2.2 as the enrichment path for
the inspector panel.** This de-risks M2 considerably.

**D3. Arena scope.** Main-arena-only for v0.1, or full multi-arena from the
start? The header bar says `Active Arena: Main`, which suggests a switcher.
**Recommendation: parse and label all arenas, visualise main only in v0.1.**

**D5. Recovering from dropped events.** *(supersedes part of D2.)* A full ring
drops the event and bumps a counter, per ground rule #2. But the TUI rebuilds
its chunk table by replaying Malloc/Free, so a dropped `Free` leaves a chunk
live forever (a phantom leak) and a dropped `Malloc` orphans a later `Free`.
There is no resync, so after one overflow the model is wrong for the rest of
the session. This directly undermines M5.5 leak detection: an invented leak is
indistinguishable from a real one. **D2 is therefore mis-framed** - M2.2's
`process_vm_readv` heap walk is not "enrichment for the inspector panel", it is
the only ground-truth resync available, and it should be scheduled as the
recovery path. **Recommendation: interim, the consumer baselines `dropped` at
attach and banners the UI as degraded the moment it increases; M2.2 becomes the
real fix.** No ABI support needed for the interim - `dropped` already carries
the signal.

**D6. Default ring capacity vs process trees.** `HV_DEFAULT_CAPACITY` is 1 Mi
slots, 32 MiB of tmpfs per target. `LD_PRELOAD` is inherited across `exec`, so
`heapviz -- make -j8` gives every spawned compiler its own 32 MiB segment, most
never read by anyone. At the measured ~2M events/sec, 1 Mi slots buys 500 ms of
buffer where 256 Ki buys 125 ms, still around eight frames at 60 FPS.
**Recommendation: drop the default to 256 Ki and let `HEAPVIZ_CAPACITY` raise
it.** Not yet done; interacts with D5, since a smaller ring drops sooner.

**D7. Whether to follow forked children.** `hv_atfork_child` nulls the ring
pointer, so a `fork`-only child emits nothing, while a `fork`+`exec` child runs
the constructor again and gets its own segment nobody reads. Neither behaviour
is wrong but the asymmetry is undocumented, and no test covers `fork` at all
despite `pthread_atfork` being subtle. **Recommendation: document
parent-process-only as the supported scope for v0.1, add a fork test, and treat
following children as a later feature.**

**D4. `ISIG` in raw mode.** Keeping it means Ctrl-C works conventionally;
clearing it means `q` is the only exit. **Recommendation: keep `ISIG`**, and
handle `SIGINT` with the same clean teardown as `q`.

---

## 3. Suggested build order

M0 → M1.1 → M1.6 → M4.1 → M4.2 → M4.3 → M1 (rest) → M3 → M5 → M2 → M6 → M7

The reasoning: get the bootstrap problem and the ring buffer done first (they
are the highest-risk items), then get *something on screen* early so every
subsequent milestone has visible feedback. M2 lands late because D2 makes it
an enhancement rather than a dependency.
