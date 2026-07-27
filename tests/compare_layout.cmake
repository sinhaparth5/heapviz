# SPDX-License-Identifier: GPL-3.0-or-later
# Runs the C11 and C++20 ABI dumps and requires byte-identical output.

execute_process(COMMAND ${C_DUMP}   OUTPUT_VARIABLE c_out   RESULT_VARIABLE c_rc)
execute_process(COMMAND ${CXX_DUMP} OUTPUT_VARIABLE cxx_out RESULT_VARIABLE cxx_rc)

if(NOT c_rc EQUAL 0)
  message(FATAL_ERROR "abi_layout_c exited ${c_rc}")
endif()
if(NOT cxx_rc EQUAL 0)
  message(FATAL_ERROR "abi_layout_cxx exited ${cxx_rc}")
endif()

if(NOT c_out STREQUAL cxx_out)
  message("--- C11 ---\n${c_out}")
  message("--- C++20 ---\n${cxx_out}")
  message(FATAL_ERROR
    "ABI layout differs between the C11 and C++20 builds. The two halves of "
    "heapviz would disagree about the shared memory region. Check _Atomic vs "
    "std::atomic sizing in src/common/heapviz_abi.h.")
endif()

message(STATUS "ABI layout identical across C11 and C++20")
