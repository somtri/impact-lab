# Runs engine_replay --synthetic twice and compares both outputs against each other and against
# the committed golden file. Driven by ctest; see tests/CMakeLists.txt.

set(FIRST "${WORKDIR}/replay_run1.txt")
set(SECOND "${WORKDIR}/replay_run2.txt")

foreach(OUT "${FIRST}" "${SECOND}")
    execute_process(
        COMMAND "${REPLAY}" --synthetic
        OUTPUT_FILE "${OUT}"
        RESULT_VARIABLE status
    )
    if(NOT status EQUAL 0)
        message(FATAL_ERROR "engine_replay --synthetic exited with ${status}")
    endif()
endforeach()

execute_process(COMMAND ${CMAKE_COMMAND} -E compare_files "${FIRST}" "${SECOND}"
                RESULT_VARIABLE same_runs)
if(NOT same_runs EQUAL 0)
    message(FATAL_ERROR "two runs of engine_replay --synthetic differ")
endif()

execute_process(COMMAND ${CMAKE_COMMAND} -E compare_files "${FIRST}" "${GOLDEN}"
                RESULT_VARIABLE same_golden)
if(NOT same_golden EQUAL 0)
    message(FATAL_ERROR "engine_replay --synthetic output differs from ${GOLDEN}")
endif()
