cmake_minimum_required(VERSION 3.20)
project(heapviz VERSION 0.1.0 LANGUAGES C CXX)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
  message(FATAL_ERROR
    "heapviz is Linux-only: it depends on LD_PRELOAD, /proc, POSIX shm, and "
    "process_vm_readv.")
endif()

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

option(HEAPVIZ_ASAN  "Build the TUI and tests with AddressSanitizer" OFF)
option(HEAPVIZ_TESTS "Build the test suite" ON)

set(HEAPVIZ_WARNINGS
  -Wall -Wextra -Werror
  -Wshadow -Wconversion -Wsign-conversion
  -Wcast-qual -Wpointer-arith -Wwrite-strings)

# ---------------------------------------------------------------------------
# Shared ABI. Header-only and language-neutral: both halves compile it.
# ---------------------------------------------------------------------------
add_library(heapviz_abi INTERFACE)
target_include_directories(heapviz_abi INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/src)

# We build with strict -std=c11/-std=c++20 (extensions off) so the code stays
# honest about what it relies on. That also hides every POSIX declaration behind
# a feature-test macro, so ask for POSIX.1-2008 explicitly: shm_open, ftruncate,
# clock_gettime, and mmap all live behind it.
target_compile_definitions(heapviz_abi INTERFACE _POSIX_C_SOURCE=200809L)

# ---------------------------------------------------------------------------
# libheapviz.so - the LD_PRELOAD interceptor.
#
# Deliberately C11, not C++. A C++ shared object links libstdc++, whose static
# initialisers allocate; since this library IS the allocator inside the target
# process, that is a re-entrancy deadlock waiting to happen. Building as C means
# the "no libstdc++" guarantee is structural rather than a linker flag someone
# can accidentally drop. Enforced by the preload_no_libstdcxx test below.
# ---------------------------------------------------------------------------
add_library(heapviz_preload SHARED src/preload/preload.c)
set_target_properties(heapviz_preload PROPERTIES
  OUTPUT_NAME        heapviz
  PREFIX             "lib"
  C_VISIBILITY_PRESET hidden
  LINKER_LANGUAGE    C)
target_compile_options(heapviz_preload PRIVATE ${HEAPVIZ_WARNINGS} -fPIC)
# RTLD_NEXT, malloc_usable_size, memalign, pvalloc, and gettid are all GNU
# extensions. The interceptor is inherently glibc-specific, so ask for them.
target_compile_definitions(heapviz_preload PRIVATE _GNU_SOURCE)
target_link_libraries(heapviz_preload PRIVATE heapviz_abi dl pthread rt)

# ---------------------------------------------------------------------------
# heapviz_core - the TUI's guts, as a static library.
#
# Split out from the binary so the tests exercise the same objects the shipped
# tool runs, rather than a recompilation with its own flags.
# ---------------------------------------------------------------------------
add_library(heapviz_core STATIC
  src/tui/terminal.cpp
  src/tui/framebuffer.cpp
  src/tui/capabilities.cpp
  src/tui/chunk_table.cpp
  src/tui/proc_maps.cpp
  src/tui/chunk_reader.cpp
  src/tui/region_map.cpp
  src/tui/session.cpp
  src/tui/heap_app.cpp
  src/tui/grid.cpp
  src/tui/heatmap.cpp
  src/tui/heat_color.cpp
  src/tui/map_view.cpp
  src/tui/demo_heap.cpp
  src/tui/renderer.cpp
  src/tui/event_loop.cpp
  src/tui/shm_cleanup.cpp)
target_compile_options(heapviz_core PRIVATE ${HEAPVIZ_WARNINGS})
target_link_libraries(heapviz_core PUBLIC heapviz_abi)

# process_vm_readv is a GNU extension, and <sys/uio.h> hides it behind
# _GNU_SOURCE. Set on the one file that needs it rather than on the library, so
# the other fifteen keep compiling against plain POSIX.1-2008 and cannot pick up
# a glibc-only function by accident.
set_source_files_properties(src/tui/chunk_reader.cpp
  PROPERTIES COMPILE_DEFINITIONS _GNU_SOURCE)

# ---------------------------------------------------------------------------
# heapviz - the TUI consumer.
# ---------------------------------------------------------------------------
add_executable(heapviz_tui src/tui/main.cpp)
set_target_properties(heapviz_tui PROPERTIES OUTPUT_NAME heapviz)
target_compile_options(heapviz_tui PRIVATE ${HEAPVIZ_WARNINGS})
target_link_libraries(heapviz_tui PRIVATE heapviz_core heapviz_abi pthread rt)

# ASan and an LD_PRELOAD malloc hook cannot share a process, so the sanitizer
# preset covers the TUI and tests only, never heapviz_preload.
if(HEAPVIZ_ASAN)
  foreach(t heapviz_core heapviz_tui)
    target_compile_options(${t} PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(${t}    PRIVATE -fsanitize=address,undefined)
  endforeach()
endif()

# ---------------------------------------------------------------------------
# Example targets to point heapviz at. Never sanitized: these are meant to be
# run under LD_PRELOAD, and ASan cannot share a process with a malloc hook.
# ---------------------------------------------------------------------------
add_executable(hello_alloc examples/hello_alloc.c)
target_compile_options(hello_alloc PRIVATE ${HEAPVIZ_WARNINGS})

add_executable(churn examples/churn.c)
target_compile_options(churn PRIVATE ${HEAPVIZ_WARNINGS})
target_compile_definitions(churn PRIVATE _POSIX_C_SOURCE=200809L)
target_link_libraries(churn PRIVATE pthread)

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
if(HEAPVIZ_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

message(STATUS "heapviz ${PROJECT_VERSION} | build=${CMAKE_BUILD_TYPE} "
               "asan=${HEAPVIZ_ASAN} tests=${HEAPVIZ_TESTS}")
