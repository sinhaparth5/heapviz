# heapviz roadmap and progress tracker

Derived from `heap-doc.md`. That document is the *design intent*; this one is the
*executable plan*. Every box here is small enough to finish in one sitting and
concrete enough that "done" is not a judgement call.

**Status legend:** `[ ]` not started · `[~]` in progress · `[x]` done · `[!]` blocked · `[-]` cut

---

## 0. Progress at a glance

| # | Milestone | Scope | Status | Done |
|---|-----------|-------|--------|------|
| M0 | Scaffold & shared ABI | build, layout, IPC contract | `[ ]` | 0 / 19 |
| M1 | Zero-overhead interceptor | `libheapviz.so` | `[ ]` | 0 / 39 |
| M2 | Kernel & memory parsing | `/proc`, ptmalloc headers | `[ ]` | 0 / 17 |
| M3 | Sparse address representation | grid, hash table, aging | `[ ]` | 0 / 24 |
| M4 | ANSI terminal engine | raw mode, double buffer, diff | `[ ]` | 0 / 34 |
| M5 | Interactivity & analysis | cursor, frag, snapshots | `[ ]` | 0 / 33 |
| M6 | Visual polish | the *beautiful* part | `[ ]` | 0 / 20 |
| M7 | Hardening & release | perf, tests, docs, packaging | `[ ]` | 0 / 23 |

**Total: 0 / 209**

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

- [ ] `CMakeLists.txt` at root, `cmake_minimum_required(VERSION 3.20)`,
      `project(heapviz LANGUAGES C CXX)`, C++20 / C11.
- [ ] Target `heapviz`, the TUI binary. Links `pthread`, `rt` (for `shm_open`
      on older glibc).
- [ ] Target `heapviz_preload`: `SHARED`, output name `libheapviz.so`.
      Links `dl`, `pthread`, `rt`.
- [ ] Preload target compiled with `-fPIC -fvisibility=hidden
      -fno-exceptions -fno-rtti -O2`. Only the interposed symbols are
      `__attribute__((visibility("default")))`.
- [ ] Preload target must **not** link `libstdc++` (`set_target_properties(...
      LINKER_LANGUAGE C)` or link `-nodefaultlibs` + explicit libs). Verify with
      `ldd libheapviz.so`; libstdc++ pulls in static init that allocates.
- [ ] `-Wall -Wextra -Werror` on both targets. Sanitizer preset
      (`-DHEAPVIZ_ASAN=ON`) for the TUI only. ASan and an LD_PRELOAD malloc
      hook in the same process do not coexist.
- [ ] `CMakePresets.json` with `debug`, `release`, `asan` presets.

### M0.2 Source layout

- [ ] `src/common/`: headers shared by both binaries. No `.cpp` files that
      link into the preload lib unless they honour ground rule #1.
- [ ] `src/preload/`: interceptor.
- [ ] `src/tui/`: renderer, parsers, analysis.
- [ ] `examples/`: target programs to profile.
- [ ] `tests/`: unit tests.

### M0.3 The shared ABI (`src/common/heapviz_abi.h`)

Get this file wrong and you get silent corruption across process boundaries,
so it is worth more care than anything else in M0.

- [ ] `HEAPVIZ_ABI_MAGIC` (8-byte constant, e.g. `0x48505A5631000000` = "HPZV1")
      and `HEAPVIZ_ABI_VERSION`. Consumer refuses to attach on mismatch and says
      so in plain English.
- [ ] `struct Event`, exactly 32 bytes, standard layout, no padding
      surprises:

      | Offset | Size | Field       | Notes                                    |
      |--------|------|-------------|------------------------------------------|
      | 0      | 8    | `timestamp` | `CLOCK_MONOTONIC` nanoseconds            |
      | 8      | 8    | `ptr`       | `uintptr_t`, user pointer                |
      | 16     | 8    | `size`      | requested bytes (0 for `free`)           |
      | 24     | 4    | `usable`    | `malloc_usable_size` result, truncated   |
      | 28     | 3    | `tid`       | low 24 bits of thread id                 |
      | 31     | 1    | `op`        | `enum Op : uint8_t`                      |

- [ ] `static_assert(sizeof(Event) == 32)` and a `static_assert` on every
      field offset via `offsetof`. Both binaries compile this header, so a
      layout drift is a compile error rather than a runtime mystery.
- [ ] `enum class Op : uint8_t { Malloc, Free, Calloc, Realloc, Memalign }`.
- [ ] `struct RingHeader`, cache-line aware:
      - `alignas(64)` block: magic, version, pid, capacity (power of two),
        `event_size`, process start time, target `comm` string (16 bytes).
      - `alignas(64)` block: `std::atomic<uint64_t> head` (producer only).
      - `alignas(64)` block: `std::atomic<uint64_t> tail` (consumer only).
      - `alignas(64)` block: `std::atomic<uint64_t> dropped`, `total_events`.
      The three hot blocks sit on separate cache lines; head/tail false sharing
      is the classic SPSC performance bug.
- [ ] `heapviz_shm_name(pid)` produces the canonical `/heapviz_shm_<pid>`
      string, used by both sides. One function, no string duplication.
- [ ] Total mapping size helper: `sizeof(RingHeader) + capacity * sizeof(Event)`,
      rounded up to page size.

**Definition of done:** `cmake --build build` produces `heapviz` and
`libheapviz.so`; `ldd libheapviz.so` shows no `libstdc++`; a throwaway test
maps the shm region from both binaries and agrees on every offset.

---

## M1. Zero-overhead interceptor (`libheapviz.so`)

*Goal: hook memory calls without slowing, deadlocking, or crashing the target.*

### M1.1 The bootstrap problem (do this first; it is the hard part)

`dlsym(RTLD_NEXT, "malloc")` calls `calloc` internally in glibc on first use.
Your hook is therefore called *before it knows what the real malloc is*. Every
naive LD_PRELOAD malloc hook dies here.

- [ ] Static bump-allocator arena: `static char g_bootstrap[64 * 1024];` plus an
      atomic offset. Serves allocations while `g_real_malloc == nullptr`.
- [ ] `heapviz_is_bootstrap_ptr(p)`: pointer-range check against the arena.
- [ ] `free()` / `realloc()` must call that check first and no-op (or
      bump-copy) for bootstrap pointers. Passing them to the real `free` is an
      instant abort.
- [ ] Bump allocator honours 16-byte alignment.
- [ ] Arena exhaustion is a hard, loud failure (`write(2)` to stderr + `_exit`),
      not silent corruption. 64 KB is generous; if it exhausts, something is
      wrong.

### M1.2 Symbol resolution

- [ ] `resolve_symbols()`: one-shot, guarded by `std::atomic_flag` or
      `pthread_once`, resolving: `malloc`, `free`, `calloc`, `realloc`,
      `posix_memalign`, `aligned_alloc`, `memalign`, `valloc`, `pvalloc`,
      `malloc_usable_size`.
- [ ] Store as plain function pointers in file-scope statics, not `std::function`.
- [ ] `__attribute__((constructor(101)))` init hook, which runs before most
      user constructors, so the ring is live for early allocations.
- [ ] Handle `dlsym` returning `NULL` for optional symbols (`pvalloc` is not
      everywhere) without aborting.

### M1.3 Re-entrancy guard

- [ ] `static __thread bool g_in_hook __attribute__((tls_model("initial-exec")));`
      Not `thread_local` on a non-trivial type: dynamic TLS allocates on
      first access in a dlopen'd library, and you are inside malloc.
- [ ] RAII-free guard (plain set/clear around the telemetry block) since the
      preload lib is built `-fno-exceptions`.
- [ ] Guard covers telemetry only. The pass-through to the real allocator
      happens outside it, so a guarded call still returns correct memory.

### M1.4 Interposed functions

Each one: call real function → if `!g_in_hook`, set guard, emit event, clear
guard → return. Never the other order; the pointer must exist before it is
reported.

- [ ] `malloc(size)`
- [ ] `free(ptr)`: bootstrap check, `NULL` check, emit `Op::Free`
- [ ] `calloc(n, size)`: overflow check on `n * size`; must work during
      bootstrap
- [ ] `realloc(ptr, size)`: see open decision D1 below
- [ ] `posix_memalign(out, align, size)`
- [ ] `aligned_alloc(align, size)`, `memalign`, `valloc`, `pvalloc`
- [ ] Confirm `strdup`/`asprintf`/`getline` are covered transitively (they call
      the interposed `malloc` through the PLT). Write a test, do not assume.

### M1.5 Event emission

- [ ] `clock_gettime(CLOCK_MONOTONIC, ...)`: vDSO, no syscall. Benchmark it;
      if it exceeds ~25 ns, fall back to `CLOCK_MONOTONIC_COARSE` or TSC.
- [ ] `malloc_usable_size(ptr)` inline. Gives the real allocator overhead at
      no cost, inside the process, with no ptrace and no `/proc` read. It
      skips most of M2.2.
- [ ] Thread id via cached `__thread` `gettid()` result. Never call `gettid()`
      per event.
- [ ] Single 32-byte struct write into the ring slot; no memcpy of parts.

### M1.6 SPSC lock-free ring buffer

- [ ] Capacity is a compile-time-configurable power of two (default 1 MiB
      events = 32 MiB). Index with `& (capacity - 1)`, never `%`.
- [ ] Producer: `head` loaded `relaxed` (we are the only writer), `tail` loaded
      `acquire`, slot written, `head` stored `release`. The release store is
      what publishes the payload. Get this wrong and the consumer reads torn
      data on ARM.
- [ ] Producer caches the last-seen `tail` in a local and only re-reads the
      atomic when the cached value says "full". Cuts cross-core traffic
      dramatically.
- [ ] Full ⇒ `dropped.fetch_add(1, relaxed)` and return. Never spin, never
      block.
- [ ] Consumer: `tail` `relaxed`, `head` `acquire`, read, `tail` `release`.
- [ ] Consumer drains in batches (read up to N events per frame) rather than one
      at a time.

### M1.7 Shared memory setup

- [ ] `shm_open("/heapviz_shm_<pid>", O_CREAT | O_EXCL | O_RDWR, 0600)`.
      `O_EXCL` so a stale segment is detected rather than silently reused.
- [ ] On `EEXIST`: unlink the stale segment and retry once, then give up loudly.
- [ ] `ftruncate` to the computed size, `mmap(PROT_READ | PROT_WRITE,
      MAP_SHARED)`, then `close(fd)`; the mapping outlives the descriptor.
- [ ] Write `RingHeader` fields, then publish `magic` last with a release
      store. The magic is the "region is ready" flag.
- [ ] `__attribute__((destructor))` + `atexit` ⇒ `shm_unlink`. Set a "producer
      exited" flag in the header first so the TUI can report it.
- [ ] `pthread_atfork` child handler: the child has a new PID and inherits the
      parent's mapping. Either detach (simplest, ship this) or create a fresh
      segment. Document which.

### M1.8 Verification

- [ ] `examples/churn.c`: configurable allocation workload: steady rate, bursty,
      fragmenting (alloc many, free every other), large-mmap path (>128 KB
      triggers `mmap` not `brk`), and multi-threaded.
- [ ] Standalone `tests/ring_test`: two threads, 10M events, assert zero loss
      and correct ordering under TSan.
- [ ] `tests/overhead_bench`: same workload with and without `LD_PRELOAD`,
      report ns/alloc delta. Target: under 50 ns added per allocation. Record
      the measured number here: `______`.
- [ ] Run the churn example under `LD_PRELOAD` for 60 s with no consumer
      attached. Must not crash, must not grow memory, `dropped` climbs.

**Definition of done:** `LD_PRELOAD=./libheapviz.so ./examples/churn` runs
clean under a multi-threaded workload, `dropped == 0` when a consumer is
draining, overhead is measured and recorded.

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

- [ ] `cell_bytes = next_pow2(ceil(span / (cols * rows)))`, clamped to
      `[64 B, 1 GiB]`. Power of two so address→cell is a shift, not a divide.
- [ ] `cell_index(addr) = (addr - base) >> log2_cell_bytes`.
- [ ] Recompute on resize (`SIGWINCH`) and on heap-bounds change. Both paths go
      through one function so they cannot diverge.
- [ ] Display the current granularity in the legend. The mockup shows
      `(1 cell = 256 B)`, and it must be the live value, not a constant.
- [ ] Left gutter labels: address offset from `heap_start` per row, auto-unit
      (B / KB / MB) with consistent width.
- [ ] Guard `span == 0` and `cols * rows == 0` (1-column terminal): no
      div-by-zero, no negative shift.

### M3.2 Chunk tracking hash table

- [ ] Open-addressing Robin Hood table, `uintptr_t` key → chunk record.
- [ ] Record: `size` (requested), `usable`, `alloc_ts`, `free_ts`, `tid`,
      `state`. Keep it ≤ 32 bytes so a probe touches one cache line.
- [ ] Hash: Fibonacci mixing, `(ptr * 0x9E3779B97F4A7C15) >> (64 - log2_cap)`.
      Pointers are 16-byte aligned, so the low bits are worthless; this fixes
      that. Do not use `ptr % capacity`.
- [ ] Robin Hood insert: on probe, if our displacement exceeds the incumbent's,
      swap and continue. Keeps the max probe length low.
- [ ] Backward-shift deletion, no tombstones. Tombstone tables degrade badly
      under the alloc/free churn this tool exists to watch.
- [ ] Grow at 0.85 load factor, doubling, with rehash.
- [ ] Bounded memory policy: cap live entries; when the cap is hit, evict the
      oldest *freed* records first (they are only needed for the fade
      animation). Never evict live allocations silently; surface it.
- [ ] Benchmark: 1M inserts + 1M lookups + 1M deletes, report ns/op.
      Target < 30 ns lookup. Record: `______`.

### M3.3 Cell aggregation

A cell covers many bytes and therefore many chunks. It needs one colour.

- [ ] Per-cell aggregate: `live_bytes`, `n_live`, `last_alloc_ts`,
      `last_free_ts`, `overhead_bytes`, `dominant_state`.
- [ ] Precedence when a cell holds mixed states (this decides what the user
      sees): recent free (red flash) > recent malloc (green pulse) > live
      (blue) > overhead marker (yellow) > empty (dark gray).
- [ ] Incremental update on each event. Never rebuild the whole grid per frame.
- [ ] Full rebuild only on granularity change.

### M3.4 Heatmap aging

- [ ] Colour is a pure function of `(state, now - timestamp)`, computed at
      render time. No animation state machine, no timers per cell.
- [ ] Malloc: bright green pulse for 200 ms (triangle wave on value/brightness),
      then lerp green → blue over 800 ms.
- [ ] Free: flash red for 300 ms, then lerp red → dark gray over 2000 ms.
- [ ] Long-lived allocations sit at solid blue with brightness scaled by
      `live_bytes / cell_bytes` (fill density), so a half-used cell reads dimmer.
- [ ] All durations in one `constexpr` struct so the feel can be tuned in one
      place.
- [ ] Lerp in a perceptually reasonable space (Oklab, or at minimum
      gamma-correct sRGB). Naive RGB lerp between green and blue passes through
      a muddy gray and looks cheap. The difference is visible.

**Definition of done:** 1M live allocations render at a stable granularity with
bounded RSS; colours age smoothly with no popping.

---

## M4. High-performance ANSI terminal engine

*Goal: 60 FPS with sub-millisecond CPU per frame and zero tearing.*

### M4.1 Terminal setup / teardown

- [ ] `tcgetattr` to save original termios; store in a file-scope static so
      signal handlers can reach it.
- [ ] Raw mode: clear `ICANON | ECHO | ISIG` (keep `ISIG` if you want Ctrl-C to
      work normally; decide and document), clear `IXON | ICRNL`, set
      `VMIN = 0`, `VTIME = 0`.
- [ ] Enter alternate screen `\033[?1049h`, hide cursor `\033[?25l`, clear
      `\033[2J`.
- [ ] Teardown in exact reverse order, wrapped in an RAII guard.
- [ ] `atexit` handler + handlers for `SIGINT`, `SIGTERM`, `SIGHUP`, `SIGSEGV`,
      `SIGABRT`. Handlers must be async-signal-safe: set a flag, or if dying,
      `write(2)` the restore sequence directly and `_exit`. No `printf`.
- [ ] `std::set_terminate` also restores, so an uncaught exception does not
      leave a wrecked terminal.
- [ ] Verify: `kill -9` cannot be caught, so also document
      `reset` / `stty sane` as the user escape hatch in the README.

### M4.2 Framebuffer

- [ ] `struct Cell { char32_t glyph; uint32_t fg; uint32_t bg; uint8_t attrs; }`,
      kept to 16 bytes, `attrs` for bold/dim/underline.
- [ ] `FrontBuffer` and `BackBuffer`, flat `std::vector<Cell>` of `w * h`,
      allocated once per resize.
- [ ] Drawing API on the back buffer: `put(x, y, cell)`, `text(x, y, str, fg,
      bg)`, `hline`, `vline`, `box(rect, style)`, `fill(rect, cell)`.
- [ ] All drawing clipped to the buffer. An off-by-one on a 40-column terminal
      must not be a heap overflow in the tool that watches for heap overflows.
- [ ] `swap()` after flush; clear the new back buffer to the "empty cell".

### M4.3 Differential ANSI streamer

This is the performance core. Each item below is worth milliseconds.

- [ ] Cell-by-cell compare Front vs Back; skip unchanged.
- [ ] Pen state tracking: remember the last emitted fg/bg/attrs and only
      emit an SGR sequence when it actually changes. A naive renderer emits
      ~20 bytes of colour per cell; this cuts output by an order of magnitude.
- [ ] Cursor position tracking: only emit `\033[Y;XH` when the next changed
      cell is not immediately after the last written one.
- [ ] TrueColor: `\033[38;2;R;G;Bm` (fg), `\033[48;2;R;G;Bm` (bg).
- [ ] Integer-to-ASCII by hand into the output buffer. No `snprintf` per cell.
- [ ] UTF-8 encoder for `char32_t` (the block glyphs are 3 bytes each).
- [ ] Output buffer is a single pre-sized `std::vector<char>`, reserved once,
      `.clear()`ed per frame; capacity is retained, so no allocation.
- [ ] One `write(STDOUT_FILENO, buf, len)` per frame, looped on partial writes
      and `EINTR`.
- [ ] Reset SGR (`\033[0m`) at end of frame so a crash mid-scroll does not tint
      the user's shell.

### M4.4 Capability detection & fallback

- [ ] Detect TrueColor: `COLORTERM` ∈ {`truecolor`, `24bit`}. Fall back to the
      256-colour cube (`\033[38;5;Nm`) with an RGB→cube quantiser.
- [ ] Fall back further to 16 colours if `TERM` suggests it; the tool should
      still be *usable* over a bad SSH session even if not beautiful.
- [ ] `--no-unicode` flag → ASCII glyph set (`#`, `=`, `.`) for terminals with
      broken block-character fonts.
- [ ] Refuse to start below a minimum size (e.g. 80×24) with a readable message
      rather than rendering garbage.

### M4.5 Event loop & frame pacing

- [ ] Single-threaded loop: drain ring → update model → draw → diff → write →
      sleep-to-deadline.
- [ ] `poll()` on stdin with a timeout computed as `next_frame_deadline - now`.
      Input stays responsive and the process idles at ~0% CPU when nothing
      changes.
- [ ] Skip the draw entirely when no events arrived, no key was pressed, and no
      cell has a live animation. Idle CPU is a feature.
- [ ] Frame budget instrumentation: measure drain / update / draw / diff / write
      separately; surface as the FPS counter (and a `--debug-timing` overlay).
- [ ] `SIGWINCH` handler sets a `volatile sig_atomic_t` flag; the loop calls
      `ioctl(TIOCGWINSZ)`, reallocates buffers, recomputes granularity, forces a
      full repaint.
- [ ] Resize during a frame must not tear or crash; buffers are only swapped at
      a defined point in the loop.

### M4.6 Verification

- [ ] Frame time budget met: under 1 ms CPU per frame at 60 FPS on a 200×50
      terminal with heavy churn. Record: `______`.
- [ ] `strace -c` shows one `write` per frame, no per-cell syscalls.
- [ ] Visual: no tearing, no flicker, no cursor artefacts during rapid resize.

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
