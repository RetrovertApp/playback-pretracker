if(NOT DEFINED FUZZER OR NOT DEFINED SEED_CORPUS OR
   NOT DEFINED WORK_CORPUS OR NOT DEFINED ARTIFACT_DIR)
    message(FATAL_ERROR "Fuzzer smoke-test paths are required")
endif()

file(REMOVE_RECURSE "${WORK_CORPUS}")
file(MAKE_DIRECTORY "${WORK_CORPUS}" "${ARTIFACT_DIR}")

execute_process(
    COMMAND "${FUZZER}"
        -runs=1000
        -seed=1337
        -max_len=65536
        -timeout=5
        "-artifact_prefix=${ARTIFACT_DIR}/"
        "${WORK_CORPUS}"
        "${SEED_CORPUS}"
    RESULT_VARIABLE result
)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "Fuzzer smoke test failed with exit code ${result}")
endif()
