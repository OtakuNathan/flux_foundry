cmake_minimum_required(VERSION 3.16)

project(flux_foundry VERSION 0.1.0 LANGUAGES CXX)

include(GNUInstallDirs)

if(CMAKE_SOURCE_DIR STREQUAL PROJECT_SOURCE_DIR)
    set(FLUX_FOUNDRY_BUILD_TESTS_DEFAULT ON)
else()
    set(FLUX_FOUNDRY_BUILD_TESTS_DEFAULT OFF)
endif()

option(FLUX_FOUNDRY_BUILD_TESTS "Build test/probe targets under test/" ${FLUX_FOUNDRY_BUILD_TESTS_DEFAULT})
option(FLUX_FOUNDRY_WARNINGS_AS_ERRORS "Treat warnings in flux_foundry test/probe targets as errors" OFF)
option(FLUX_FOUNDRY_ENABLE_GSOURCE_EXECUTOR "Expose the Linux GLib GSource executor component target" OFF)

add_library(flux_foundry INTERFACE)
add_library(flux_foundry::flux_foundry ALIAS flux_foundry)

target_compile_features(flux_foundry INTERFACE cxx_std_14)
target_include_directories(flux_foundry INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/flux_foundry>)

if(FLUX_FOUNDRY_ENABLE_GSOURCE_EXECUTOR)
    if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
        message(FATAL_ERROR "gsource_executor requires Linux eventfd support")
    endif()

    find_package(PkgConfig REQUIRED)
    pkg_check_modules(FLUX_FOUNDRY_GLIB2 REQUIRED IMPORTED_TARGET glib-2.0)

    add_library(flux_foundry_gsource_executor INTERFACE)
    add_library(flux_foundry::gsource_executor ALIAS flux_foundry_gsource_executor)
    target_link_libraries(flux_foundry_gsource_executor INTERFACE
        flux_foundry::flux_foundry
        PkgConfig::FLUX_FOUNDRY_GLIB2)
endif()

if(FLUX_FOUNDRY_BUILD_TESTS)
    add_library(flux_foundry_test_warnings INTERFACE)
    if(MSVC)
        target_compile_options(flux_foundry_test_warnings INTERFACE
            $<$<COMPILE_LANGUAGE:CXX>:/W4>
            $<$<COMPILE_LANGUAGE:CXX>:/permissive->)
        if(FLUX_FOUNDRY_WARNINGS_AS_ERRORS)
            target_compile_options(flux_foundry_test_warnings INTERFACE $<$<COMPILE_LANGUAGE:CXX>:/WX>)
        endif()
    else()
        target_compile_options(flux_foundry_test_warnings INTERFACE
            $<$<COMPILE_LANGUAGE:CXX>:-Wall>
            $<$<COMPILE_LANGUAGE:CXX>:-Wextra>
            $<$<COMPILE_LANGUAGE:CXX>:-Wpedantic>)
        if(FLUX_FOUNDRY_WARNINGS_AS_ERRORS)
            target_compile_options(flux_foundry_test_warnings INTERFACE $<$<COMPILE_LANGUAGE:CXX>:-Werror>)
        endif()
    endif()

    include(CTest)
    add_subdirectory(test)
endif()

include(CMakePackageConfigHelpers)

set(FLUX_FOUNDRY_INSTALL_CMAKEDIR
    "${CMAKE_INSTALL_LIBDIR}/cmake/flux_foundry"
    CACHE PATH "Directory for installed flux_foundry CMake package files")

set(flux_foundry_install_targets flux_foundry)
if(TARGET flux_foundry_gsource_executor)
    set_target_properties(flux_foundry_gsource_executor PROPERTIES EXPORT_NAME gsource_executor)
    list(APPEND flux_foundry_install_targets flux_foundry_gsource_executor)
endif()

install(TARGETS ${flux_foundry_install_targets}
    EXPORT flux_foundryTargets
    INCLUDES DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/flux_foundry")

install(DIRECTORY
        base
        executor
        extension
        flow
        memory
        task
        utility
    DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/flux_foundry"
    FILES_MATCHING PATTERN "*.h")

install(FILES LICENSE
    DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/licenses/flux_foundry")

configure_package_config_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/flux_foundryConfig.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/flux_foundryConfig.cmake"
    INSTALL_DESTINATION "${FLUX_FOUNDRY_INSTALL_CMAKEDIR}")

write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/flux_foundryConfigVersion.cmake"
    VERSION "${PROJECT_VERSION}"
    COMPATIBILITY SameMinorVersion)

install(EXPORT flux_foundryTargets
    FILE flux_foundryTargets.cmake
    NAMESPACE flux_foundry::
    DESTINATION "${FLUX_FOUNDRY_INSTALL_CMAKEDIR}")

install(FILES
        "${CMAKE_CURRENT_BINARY_DIR}/flux_foundryConfig.cmake"
        "${CMAKE_CURRENT_BINARY_DIR}/flux_foundryConfigVersion.cmake"
    DESTINATION "${FLUX_FOUNDRY_INSTALL_CMAKEDIR}")
