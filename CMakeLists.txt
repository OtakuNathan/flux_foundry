cmake_minimum_required(VERSION 3.16)

project(flux_foundry LANGUAGES CXX)

option(FLUX_FOUNDRY_BUILD_TESTS "Build test/probe targets under test/" ON)
option(FLUX_FOUNDRY_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)

# Backward-compatible aliases for previous option names.
if(DEFINED FLUEX_FOUNDRY_BUILD_TESTS)
    set(FLUX_FOUNDRY_BUILD_TESTS "${FLUEX_FOUNDRY_BUILD_TESTS}" CACHE BOOL "Build test/probe targets under test/" FORCE)
endif()
if(DEFINED FLUEX_FOUNDRY_WARNINGS_AS_ERRORS)
    set(FLUX_FOUNDRY_WARNINGS_AS_ERRORS "${FLUEX_FOUNDRY_WARNINGS_AS_ERRORS}" CACHE BOOL "Treat compiler warnings as errors" FORCE)
endif()

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
    add_compile_options(/W4 /permissive-)
    if(FLUX_FOUNDRY_WARNINGS_AS_ERRORS)
        add_compile_options(/WX)
    endif()
else()
    add_compile_options(-Wall -Wextra -Wpedantic)
    if(FLUX_FOUNDRY_WARNINGS_AS_ERRORS)
        add_compile_options(-Werror)
    endif()
endif()

if(FLUX_FOUNDRY_BUILD_TESTS)
    include(CTest)
    add_subdirectory(test)
endif()
