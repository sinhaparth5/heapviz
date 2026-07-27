# SPDX-License-Identifier: GPL-3.0-or-later
# Runs the allocation benchmark bare and under LD_PRELOAD, and enforces the
# ROADMAP.md M1.8 budget of 50 ns added per allocator call.

set(ITERATIONS 2000000)

function(measure label preload out_var)
  if(preload STREQUAL "")
    execute_process(COMMAND ${BENCH} ${ITERATIONS}
                    OUTPUT_VARIABLE out RESULT_VARIABLE rc)
  else()
    execute_process(COMMAND ${CMAKE_COMMAND} -E env
                            LD_PRELOAD=${preload}
                            HEAPVIZ_CAPACITY=1048576
                            ${BENCH} ${ITERATIONS}
                    OUTPUT_VARIABLE out RESULT_VARIABLE rc)
  endif()
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "${label} run exited ${rc}")
  endif()
  string(REGEX MATCH "([0-9]+\\.[0-9]+) ns/call" _m "${out}")
  if(NOT CMAKE_MATCH_1)
    message(FATAL_ERROR "${label}: could not parse '${out}'")
  endif()
  message(STATUS "${label}: ${CMAKE_MATCH_1} ns/call")
  set(${out_var} ${CMAKE_MATCH_1} PARENT_SCOPE)
endfunction()

# Take the best of three. The benchmark is sensitive to scheduling noise, and
# the minimum is the closest thing to the true cost.
set(baseline 1e9)
set(hooked 1e9)
foreach(run RANGE 1 3)
  measure("baseline run ${run}" "" b)
  measure("hooked   run ${run}" "${PRELOAD}" h)
  if(b LESS baseline)
    set(baseline ${b})
  endif()
  if(h LESS hooked)
    set(hooked ${h})
  endif()
endforeach()

math(EXPR _dummy "0") # keep CMake happy about the float math below
set(added "0")
execute_process(
  COMMAND ${CMAKE_COMMAND} -E echo "${hooked} ${baseline}"
  OUTPUT_QUIET)

# CMake has no float arithmetic; do the subtraction in the shell.
execute_process(
  COMMAND awk "BEGIN { printf \"%.2f\", ${hooked} - ${baseline} }"
  OUTPUT_VARIABLE added
  RESULT_VARIABLE awk_rc)

if(NOT awk_rc EQUAL 0)
  message(WARNING "awk unavailable; skipping the budget assertion")
  return()
endif()

message(STATUS "")
message(STATUS "  baseline : ${baseline} ns/call")
message(STATUS "  hooked   : ${hooked} ns/call")
message(STATUS "  added    : ${added} ns/call  (budget: 50.00)")
message(STATUS "")

execute_process(
  COMMAND awk "BEGIN { exit (${added} <= 50.0) ? 0 : 1 }"
  RESULT_VARIABLE over_budget)

if(NOT over_budget EQUAL 0)
  message(FATAL_ERROR
    "interceptor adds ${added} ns/call, over the 50 ns budget in ROADMAP.md M1.8")
endif()
