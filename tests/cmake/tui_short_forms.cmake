# SPDX-License-Identifier: GPL-3.0-or-later
# Holds the positional dispatch rule: a digits-only argument attaches, anything
# else launches. Each case is checked by the error it reaches, because all three
# fail before terminal setup -- which is what lets this run without a pty.
#
# The launch and instrument cases use a path that cannot exist, so what they
# assert is which code path handled the argument, not that exec works. The pid
# case uses INT_MAX because no such process can be running, so the attach is
# answered rather than waited out.

execute_process(
  COMMAND "${HEAPVIZ}" /definitely/not/a/real/heapviz-target
  RESULT_VARIABLE launch_result
  ERROR_VARIABLE launch_error)
if(launch_result EQUAL 0 OR
   NOT launch_error MATCHES "could not run the command")
  message(FATAL_ERROR "positional command did not use the launch path: ${launch_error}")
endif()

execute_process(
  COMMAND "${HEAPVIZ}" 2147483647
  RESULT_VARIABLE pid_result
  ERROR_VARIABLE pid_error)
if(pid_result EQUAL 0 OR
   NOT pid_error MATCHES "target exited before it could be attached")
  message(FATAL_ERROR "numeric positional did not use the PID path: ${pid_error}")
endif()

execute_process(
  COMMAND "${HEAPVIZ}" --instrument /definitely/not/a/real/heapviz-target
  RESULT_VARIABLE instrument_result
  OUTPUT_VARIABLE instrument_output
  ERROR_VARIABLE instrument_error)
if(instrument_result EQUAL 0 OR
   NOT instrument_output MATCHES "instrumented target PID" OR
   NOT instrument_error MATCHES "could not run the command")
  message(FATAL_ERROR
    "interactive instrumentation path failed: ${instrument_output}${instrument_error}")
endif()
