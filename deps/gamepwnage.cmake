set(GAMEPWNAGE_ROOT
        ${CMAKE_CURRENT_LIST_DIR}/gamepwnage
)

add_library(gamepwnage STATIC
        ${GAMEPWNAGE_ROOT}/src/armhook.c
        ${GAMEPWNAGE_ROOT}/src/inlinehook.c
        ${GAMEPWNAGE_ROOT}/src/mem.c
        ${GAMEPWNAGE_ROOT}/src/nop.c
        ${GAMEPWNAGE_ROOT}/src/proc.c
        ${GAMEPWNAGE_ROOT}/src/memscan.c
        ${GAMEPWNAGE_ROOT}/src/plthook.c
        ${GAMEPWNAGE_ROOT}/src/dynlib.c
)

target_include_directories(gamepwnage
        PUBLIC
        ${GAMEPWNAGE_ROOT}/src
        ${GAMEPWNAGE_ROOT}/includes
)
