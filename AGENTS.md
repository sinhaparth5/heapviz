# Repository Guidelines

## Project Structure & Module Organization

heapviz is a Linux-only terminal heap profiler with two halves:

- `src/preload/`: the C11 `LD_PRELOAD` allocator interceptor.
- `src/tui/`: the C++20 terminal UI and profiling pipeline.
- `src/common/`: C/C++-compatible shared-memory ABI and ring headers.
- `tests/unit/`: in-process tests; `tests/integration/`: tests using processes, PTYs, or the preload library.
- `tests/fixtures/`, `tests/support/`, and `tests/cmake/`: measured programs, shared helpers, and CTest drivers.
- `examples/`: `hello_alloc` and `churn` workloads; `assets/`: documentation images.

Consult `CLAUDE.md` for detailed architecture constraints and `ROADMAP.md` for planned work.

## Build, Test, and Development Commands

Use the checked-in CMake presets (Ninja and CMake 3.20+):

```bash
cmake --preset debug
cmake --build --preset debug
ctest --preset debug
```

Replace `debug` with `release` for optimized builds or `asan` for AddressSanitizer/UBSan coverage of the TUI and tests. Artifacts appear under `build/<preset>/`. Run a focused test with:

```bash
ctest --preset release -R renderer --output-on-failure
```

The CMake targets are `heapviz_tui`, `heapviz_preload`, and `heapviz_core`; output files are `heapviz`, `libheapviz.so`, and `libheapviz_core.a`.

## Coding Style & Naming Conventions

Match surrounding code: four-space indentation in C/C++, two spaces in CMake, and no tabs. Builds treat warnings as errors and enable conversion, shadowing, and qualifier checks. There is no repository-wide formatter configuration.

Public C/ABI names use `heapviz_` or `HEAPVIZ_`; internal C names use `hv_` or `HV_`. C++ code lives in `namespace hv`, with `PascalCase` types, `snake_case` functions, and `kCamelCase` constants. Use include guards like `HEAPVIZ_TUI_RENDERER_H`.

Keep the preload library allocation-free and C-only. Changes to `src/common/heapviz_abi.h` affect both producer and consumer and require ABI-focused tests.

## Testing Guidelines

Tests are standalone executables registered with CTest. Name files `<feature>_test.cpp` (or `.c`) and register the CTest name without `_test`. Put process-boundary behavior in `integration/`; otherwise use `unit/`. Add tests for fixes and run debug plus release; use `asan` for memory-sensitive TUI changes. No numeric coverage threshold is defined.

## Commit & Pull Request Guidelines

Recent commits use concise milestone subjects, for example `M5.4: fragmentation analysis`. Follow that pattern when work maps to the roadmap; otherwise use a short, imperative subject.

Pull requests should explain behavior and architectural impact, list commands run, link the relevant issue or roadmap milestone, and include terminal captures for visible TUI changes. Call out ABI changes, performance effects, and Linux/glibc assumptions explicitly.
