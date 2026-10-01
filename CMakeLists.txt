cmake_minimum_required(VERSION 3.20)
project(ternsim VERSION 0.0.0 LANGUAGES C)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "" FORCE)
endif()

option(TSIM_WERROR "Treat warnings as errors" OFF)
option(TSIM_SANITIZE "Build with AddressSanitizer and UndefinedBehaviorSanitizer" OFF)

set(TSIM_WARNINGS -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wno-sign-conversion
    -Wstrict-prototypes -Wmissing-prototypes)
if(TSIM_WERROR)
    list(APPEND TSIM_WARNINGS -Werror)
endif()

if(TSIM_SANITIZE)
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer
        -fno-sanitize-recover=all)
    add_link_options(-fsanitize=address,undefined)
endif()

add_library(ternsim
    src/baseline.c
    src/channel.c
    src/lora.c
    src/metrics.c
    src/net.c
    src/phy.c
    src/place.c
    src/rng.c
    src/scenario.c
    src/sched.c
    src/traffic.c
)
target_include_directories(ternsim PUBLIC include)
target_link_libraries(ternsim PUBLIC m)
target_compile_options(ternsim PRIVATE ${TSIM_WARNINGS})

add_executable(tsim tools/tsim.c tools/json.c)
target_link_libraries(tsim PRIVATE ternsim)
target_compile_options(tsim PRIVATE ${TSIM_WARNINGS})

include(CTest)
if(BUILD_TESTING)
    foreach(t baseline channel lora metrics net phy place rng scenario sched traffic)
        add_executable(test_${t} tests/${t}.c)
        target_link_libraries(test_${t} PRIVATE ternsim)
        # The test files define their cases as static functions, so the prototype warning is
        # the library's rule, not theirs.
        target_compile_options(test_${t} PRIVATE ${TSIM_WARNINGS} -Wno-missing-prototypes)
        add_test(NAME ${t} COMMAND test_${t})
    endforeach()

    # The tool's JSON writer, which is not the library's.
    add_executable(test_json tests/json.c tools/json.c)
    target_include_directories(test_json PRIVATE tools)
    target_compile_options(test_json PRIVATE ${TSIM_WARNINGS} -Wno-missing-prototypes)
    add_test(NAME json COMMAND test_json)

    # Every scenario under scenarios/ runs as a test, through the tool, so a scenario that stops
    # parsing or running fails CI. Those under scenarios/scale/ are too big for a sanitized debug
    # build and run in CI's timing job instead.
    file(GLOB TSIM_SCENARIOS CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/scenarios/*.tsim)
    foreach(s ${TSIM_SCENARIOS})
        get_filename_component(name ${s} NAME_WE)
        add_test(NAME scenario_${name} COMMAND tsim ${s})
    endforeach()
endif()
