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

# --- Differential ANSI streamer (M4.3) -------------------------------------
add_executable(renderer_test unit/renderer_test.cpp)
heapviz_test_target(renderer_test)
target_link_libraries(renderer_test PRIVATE heapviz_core)
add_test(NAME renderer COMMAND renderer_test)
set_tests_properties(renderer PROPERTIES TIMEOUT 60)

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
  PASS_REGULAR_EXPRESSION "ABI v2, 32-byte events, 256-byte ring header")
