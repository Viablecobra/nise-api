cmake_minimum_required(VERSION 3.0)

add_library(example_mod SHARED
        example_mod.cpp
)

include_directories(${CMAKE_CURRENT_LIST_DIR}/../deps/gamepwnage/includes)

target_include_directories(example_mod
        PUBLIC
        ../include
)

target_link_libraries(example_mod nise)