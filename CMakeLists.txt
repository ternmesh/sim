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
    src/lora.c
    src/rng.c
    src/sched.c
)
target_include_directories(ternsim PUBLIC include)
target_compile_options(ternsim PRIVATE ${TSIM_WARNINGS})

include(CTest)
if(BUILD_TESTING)
    foreach(t lora rng sched)
        add_executable(test_${t} tests/${t}.c)
        target_link_libraries(test_${t} PRIVATE ternsim)
        # The test files define their cases as static functions, so the prototype warning is
        # the library's rule, not theirs.
        target_compile_options(test_${t} PRIVATE ${TSIM_WARNINGS} -Wno-missing-prototypes)
        add_test(NAME ${t} COMMAND test_${t})
    endforeach()
endif()
