cmake_minimum_required(VERSION 3.16)

project(pretracker LANGUAGES C)

include(CTest)

option(PRETRACKER_ENABLE_SANITIZERS "Enable AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
option(PRETRACKER_BUILD_FUZZER "Build the Clang libFuzzer parser/playback target" OFF)

add_library(pretracker STATIC
    pretracker/pretracker.c
    pretracker/pretracker_mixer.c
    pretracker/pretracker_parse.c
    pretracker/pretracker_sequencer.c
    pretracker/pretracker_tables.c
    pretracker/pretracker_wavegen.c
)

target_include_directories(pretracker PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/pretracker>
)
target_compile_features(pretracker PUBLIC c_std_11)
set_target_properties(pretracker PROPERTIES POSITION_INDEPENDENT_CODE ON)

if(PRETRACKER_ENABLE_SANITIZERS)
    if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
        target_compile_options(pretracker PRIVATE
            -fsanitize=address,undefined
            -fno-omit-frame-pointer
            -fno-sanitize-recover=all
        )
        target_link_options(pretracker INTERFACE -fsanitize=address,undefined)
    else()
        message(FATAL_ERROR "PRETRACKER_ENABLE_SANITIZERS requires Clang or GCC")
    endif()
endif()

if(NOT MSVC)
    target_link_libraries(pretracker PUBLIC m)
endif()

if(BUILD_TESTING)
    add_executable(pretracker_parse_tests tests/pretracker_parse_tests.c)
    target_link_libraries(pretracker_parse_tests PRIVATE pretracker)
    target_include_directories(pretracker_parse_tests PRIVATE pretracker)
    if(PRETRACKER_ENABLE_SANITIZERS)
        target_compile_options(pretracker_parse_tests PRIVATE
            -fsanitize=address,undefined
            -fno-omit-frame-pointer
            -fno-sanitize-recover=all
        )
    endif()
    add_test(NAME pretracker_parse_tests COMMAND pretracker_parse_tests)

    add_executable(pretracker_audio_regression_tests tests/pretracker_audio_regression_tests.c)
    target_link_libraries(pretracker_audio_regression_tests PRIVATE pretracker)
    target_include_directories(pretracker_audio_regression_tests PRIVATE pretracker)
    if(PRETRACKER_ENABLE_SANITIZERS)
        target_compile_options(pretracker_audio_regression_tests PRIVATE
            -fsanitize=address,undefined
            -fno-omit-frame-pointer
            -fno-sanitize-recover=all
        )
    endif()
    add_test(NAME pretracker_audio_regression_tests COMMAND pretracker_audio_regression_tests)
endif()

if(PRETRACKER_BUILD_FUZZER)
    if(NOT PRETRACKER_ENABLE_SANITIZERS)
        message(FATAL_ERROR "PRETRACKER_BUILD_FUZZER requires PRETRACKER_ENABLE_SANITIZERS=ON")
    endif()
    if(NOT CMAKE_C_COMPILER_ID MATCHES "Clang")
        message(FATAL_ERROR "PRETRACKER_BUILD_FUZZER requires Clang and libFuzzer")
    endif()

    target_compile_options(pretracker PRIVATE -fsanitize=fuzzer-no-link)

    add_executable(pretracker_fuzz tests/fuzz/pretracker_fuzz.c)
    target_link_libraries(pretracker_fuzz PRIVATE pretracker)
    target_include_directories(pretracker_fuzz PRIVATE pretracker)
    target_compile_options(pretracker_fuzz PRIVATE
        -fsanitize=fuzzer,address,undefined
        -fno-omit-frame-pointer
        -fno-sanitize-recover=all
    )
    target_link_options(pretracker_fuzz PRIVATE -fsanitize=fuzzer,address,undefined)

    add_executable(pretracker_fuzz_seed_corpus tests/fuzz/make_seed_corpus.c)
    set(PRETRACKER_FUZZ_CORPUS_DIR "${CMAKE_CURRENT_BINARY_DIR}/fuzz-corpus")
    set(PRETRACKER_FUZZ_ARTIFACT_DIR "${CMAKE_CURRENT_BINARY_DIR}/fuzz-artifacts")
    set(PRETRACKER_FUZZ_SEEDS
        "${PRETRACKER_FUZZ_CORPUS_DIR}/legacy-1b.prt"
        "${PRETRACKER_FUZZ_CORPUS_DIR}/v15-two-subsongs.prt"
    )
    add_custom_command(
        OUTPUT ${PRETRACKER_FUZZ_SEEDS}
        COMMAND ${CMAKE_COMMAND} -E make_directory "${PRETRACKER_FUZZ_CORPUS_DIR}"
        COMMAND $<TARGET_FILE:pretracker_fuzz_seed_corpus> "${PRETRACKER_FUZZ_CORPUS_DIR}"
        DEPENDS pretracker_fuzz_seed_corpus
        VERBATIM
    )
    add_custom_target(pretracker_fuzz_corpus ALL DEPENDS ${PRETRACKER_FUZZ_SEEDS})
    add_dependencies(pretracker_fuzz pretracker_fuzz_corpus)

    if(BUILD_TESTING)
        file(MAKE_DIRECTORY "${PRETRACKER_FUZZ_ARTIFACT_DIR}")
        add_test(NAME pretracker_fuzz_smoke
            COMMAND ${CMAKE_COMMAND}
                "-DFUZZER=$<TARGET_FILE:pretracker_fuzz>"
                "-DSEED_CORPUS=${PRETRACKER_FUZZ_CORPUS_DIR}"
                "-DWORK_CORPUS=${CMAKE_CURRENT_BINARY_DIR}/fuzz-smoke-corpus"
                "-DARTIFACT_DIR=${PRETRACKER_FUZZ_ARTIFACT_DIR}"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/fuzz/run_smoke.cmake"
        )
        set_tests_properties(pretracker_fuzz_smoke PROPERTIES
            ENVIRONMENT "ASAN_OPTIONS=abort_on_error=1:detect_leaks=1;UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1"
        )
    endif()
endif()
