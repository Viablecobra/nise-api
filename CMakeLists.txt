cmake_minimum_required(VERSION 3.0)

add_library(nise INTERFACE IMPORTED)

include(${CMAKE_CURRENT_LIST_DIR}/deps/gamepwnage.cmake)

set_property(TARGET nise PROPERTY
        INTERFACE_INCLUDE_DIRECTORIES
        ${CMAKE_CURRENT_LIST_DIR}
)

set_property(TARGET nise PROPERTY
        INTERFACE_LINK_LIBRARIES
        gamepwnage
        "-L${CMAKE_CURRENT_LIST_DIR}/libs/${ANDROID_ABI} -lnise"
)
add_subdirectory(example)