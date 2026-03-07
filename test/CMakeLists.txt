cmake_minimum_required(VERSION 3.18)

set(FLUX_FOUNDRY_TEST_OUTPUT_DIR "${CMAKE_SOURCE_DIR}/test/bin")

if(CMAKE_CONFIGURATION_TYPES)
    foreach(cfg IN LISTS CMAKE_CONFIGURATION_TYPES)
        string(TOUPPER "${cfg}" cfg_upper)
        set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_${cfg_upper} "${FLUX_FOUNDRY_TEST_OUTPUT_DIR}")
    endforeach()
else()
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${FLUX_FOUNDRY_TEST_OUTPUT_DIR}")
endif()

function(flux_foundry_add_probe target source)
    add_executable(${target} ${source})
    target_link_libraries(${target} PRIVATE flux_foundry::flux_foundry)
endfunction()

function(flux_foundry_add_noexc_probe target source)
    flux_foundry_add_probe(${target} ${source})
    target_compile_definitions(${target} PRIVATE FLUX_FOUNDRY_NO_EXCEPTION_STRICT=1)
    if(MSVC)
        target_compile_definitions(${target} PRIVATE _HAS_EXCEPTIONS=0)
        target_compile_options(${target} PRIVATE /EHs-c-)
    else()
        target_compile_options(${target} PRIVATE -fno-exceptions)
    endif()
endfunction()

# Smoke
flux_foundry_add_probe(flux_foundry_flow_smoke flow_smoke_test.cpp)
add_test(NAME flow_smoke COMMAND flux_foundry_flow_smoke)
set_tests_properties(flow_smoke PROPERTIES LABELS "smoke")

flux_foundry_add_probe(flux_foundry_flow_fullchain_smoke flow_fullchain_smoke.cpp)
add_test(NAME flow_fullchain_smoke COMMAND flux_foundry_flow_fullchain_smoke)
set_tests_properties(flow_fullchain_smoke PROPERTIES LABELS "smoke")

flux_foundry_add_noexc_probe(flux_foundry_flow_fullchain_smoke_noexc flow_fullchain_smoke_noexcept.cpp)
add_test(NAME flow_fullchain_smoke_noexc COMMAND flux_foundry_flow_fullchain_smoke_noexc)
set_tests_properties(flow_fullchain_smoke_noexc PROPERTIES LABELS "smoke;noexc")

flux_foundry_add_probe(flux_foundry_flow_fork_join_semantics flow_fork_join_semantics_test.cpp)
add_test(NAME flow_fork_join_semantics COMMAND flux_foundry_flow_fork_join_semantics)
set_tests_properties(flow_fork_join_semantics PROPERTIES LABELS "smoke")

flux_foundry_add_probe(flux_foundry_external_async_awaitable_probe external_async_awaitable_probe.cpp)
add_test(NAME external_async_awaitable_probe COMMAND flux_foundry_external_async_awaitable_probe)
set_tests_properties(external_async_awaitable_probe PROPERTIES LABELS "smoke;extension")

flux_foundry_add_noexc_probe(flux_foundry_external_async_awaitable_probe_noexc external_async_awaitable_probe.cpp)
add_test(NAME external_async_awaitable_probe_noexc COMMAND flux_foundry_external_async_awaitable_probe_noexc)
set_tests_properties(external_async_awaitable_probe_noexc PROPERTIES LABELS "smoke;extension;noexc")

# Stress
flux_foundry_add_probe(flux_foundry_flow_state_stress flow_state_stress.cpp)
add_test(NAME flow_state_stress COMMAND flux_foundry_flow_state_stress)
set_tests_properties(flow_state_stress PROPERTIES LABELS "stress" TIMEOUT 300)

flux_foundry_add_probe(flux_foundry_pooling_allocator_stress pooling_allocator_stress.cpp)
add_test(NAME pooling_allocator_stress COMMAND flux_foundry_pooling_allocator_stress)
set_tests_properties(pooling_allocator_stress PROPERTIES LABELS "stress" TIMEOUT 300)

# Perf (runtime benchmark probe)
flux_foundry_add_probe(flux_foundry_flow_perf flow_perf.cpp)
add_test(NAME flow_perf COMMAND flux_foundry_flow_perf)
set_tests_properties(flow_perf PROPERTIES LABELS "perf" TIMEOUT 300)

# CUDA extension demos (optional, requires nvcc)
include(CheckLanguage)
check_language(CUDA)
if(CMAKE_CUDA_COMPILER AND EXISTS "${CMAKE_CUDA_COMPILER}" AND NOT IS_DIRECTORY "${CMAKE_CUDA_COMPILER}")
    enable_language(CUDA)

    add_executable(flux_foundry_cuda_runtime_demo
        cuda_runtime_demo.cpp
        cuda_runtime_backend.cu)
    target_include_directories(flux_foundry_cuda_runtime_demo PRIVATE "${CMAKE_SOURCE_DIR}")

    if(CMAKE_CUDA_COMPILER_ID STREQUAL "NVIDIA")
        target_compile_options(flux_foundry_cuda_runtime_demo PRIVATE
            $<$<COMPILE_LANGUAGE:CUDA>:--allow-unsupported-compiler>)
    endif()

    set_target_properties(flux_foundry_cuda_runtime_demo PROPERTIES
        CUDA_STANDARD 14
        CUDA_STANDARD_REQUIRED ON
        CUDA_EXTENSIONS OFF
        CUDA_ARCHITECTURES 52)

    add_test(NAME cuda_runtime_demo COMMAND flux_foundry_cuda_runtime_demo)
    set_tests_properties(cuda_runtime_demo PROPERTIES LABELS "demo;extension;cuda")
else()
    message(STATUS "CUDA compiler not found; skipping flux_foundry_cuda_runtime_demo")
endif()


if(TARGET flux_foundry_cuda_runtime_demo)
    add_executable(flux_foundry_cuda_image_demo
        cuda_image_demo.cpp
        cuda_image_backend.cu)
    target_include_directories(flux_foundry_cuda_image_demo PRIVATE "${CMAKE_SOURCE_DIR}")

    if(CMAKE_CUDA_COMPILER_ID STREQUAL "NVIDIA")
        target_compile_options(flux_foundry_cuda_image_demo PRIVATE
            $<$<COMPILE_LANGUAGE:CUDA>:--allow-unsupported-compiler>)
    endif()

    set_target_properties(flux_foundry_cuda_image_demo PROPERTIES
        CUDA_STANDARD 14
        CUDA_STANDARD_REQUIRED ON
        CUDA_EXTENSIONS OFF
        CUDA_ARCHITECTURES 52)

    add_test(NAME cuda_image_demo COMMAND flux_foundry_cuda_image_demo)
    set_tests_properties(cuda_image_demo PROPERTIES
        LABELS "demo;extension;cuda"
        ENVIRONMENT "FLUX_FOUNDRY_CUDA_DEMO_NO_OPEN=1")
endif()
