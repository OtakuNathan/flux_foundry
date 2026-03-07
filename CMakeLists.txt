cmake_minimum_required(VERSION 3.16)

project(flux_foundry LANGUAGES CXX)

option(FLUX_FOUNDRY_BUILD_TESTS "Build test/probe targets under test/" ON)
option(FLUX_FOUNDRY_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)

add_library(flux_foundry INTERFACE)
add_library(flux_foundry::flux_foundry ALIAS flux_foundry)

# Backward-compatible aliases for previous option names.
if(DEFINED FLUX_FOUNDRY_BUILD_TESTS)
    set(FLUX_FOUNDRY_BUILD_TESTS "${FLUX_FOUNDRY_BUILD_TESTS}" CACHE BOOL "Build test/probe targets under test/" FORCE)
endif()
if(DEFINED FLUX_FOUNDRY_WARNINGS_AS_ERRORS)
    set(FLUX_FOUNDRY_WARNINGS_AS_ERRORS "${FLUX_FOUNDRY_WARNINGS_AS_ERRORS}" CACHE BOOL "Treat compiler warnings as errors" FORCE)
endif()

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

target_compile_features(flux_foundry INTERFACE cxx_std_14)
target_include_directories(flux_foundry INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}")

if(MSVC)
    add_compile_options(
        $<$<COMPILE_LANGUAGE:CXX>:/W4>
        $<$<COMPILE_LANGUAGE:CXX>:/permissive->)
    if(FLUX_FOUNDRY_WARNINGS_AS_ERRORS)
        add_compile_options($<$<COMPILE_LANGUAGE:CXX>:/WX>)
    endif()
else()
    add_compile_options(
        $<$<COMPILE_LANGUAGE:CXX>:-Wall>
        $<$<COMPILE_LANGUAGE:CXX>:-Wextra>
        $<$<COMPILE_LANGUAGE:CXX>:-Wpedantic>)
    if(FLUX_FOUNDRY_WARNINGS_AS_ERRORS)
        add_compile_options($<$<COMPILE_LANGUAGE:CXX>:-Werror>)
    endif()
endif()

if(FLUX_FOUNDRY_BUILD_TESTS)
    include(CTest)
    add_subdirectory(test)
endif()
