# heapviz test suite. SPDX-License-Identifier: GPL-3.0-or-later

function(heapviz_test_target name)
  target_compile_options(${name} PRIVATE ${HEAPVIZ_WARNINGS})
  target_link_libraries(${name} PRIVATE heapviz_abi rt)
  target_include_directories(${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  if(HEAPVIZ_ASAN)
    target_compile_options(${name} PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(${name}    PRIVATE -fsanitize=address,undefined)
  endif()
endfunction()

# --- ABI layout: the same dump compiled as C11 and as C++20 ----------------
add_executable(abi_layout_c   abi_layout_c.c)
add_executable(abi_layout_cxx abi_layout_cxx.cpp)
heapviz_test_target(abi_layout_c)
heapviz_test_target(abi_layout_cxx)

add_test(NAME abi_layout_matches
  COMMAND ${CMAKE_COMMAND}
          -DC_DUMP=$<TARGET_FILE:abi_layout_c>
          -DCXX_DUMP=$<TARGET_FILE:abi_layout_cxx>
          -P ${CMAKE_CURRENT_SOURCE_DIR}/compare_layout.cmake)

# --- Shared memory round-trip: C11 writes, C++20 reads ---------------------
add_executable(shm_probe_writer shm_probe_writer.c)
add_executable(shm_probe_reader shm_probe_reader.cpp)
heapviz_test_target(shm_probe_writer)
heapviz_test_target(shm_probe_reader)

add_test(NAME shm_roundtrip
  COMMAND ${CMAKE_COMMAND}
          -DWRITER=$<TARGET_FILE:shm_probe_writer>
          -DREADER=$<TARGET_FILE:shm_probe_reader>
          -P ${CMAKE_CURRENT_SOURCE_DIR}/run_shm_probe.cmake)

# --- The .so must not drag in libstdc++ ------------------------------------
add_test(NAME preload_no_libstdcxx
  COMMAND ${CMAKE_COMMAND}
          -DSO=$<TARGET_FILE:heapviz_preload>
          -P ${CMAKE_CURRENT_SOURCE_DIR}/check_no_libstdcxx.cmake)

# --- ...and must actually load, so the check above cannot pass vacuously ---
add_executable(preload_load preload_load.c)
heapviz_test_target(preload_load)
target_link_libraries(preload_load PRIVATE dl)
add_test(NAME preload_loads
  COMMAND preload_load $<TARGET_FILE:heapviz_preload>)

# --- Multi-producer ring stress -------------------------------------------
add_executable(ring_test ring_test.c)
heapviz_test_target(ring_test)
target_link_libraries(ring_test PRIVATE pthread)
# 4 producers x 2.5M events through a 4096-slot ring: ~2400 wraps, which is what
# actually exercises the lap-parity bit.
add_test(NAME ring_mpsc COMMAND ring_test 4 10000000)
set_tests_properties(ring_mpsc PROPERTIES TIMEOUT 120)

# --- End-to-end: real LD_PRELOAD interception across a process boundary ----
add_executable(intercept_e2e intercept_e2e.cpp)
heapviz_test_target(intercept_e2e)
# ASan supplies its own malloc, so LD_PRELOAD-ing ours into an ASan-built
# target is the combination that cannot coexist (ROADMAP.md M0.1). Skip the
# preload-driven tests there; they run in debug and release.
if(NOT HEAPVIZ_ASAN)
  add_test(NAME intercept_end_to_end
    COMMAND intercept_e2e $<TARGET_FILE:heapviz_preload> $<TARGET_FILE:churn>)
  set_tests_properties(intercept_end_to_end PROPERTIES TIMEOUT 120)
endif()

# --- Transitive coverage: strdup/asprintf/getline/operator new -------------
add_executable(transitive_target transitive_target.cpp)
target_compile_options(transitive_target PRIVATE ${HEAPVIZ_WARNINGS})
target_compile_definitions(transitive_target PRIVATE _GNU_SOURCE)

add_executable(transitive_e2e transitive_e2e.cpp)
heapviz_test_target(transitive_e2e)
if(NOT HEAPVIZ_ASAN)
  add_test(NAME transitive_interception
    COMMAND transitive_e2e $<TARGET_FILE:heapviz_preload> $<TARGET_FILE:transitive_target>)
  set_tests_properties(transitive_interception PROPERTIES TIMEOUT 120)
endif()

# --- Interceptor overhead budget (M1.8: under 50 ns/call) ------------------
add_executable(overhead_bench overhead_bench.c)
heapviz_test_target(overhead_bench)
# Only meaningful on an optimised build. At -O0 the interceptor costs 45-56 ns
# and straddles the budget, which made this test flaky in debug for no useful
# reason: the 50 ns figure is a claim about what ships, not about -O0.
if(NOT HEAPVIZ_ASAN AND CMAKE_BUILD_TYPE MATCHES "^(Release|RelWithDebInfo)$")
  add_test(NAME interceptor_overhead
    COMMAND ${CMAKE_COMMAND}
            -DBENCH=$<TARGET_FILE:overhead_bench>
            -DPRELOAD=$<TARGET_FILE:heapviz_preload>
            -P ${CMAKE_CURRENT_SOURCE_DIR}/run_overhead.cmake)
  set_tests_properties(interceptor_overhead PROPERTIES TIMEOUT 300)
endif()

# --- Terminal setup/teardown (M4.1) ----------------------------------------
# Needs a real terminal to be meaningful, so the test allocates a pty and runs
# each exit path in a child attached to it. -lutil is for openpty.
add_executable(terminal_test terminal_test.cpp)
heapviz_test_target(terminal_test)
target_link_libraries(terminal_test PRIVATE heapviz_core util)
add_test(NAME terminal_restore COMMAND terminal_test)
set_tests_properties(terminal_restore PROPERTIES TIMEOUT 60)

# --- Framebuffer: clipping and the no-allocation rule (M4.2) ---------------
add_executable(framebuffer_test framebuffer_test.cpp)
heapviz_test_target(framebuffer_test)
target_link_libraries(framebuffer_test PRIVATE heapviz_core)
add_test(NAME framebuffer COMMAND framebuffer_test)
set_tests_properties(framebuffer PROPERTIES TIMEOUT 60)

# --- Differential ANSI streamer (M4.3) -------------------------------------
add_executable(renderer_test renderer_test.cpp)
heapviz_test_target(renderer_test)
target_link_libraries(renderer_test PRIVATE heapviz_core)
add_test(NAME renderer COMMAND renderer_test)
set_tests_properties(renderer PROPERTIES TIMEOUT 60)

# --- The TUI binary runs ----------------------------------------------------
add_test(NAME tui_version COMMAND heapviz_tui --version)
set_tests_properties(tui_version PROPERTIES
  PASS_REGULAR_EXPRESSION "ABI v2, 32-byte events, 256-byte ring header")
