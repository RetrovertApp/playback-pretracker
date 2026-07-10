cmake_minimum_required(VERSION 3.16)

project(pretracker LANGUAGES C)

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

if(NOT MSVC)
    target_link_libraries(pretracker PUBLIC m)
endif()
