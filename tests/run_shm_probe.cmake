# SPDX-License-Identifier: GPL-3.0-or-later
# C11 writer creates and fills a ring segment; C++20 reader validates it.

execute_process(COMMAND ${WRITER}
                OUTPUT_VARIABLE writer_pid
                RESULT_VARIABLE writer_rc
                OUTPUT_STRIP_TRAILING_WHITESPACE)

if(NOT writer_rc EQUAL 0)
  message(FATAL_ERROR "shm_probe_writer exited ${writer_rc}")
endif()
if(NOT writer_pid MATCHES "^[0-9]+$")
  message(FATAL_ERROR "shm_probe_writer printed '${writer_pid}', expected a pid")
endif()

execute_process(COMMAND ${READER} ${writer_pid} RESULT_VARIABLE reader_rc)

if(NOT reader_rc EQUAL 0)
  message(FATAL_ERROR
    "shm_probe_reader exited ${reader_rc}: the C++20 consumer disagrees with "
    "the C11 producer about the shared memory layout.")
endif()
