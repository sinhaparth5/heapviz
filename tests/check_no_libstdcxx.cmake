# SPDX-License-Identifier: GPL-3.0-or-later
# libheapviz.so must not link libstdc++: its static initialisers allocate, and
# this library is loaded inside the allocator it instruments.

find_program(LDD_EXE ldd)
if(NOT LDD_EXE)
  message(WARNING "ldd not found; skipping libstdc++ check")
  return()
endif()

execute_process(COMMAND ${LDD_EXE} ${SO}
                OUTPUT_VARIABLE deps
                RESULT_VARIABLE rc)

if(NOT rc EQUAL 0)
  message(FATAL_ERROR "ldd ${SO} exited ${rc}")
endif()

if(deps MATCHES "libstdc\\+\\+")
  message("${deps}")
  message(FATAL_ERROR
    "libheapviz.so links libstdc++. Its static initialisers allocate, which "
    "deadlocks against the malloc hook. Keep src/preload/ pure C11.")
endif()

message(STATUS "libheapviz.so is free of libstdc++")
