cmake_minimum_required(VERSION 3.18)

function(flux_foundry_add_probe target source)
    add_executable(${target} ${source})
    target_link_libraries(${target} PRIVATE flux_foundry::flux_foundry flux_foundry_test_warnings)
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

flux_foundry_add_probe(flux_foundry_core_edge_cases core_edge_cases.cpp)
add_test(NAME core_edge_cases COMMAND flux_foundry_core_edge_cases)
set_tests_properties(core_edge_cases PROPERTIES LABELS "smoke")

flux_foundry_add_probe(flux_foundry_runner_reuse_contract runner_reuse_contract.cpp)
add_test(NAME runner_reuse_contract COMMAND flux_foundry_runner_reuse_contract)
set_tests_properties(runner_reuse_contract PROPERTIES LABELS "smoke")

flux_foundry_add_probe(flux_foundry_hazard_ptr_contract hazard_ptr_contract.cpp)
add_test(NAME hazard_ptr_contract COMMAND flux_foundry_hazard_ptr_contract)
set_tests_properties(hazard_ptr_contract PROPERTIES LABELS "smoke" TIMEOUT 30)

flux_foundry_add_noexc_probe(flux_foundry_hazard_ptr_contract_noexc hazard_ptr_contract.cpp)
add_test(NAME hazard_ptr_contract_noexc COMMAND flux_foundry_hazard_ptr_contract_noexc)
set_tests_properties(hazard_ptr_contract_noexc PROPERTIES LABELS "smoke;noexc" TIMEOUT 30)

# Stress
flux_foundry_add_probe(flux_foundry_flow_state_stress flow_state_stress.cpp)
add_test(NAME flow_state_stress COMMAND flux_foundry_flow_state_stress)
set_tests_properties(flow_state_stress PROPERTIES LABELS "stress" TIMEOUT 300)

flux_foundry_add_probe(flux_foundry_pooling_allocator_stress pooling_allocator_stress.cpp)
add_test(NAME pooling_allocator_stress COMMAND flux_foundry_pooling_allocator_stress)
set_tests_properties(pooling_allocator_stress PROPERTIES LABELS "stress" TIMEOUT 300)

flux_foundry_add_probe(flux_foundry_hazard_ptr_stack_stress hazard_ptr_stack_stress.cpp)
add_test(NAME hazard_ptr_stack_stress
    COMMAND flux_foundry_hazard_ptr_stack_stress 2 2 10000)
set_tests_properties(hazard_ptr_stack_stress PROPERTIES LABELS "stress" TIMEOUT 120)

flux_foundry_add_probe(flux_foundry_flow_controller_memorder_stress flow_controller_memorder_stress.cpp)
add_test(NAME flow_controller_memorder_stress
    COMMAND flux_foundry_flow_controller_memorder_stress 200 200 200 200)
set_tests_properties(flow_controller_memorder_stress PROPERTIES LABELS "stress" TIMEOUT 120)

flux_foundry_add_probe(flux_foundry_flow_multiflow_cancel_stress flow_state_multiflow_cancel_stress.cpp)
add_test(NAME flow_multiflow_cancel_stress
    COMMAND flux_foundry_flow_multiflow_cancel_stress 4 100 2 2 500)
set_tests_properties(flow_multiflow_cancel_stress PROPERTIES LABELS "stress" TIMEOUT 120)

flux_foundry_add_probe(flux_foundry_flow_multiflow_cancel_executor_stress
    flow_state_multiflow_cancel_simple_executor_stress.cpp)
add_test(NAME flow_multiflow_cancel_executor_stress
    COMMAND flux_foundry_flow_multiflow_cancel_executor_stress 4 100 2 2 500)
set_tests_properties(flow_multiflow_cancel_executor_stress PROPERTIES LABELS "stress" TIMEOUT 120)

flux_foundry_add_probe(flux_foundry_inline_executor_probe inline_executor_probe.cpp)
add_test(NAME inline_executor_probe COMMAND flux_foundry_inline_executor_probe)
set_tests_properties(inline_executor_probe PROPERTIES LABELS "stress" TIMEOUT 120)

flux_foundry_add_probe(flux_foundry_queue_stress test_queues.cpp)
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    # GCC 14 mistakes the intentionally disengaged payload bytes in inplace_t<T>
    # for a read during an optimized empty return from the SPMC deque.
    target_compile_options(flux_foundry_queue_stress PRIVATE -Wno-maybe-uninitialized)
endif()
add_test(NAME queue_stress_quick COMMAND flux_foundry_queue_stress 1 quick)
set_tests_properties(queue_stress_quick PROPERTIES LABELS "stress" TIMEOUT 60)

flux_foundry_add_probe(flux_foundry_static_stack_stress test_static_stack.cpp)
add_test(NAME static_stack_stress_quick COMMAND flux_foundry_static_stack_stress 2 2 1)
set_tests_properties(static_stack_stress_quick PROPERTIES LABELS "stress" TIMEOUT 60)

# Build representative simple_executor lifecycle probes on every supported architecture.
# They are intentionally not registered with CTest because several are historical hang reproducers.
flux_foundry_add_probe(flux_foundry_simple_run_shutdown_compile_probe simple_run_shutdown_join.cpp)
flux_foundry_add_probe(flux_foundry_simple_fullqueue_compile_probe simple_fullqueue_with_run.cpp)

# Public-header self-containment checks. Building these targets is the test.
flux_foundry_add_probe(flux_foundry_header_back_off_standalone header_back_off_standalone.cpp)
flux_foundry_add_probe(flux_foundry_header_flow_runner_standalone header_flow_runner_standalone.cpp)
if(TARGET flux_foundry_gsource_executor)
    add_executable(flux_foundry_header_gsource_standalone header_gsource_standalone.cpp)
    target_link_libraries(flux_foundry_header_gsource_standalone PRIVATE
        flux_foundry::gsource_executor
        flux_foundry_test_warnings)
endif()

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
    target_link_libraries(flux_foundry_cuda_runtime_demo PRIVATE flux_foundry::flux_foundry)

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
    target_link_libraries(flux_foundry_cuda_image_demo PRIVATE flux_foundry::flux_foundry)

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
