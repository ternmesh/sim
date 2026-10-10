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
    src/distvec.c
    src/listen.c
    src/lora.c
    src/meshcore.c
    src/meshtastic.c
    src/metrics.c
    src/net.c
    src/nodeset.c
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

# The firmware's protocol core (ternmesh/firmware), which the `core` routing runs: the code a
# device runs, measured on the scenarios the design was. It is built from the firmware's source,
# from a checkout beside this one unless TSIM_FIRMWARE_DIR names another. Without one the simulator
# builds as before, and has no `core` routing.
set(TSIM_FIRMWARE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../firmware" CACHE PATH
    "A checkout of ternmesh/firmware, for the core routing")
option(TSIM_REQUIRE_CORE "Fail if the firmware's source is not there to build the core from" OFF)
# A checkout from before the firmware's flooder was told how busy its radio is
# (ternmesh/firmware#45, after v0.1.0-alpha.4) cannot build the core as it is here, and is as good
# as none.
set(TSIM_FIRMWARE_NEW_ENOUGH OFF)
if(EXISTS "${TSIM_FIRMWARE_DIR}/src/forward.c" AND
   EXISTS "${TSIM_FIRMWARE_DIR}/include/tern/flood.h")
    file(STRINGS "${TSIM_FIRMWARE_DIR}/include/tern/flood.h" TSIM_FLOOD_RADIO
         REGEX "tern_flood_radio")
    if(TSIM_FLOOD_RADIO)
        set(TSIM_FIRMWARE_NEW_ENOUGH ON)
    endif()
endif()
if(TSIM_FIRMWARE_NEW_ENOUGH)
    add_library(terncore STATIC
        ${TSIM_FIRMWARE_DIR}/src/flood.c
        ${TSIM_FIRMWARE_DIR}/src/forward.c
        ${TSIM_FIRMWARE_DIR}/src/lora.c
        ${TSIM_FIRMWARE_DIR}/src/route.c
        ${TSIM_FIRMWARE_DIR}/src/sha256.c
        ${TSIM_FIRMWARE_DIR}/src/hkdf.c
    )
    target_include_directories(terncore PUBLIC ${TSIM_FIRMWARE_DIR}/include)
    target_compile_options(terncore PRIVATE ${TSIM_WARNINGS})
    target_sources(ternsim PRIVATE src/core.c)
    target_link_libraries(ternsim PUBLIC terncore)
    target_compile_definitions(ternsim PUBLIC TSIM_HAVE_CORE)
    # The tables a board keeps, read from the node the boards run (ports/node/node.c), so that the
    # core is measured by default with the memory a device has and not more. A checkout without
    # one, or without a size, leaves that size to tsim_core_default()'s own.
    set(TSIM_NODE_C "${TSIM_FIRMWARE_DIR}/ports/node/node.c")
    foreach(pair "NEIGHBOURS;NEIGHBOURS" "DESTINATIONS;DESTINATIONS" "FORWARD_SLOTS;FRAMES"
                 "FLOOD_SLOTS;FLOOD_FRAMES")
        list(GET pair 0 from)
        list(GET pair 1 to)
        if(EXISTS "${TSIM_NODE_C}")
            file(STRINGS "${TSIM_NODE_C}" line REGEX "^#define ${from} [0-9]+")
            if(line)
                list(GET line 0 line)
                string(REGEX REPLACE "^#define ${from} ([0-9]+).*" "\\1" n "${line}")
                target_compile_definitions(ternsim PRIVATE TSIM_BOARD_${to}=${n})
                message(STATUS "Core routing: a board's ${from} is ${n}")
            endif()
        endif()
    endforeach()
    set(TSIM_HAVE_CORE ON)
    message(STATUS "Core routing: from ${TSIM_FIRMWARE_DIR}")
elseif(TSIM_REQUIRE_CORE)
    message(FATAL_ERROR "No firmware source at ${TSIM_FIRMWARE_DIR}, or one from before "
        "ternmesh/firmware#45, which has no tern_flood_radio()")
else()
    message(STATUS "Core routing: none, no firmware source at ${TSIM_FIRMWARE_DIR}, or one from "
        "before ternmesh/firmware#45")
endif()

add_executable(tsim tools/tsim.c tools/json.c)
target_link_libraries(tsim PRIVATE ternsim)
target_compile_options(tsim PRIVATE ${TSIM_WARNINGS})

include(CTest)
if(BUILD_TESTING)
    foreach(t baseline channel distvec listen lora meshcore meshtastic metrics net phy place rng scenario sched traffic)
        add_executable(test_${t} tests/${t}.c)
        target_link_libraries(test_${t} PRIVATE ternsim)
        # The test files define their cases as static functions, so the prototype warning is
        # the library's rule, not theirs.
        target_compile_options(test_${t} PRIVATE ${TSIM_WARNINGS} -Wno-missing-prototypes)
        add_test(NAME ${t} COMMAND test_${t})
    endforeach()

    if(TSIM_HAVE_CORE)
        add_executable(test_core tests/core.c)
        target_link_libraries(test_core PRIVATE ternsim)
        target_compile_options(test_core PRIVATE ${TSIM_WARNINGS} -Wno-missing-prototypes)
        add_test(NAME core COMMAND test_core)
        file(GLOB TSIM_CORE_SCENARIOS CONFIGURE_DEPENDS
            ${CMAKE_CURRENT_SOURCE_DIR}/scenarios/core/*.tsim)
        foreach(s ${TSIM_CORE_SCENARIOS})
            get_filename_component(name ${s} NAME_WE)
            add_test(NAME scenario_core_${name} COMMAND tsim ${s})
        endforeach()
    endif()

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

    # Candidate 1's compatibility mode against Meshtasticator's results on the same maps, which
    # tools/meshtasticator.py recorded in scenarios/meshtasticator/, and candidate 2 against
    # MeshBench's on its Fife network, which tools/meshbench.py recorded in scenarios/meshbench/.
    # Only the comparisons run here; Meshtasticator and MeshBench themselves are run by hand.
    find_package(Python3 COMPONENTS Interpreter)
    if(Python3_Interpreter_FOUND)
        add_test(NAME meshtasticator
            COMMAND Python3::Interpreter ${CMAKE_CURRENT_SOURCE_DIR}/tools/meshtasticator.py check
                --tsim $<TARGET_FILE:tsim>)
        add_test(NAME meshbench
            COMMAND Python3::Interpreter ${CMAKE_CURRENT_SOURCE_DIR}/tools/meshbench.py check
                --tsim $<TARGET_FILE:tsim>)
        # The firmware's core on the town: the routes it holds, and that it keeps to its cap.
        if(TSIM_HAVE_CORE)
            add_test(NAME core_town
                COMMAND Python3::Interpreter ${CMAKE_CURRENT_SOURCE_DIR}/tools/core.py check
                    --tsim $<TARGET_FILE:tsim>)
        endif()
    endif()
endif()
