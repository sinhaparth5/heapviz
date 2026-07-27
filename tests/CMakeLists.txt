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

# --- The TUI binary runs ----------------------------------------------------
add_test(NAME tui_version COMMAND heapviz_tui --version)
set_tests_properties(tui_version PROPERTIES
  PASS_REGULAR_EXPRESSION "ABI v1, 32-byte events, 256-byte ring header")
