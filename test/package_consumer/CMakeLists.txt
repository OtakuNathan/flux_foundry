cmake_minimum_required(VERSION 3.16)

project(flux_foundry_package_consumer LANGUAGES CXX)

option(FLUX_FOUNDRY_TEST_UV "Test the installed optional libuv component" OFF)

option(FLUX_FOUNDRY_TEST_GSOURCE "Test the installed optional GSource component" OFF)
option(FLUX_FOUNDRY_TEST_IO_URING "Test the installed optional io_uring component" OFF)
option(FLUX_FOUNDRY_TEST_DPDK "Test the installed optional DPDK component" OFF)

set(required_components)
if(FLUX_FOUNDRY_TEST_UV)
    list(APPEND required_components uv_executor)
endif()
if(FLUX_FOUNDRY_TEST_GSOURCE)
    list(APPEND required_components gsource_executor)
endif()
if(FLUX_FOUNDRY_TEST_IO_URING)
    list(APPEND required_components io_uring)
endif()
if(FLUX_FOUNDRY_TEST_DPDK)
    list(APPEND required_components dpdk)
endif()
find_package(flux_foundry 0.1 CONFIG REQUIRED COMPONENTS ${required_components})

add_executable(flux_foundry_package_consumer main.cpp)
target_link_libraries(flux_foundry_package_consumer PRIVATE flux_foundry::flux_foundry)

enable_testing()
add_test(NAME flux_foundry_package_consumer COMMAND flux_foundry_package_consumer)

foreach(component io_uring dpdk)
    string(TOUPPER "${component}" upper)
    if(FLUX_FOUNDRY_TEST_${upper})
        add_executable(flux_foundry_package_${component}_consumer ${component}.cpp)
        target_link_libraries(flux_foundry_package_${component}_consumer PRIVATE flux_foundry::${component})
        set_target_properties(flux_foundry_package_${component}_consumer PROPERTIES
            CXX_STANDARD 14 CXX_STANDARD_REQUIRED YES CXX_EXTENSIONS NO)
        add_test(NAME flux_foundry_package_${component}_consumer COMMAND flux_foundry_package_${component}_consumer)
    endif()
endforeach()

if(FLUX_FOUNDRY_TEST_GSOURCE)
    add_executable(flux_foundry_package_gsource_consumer gsource.cpp)
    target_link_libraries(flux_foundry_package_gsource_consumer PRIVATE
        flux_foundry::gsource_executor)
    add_test(NAME flux_foundry_package_gsource_consumer
        COMMAND flux_foundry_package_gsource_consumer)
endif()

if(FLUX_FOUNDRY_TEST_UV)
    add_executable(flux_foundry_package_uv_consumer uv.cpp)
    target_link_libraries(flux_foundry_package_uv_consumer PRIVATE flux_foundry::uv_executor)
    add_test(NAME flux_foundry_package_uv_consumer COMMAND flux_foundry_package_uv_consumer)
endif()
