# heapviz test suite. SPDX-License-Identifier: GPL-3.0-or-later
#
# Layout:
#   unit/         in-process and fast; no child processes, no LD_PRELOAD
#   integration/  crosses a process boundary: fork/exec, a pty, or the preload
#   fixtures/     programs that exist to be measured, not to assert
#   support/      shared helpers, no main()
#   cmake/        ctest driver scripts, each named after the test it drives
#
# Conventions:
#   - A test's CTest name is its file stem with any _test suffix removed.
#   - Where one test needs several programs they share a stem and take a role
#     suffix (shm_roundtrip_writer / shm_roundtrip_reader).
#   - The CMake target name matches the file stem.

function(heapviz_test_target name)
  target_compile_options(${name} PRIVATE ${HEAPVIZ_WARNINGS})
  target_link_libraries(${name} PRIVATE heapviz_abi rt)
  # Helpers are included as "support/...", so the include root is tests/ itself.
  target_include_directories(${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  if(HEAPVIZ_ASAN)
    target_compile_options(${name} PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(${name}    PRIVATE -fsanitize=address,undefined)
  endif()
endfunction()

set(HEAPVIZ_TEST_CMAKE ${CMAKE_CURRENT_SOURCE_DIR}/cmake)

# ===========================================================================
# unit
# ===========================================================================

# --- ABI layout: the same dump compiled as C11 and as C++20 ----------------
add_executable(abi_layout_c   unit/abi_layout_c.c)
add_executable(abi_layout_cxx unit/abi_layout_cxx.cpp)
heapviz_test_target(abi_layout_c)
heapviz_test_target(abi_layout_cxx)

add_test(NAME abi_layout
  COMMAND ${CMAKE_COMMAND}
          -DC_DUMP=$<TARGET_FILE:abi_layout_c>
          -DCXX_DUMP=$<TARGET_FILE:abi_layout_cxx>
          -P ${HEAPVIZ_TEST_CMAKE}/abi_layout.cmake)

# --- Multi-producer ring stress --------------------------------------------
add_executable(ring_mpsc_test unit/ring_mpsc_test.c)
heapviz_test_target(ring_mpsc_test)
target_link_libraries(ring_mpsc_test PRIVATE pthread)
# 4 producers x 2.5M events through a 4096-slot ring: ~2400 wraps, which is what
# actually exercises the lap-parity bit.
add_test(NAME ring_mpsc COMMAND ring_mpsc_test 4 10000000)
set_tests_properties(ring_mpsc PROPERTIES TIMEOUT 120)

# --- Framebuffer: clipping and the no-allocation rule (M4.2) ---------------
add_executable(framebuffer_test unit/framebuffer_test.cpp)
heapviz_test_target(framebuffer_test)
target_link_libraries(framebuffer_test PRIVATE heapviz_core)
add_test(NAME framebuffer COMMAND framebuffer_test)
set_tests_properties(framebuffer PROPERTIES TIMEOUT 60)

# --- Single-consumer enforcement (ABI v3) ----------------------------------
add_executable(consumer_claim_test unit/consumer_claim_test.cpp)
heapviz_test_target(consumer_claim_test)
add_test(NAME consumer_claim COMMAND consumer_claim_test)

# --- Chunk tracking table (M3.2) -------------------------------------------
# Correctness runs everywhere; the 30 ns lookup budget is asserted only under
# NDEBUG, for the same reason interceptor_overhead is release-only.
add_executable(chunk_table_test unit/chunk_table_test.cpp)
heapviz_test_target(chunk_table_test)
target_link_libraries(chunk_table_test PRIVATE heapviz_core)
add_test(NAME chunk_table COMMAND chunk_table_test)
set_tests_properties(chunk_table PROPERTIES TIMEOUT 180)

# --- Address space bucketizer (M3.1) ---------------------------------------
add_executable(grid_test unit/grid_test.cpp)
heapviz_test_target(grid_test)
target_link_libraries(grid_test PRIVATE heapviz_core)
add_test(NAME grid COMMAND grid_test)
set_tests_properties(grid PROPERTIES TIMEOUT 60)

# --- Per-cell aggregation (M3.3) -------------------------------------------
add_executable(heatmap_test unit/heatmap_test.cpp)
heapviz_test_target(heatmap_test)
target_link_libraries(heatmap_test PRIVATE heapviz_core)
add_test(NAME heatmap COMMAND heatmap_test)
set_tests_properties(heatmap PROPERTIES TIMEOUT 60)

# --- Heatmap aging: Oklab ramps and the flash windows (M3.4) ---------------
add_executable(heat_color_test unit/heat_color_test.cpp)
heapviz_test_target(heat_color_test)
target_link_libraries(heat_color_test PRIVATE heapviz_core)
add_test(NAME heat_color COMMAND heat_color_test)
set_tests_properties(heat_color PROPERTIES TIMEOUT 60)

# --- Capability detection, quantisers and fallback (M4.4) ------------------
# Detection takes the environment strings as arguments rather than reading
# them, so this needs no setenv and cannot disturb the tests around it.
add_executable(capabilities_test unit/capabilities_test.cpp)
heapviz_test_target(capabilities_test)
target_link_libraries(capabilities_test PRIVATE heapviz_core)
add_test(NAME capabilities COMMAND capabilities_test)
set_tests_properties(capabilities PROPERTIES TIMEOUT 60)

# --- Differential ANSI streamer (M4.3) -------------------------------------
add_executable(renderer_test unit/renderer_test.cpp)
heapviz_test_target(renderer_test)
target_link_libraries(renderer_test PRIVATE heapviz_core)
add_test(NAME renderer COMMAND renderer_test)
set_tests_properties(renderer PROPERTIES TIMEOUT 60)

# --- Event loop and frame pacing (M4.5) ------------------------------------
# The loop takes its fds, its terminal measurement and its write(2) as
# parameters, so this needs no pty and no child; the real SIGWINCH path is
# integration/event_loop_pty_test.cpp. Most assertions are wall-clock
# measurements, hence the unusually long timeout for a unit test.
add_executable(event_loop_test unit/event_loop_test.cpp)
heapviz_test_target(event_loop_test)
target_link_libraries(event_loop_test PRIVATE heapviz_core)
add_test(NAME event_loop COMMAND event_loop_test)
set_tests_properties(event_loop PROPERTIES TIMEOUT 60)

# ===========================================================================
# integration
# ===========================================================================

# --- Shared memory round-trip: C11 writes, C++20 reads ---------------------
add_executable(shm_roundtrip_writer integration/shm_roundtrip_writer.c)
add_executable(shm_roundtrip_reader integration/shm_roundtrip_reader.cpp)
heapviz_test_target(shm_roundtrip_writer)
heapviz_test_target(shm_roundtrip_reader)

add_test(NAME shm_roundtrip
  COMMAND ${CMAKE_COMMAND}
          -DWRITER=$<TARGET_FILE:shm_roundtrip_writer>
          -DREADER=$<TARGET_FILE:shm_roundtrip_reader>
          -P ${HEAPVIZ_TEST_CMAKE}/shm_roundtrip.cmake)

# --- The .so must not drag in libstdc++ ------------------------------------
add_test(NAME preload_no_libstdcxx
  COMMAND ${CMAKE_COMMAND}
          -DSO=$<TARGET_FILE:heapviz_preload>
          -P ${HEAPVIZ_TEST_CMAKE}/preload_no_libstdcxx.cmake)

# --- ...and must actually load, so the check above cannot pass vacuously ---
add_executable(preload_load_test integration/preload_load_test.c)
heapviz_test_target(preload_load_test)
target_link_libraries(preload_load_test PRIVATE dl)
add_test(NAME preload_load
  COMMAND preload_load_test $<TARGET_FILE:heapviz_preload>)

# --- Terminal setup/teardown (M4.1) ----------------------------------------
# Needs a real terminal to be meaningful, so the test allocates a pty and runs
# each exit path in a child attached to it. -lutil is for openpty.
add_executable(terminal_restore_test integration/terminal_restore_test.cpp)
heapviz_test_target(terminal_restore_test)
target_link_libraries(terminal_restore_test PRIVATE heapviz_core util)
add_test(NAME terminal_restore COMMAND terminal_restore_test)
set_tests_properties(terminal_restore PROPERTIES TIMEOUT 60)

# --- A real SIGWINCH through a real pty (M4.5) -----------------------------
add_executable(event_loop_pty_test integration/event_loop_pty_test.cpp)
heapviz_test_target(event_loop_pty_test)
target_link_libraries(event_loop_pty_test PRIVATE heapviz_core util)
add_test(NAME event_loop_pty COMMAND event_loop_pty_test)
set_tests_properties(event_loop_pty PROPERTIES TIMEOUT 60)

# --- Reaping rings left behind by SIGKILLed targets ------------------------
add_executable(shm_cleanup_test integration/shm_cleanup_test.cpp)
heapviz_test_target(shm_cleanup_test)
target_link_libraries(shm_cleanup_test PRIVATE heapviz_core)
add_test(NAME shm_cleanup COMMAND shm_cleanup_test)
set_tests_properties(shm_cleanup PROPERTIES TIMEOUT 60)

# --- End-to-end: real LD_PRELOAD interception across a process boundary ----
add_executable(intercept_test integration/intercept_test.cpp)
heapviz_test_target(intercept_test)

# --- Transitive coverage: strdup/asprintf/getline/operator new -------------
add_executable(transitive_target fixtures/transitive_target.cpp)
target_compile_options(transitive_target PRIVATE ${HEAPVIZ_WARNINGS})
target_compile_definitions(transitive_target PRIVATE _GNU_SOURCE)

add_executable(transitive_test integration/transitive_test.cpp)
heapviz_test_target(transitive_test)

# --- Interceptor overhead budget (M1.8: under 50 ns/call) ------------------
add_executable(interceptor_overhead_test integration/interceptor_overhead_test.c)
heapviz_test_target(interceptor_overhead_test)

# ASan supplies its own malloc, so LD_PRELOAD-ing ours into an ASan-built target
# is the combination that cannot coexist (ROADMAP.md M0.1). Every preload-driven
# test is gathered here and skipped under ASan; they run in debug and release.
if(NOT HEAPVIZ_ASAN)
  add_test(NAME intercept
    COMMAND intercept_test $<TARGET_FILE:heapviz_preload> $<TARGET_FILE:churn>)
  set_tests_properties(intercept PROPERTIES TIMEOUT 120)

  add_test(NAME transitive
    COMMAND transitive_test $<TARGET_FILE:heapviz_preload> $<TARGET_FILE:transitive_target>)
  set_tests_properties(transitive PROPERTIES TIMEOUT 120)

  # Only meaningful on an optimised build. At -O0 the interceptor costs 45-56 ns
  # and straddles the budget, which made this test flaky in debug for no useful
  # reason: the 50 ns figure is a claim about what ships, not about -O0.
  if(CMAKE_BUILD_TYPE MATCHES "^(Release|RelWithDebInfo)$")
    add_test(NAME interceptor_overhead
      COMMAND ${CMAKE_COMMAND}
              -DBENCH=$<TARGET_FILE:interceptor_overhead_test>
              -DPRELOAD=$<TARGET_FILE:heapviz_preload>
              -P ${HEAPVIZ_TEST_CMAKE}/interceptor_overhead.cmake)
    set_tests_properties(interceptor_overhead PROPERTIES TIMEOUT 300)
  endif()
endif()

# ===========================================================================
# the shipped binary
# ===========================================================================

add_test(NAME tui_version COMMAND heapviz_tui --version)
set_tests_properties(tui_version PROPERTIES
  PASS_REGULAR_EXPRESSION "ABI v3, 32-byte events, 256-byte ring header")
