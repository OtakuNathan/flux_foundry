cmake_minimum_required(VERSION 3.16)

project(flux_foundry_package_consumer LANGUAGES CXX)

option(FLUX_FOUNDRY_TEST_GSOURCE "Test the installed optional GSource component" OFF)

if(FLUX_FOUNDRY_TEST_GSOURCE)
    find_package(flux_foundry 0.1 CONFIG REQUIRED COMPONENTS gsource_executor)
else()
    find_package(flux_foundry 0.1 CONFIG REQUIRED)
endif()

add_executable(flux_foundry_package_consumer main.cpp)
target_link_libraries(flux_foundry_package_consumer PRIVATE flux_foundry::flux_foundry)

enable_testing()
add_test(NAME flux_foundry_package_consumer COMMAND flux_foundry_package_consumer)

if(FLUX_FOUNDRY_TEST_GSOURCE)
    add_executable(flux_foundry_package_gsource_consumer gsource.cpp)
    target_link_libraries(flux_foundry_package_gsource_consumer PRIVATE
        flux_foundry::gsource_executor)
    add_test(NAME flux_foundry_package_gsource_consumer
        COMMAND flux_foundry_package_gsource_consumer)
endif()
