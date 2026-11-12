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

# Deterministic contract regressions; timeout also catches deadlocks.
foreach(contract awaitable_lifecycle allocation_failure simple_executor mutable_callable void_recovery end_move ownership_assignment continuation_emplace)
    flux_foundry_add_probe(flux_foundry_${contract}_contract ${contract}_contract.cpp)
    add_test(NAME ${contract}_contract COMMAND flux_foundry_${contract}_contract)
    set_tests_properties(${contract}_contract PROPERTIES LABELS "smoke;contract" TIMEOUT 20)
endforeach()

foreach(contract awaitable_lifecycle allocation_failure simple_executor mutable_callable void_recovery end_move ownership_assignment continuation_emplace)
    flux_foundry_add_noexc_probe(flux_foundry_${contract}_contract_noexc ${contract}_contract.cpp)
    add_test(NAME ${contract}_contract_noexc COMMAND flux_foundry_${contract}_contract_noexc)
    set_tests_properties(${contract}_contract_noexc PROPERTIES LABELS "smoke;contract;noexc" TIMEOUT 20)
endforeach()

# Terminal forwarding and void recovery must also work with the relaxed policy.
foreach(contract void_recovery end_move)
    flux_foundry_add_probe(flux_foundry_${contract}_contract_noexc_relaxed ${contract}_contract.cpp)
    target_compile_definitions(flux_foundry_${contract}_contract_noexc_relaxed PRIVATE FLUX_FOUNDRY_NO_EXCEPTION_STRICT=0)
    if(MSVC)
        target_compile_definitions(flux_foundry_${contract}_contract_noexc_relaxed PRIVATE _HAS_EXCEPTIONS=0)
        target_compile_options(flux_foundry_${contract}_contract_noexc_relaxed PRIVATE /EHs-c-)
    else()
        target_compile_options(flux_foundry_${contract}_contract_noexc_relaxed PRIVATE -fno-exceptions)
    endif()
    add_test(NAME ${contract}_contract_noexc_relaxed COMMAND flux_foundry_${contract}_contract_noexc_relaxed)
    set_tests_properties(${contract}_contract_noexc_relaxed PROPERTIES LABELS "smoke;contract;noexc" TIMEOUT 20)
endforeach()

if(TARGET flux_foundry_gsource_executor)
    flux_foundry_add_probe(flux_foundry_gsource_executor_contract gsource_executor_contract.cpp)
    target_link_libraries(flux_foundry_gsource_executor_contract PRIVATE flux_foundry::gsource_executor)
    add_test(NAME gsource_executor_contract COMMAND flux_foundry_gsource_executor_contract)
    set_tests_properties(gsource_executor_contract PROPERTIES LABELS "smoke;contract;gsource" TIMEOUT 20
        ENVIRONMENT "G_DEBUG=fatal-warnings")
endif()

# Stress
if(TARGET flux_foundry_uv_executor)
    flux_foundry_add_probe(flux_foundry_uv_executor_contract uv_executor_contract.cpp)
    target_link_libraries(flux_foundry_uv_executor_contract PRIVATE flux_foundry::uv_executor)
    add_test(NAME uv_executor_contract COMMAND flux_foundry_uv_executor_contract)
    set_tests_properties(uv_executor_contract PROPERTIES LABELS "smoke;contract;uv" TIMEOUT 30)
    flux_foundry_add_noexc_probe(flux_foundry_uv_executor_contract_noexc uv_executor_contract.cpp)
    target_link_libraries(flux_foundry_uv_executor_contract_noexc PRIVATE flux_foundry::uv_executor)
    add_test(NAME uv_executor_contract_noexc COMMAND flux_foundry_uv_executor_contract_noexc)
    set_tests_properties(uv_executor_contract_noexc PROPERTIES LABELS "smoke;contract;uv;noexc" TIMEOUT 30)
    add_executable(flux_foundry_header_uv_standalone header_uv_standalone.cpp)
    target_link_libraries(flux_foundry_header_uv_standalone PRIVATE
        flux_foundry::uv_executor flux_foundry_test_warnings)
endif()

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
option(FLUX_FOUNDRY_PERF_CONTROLLED_LAYOUT
    "Align flow performance probes for layout diagnosis (also test the default layout)" OFF)
flux_foundry_add_probe(flux_foundry_flow_perf flow_perf.cpp)
if(FLUX_FOUNDRY_PERF_CONTROLLED_LAYOUT)
    target_compile_definitions(flux_foundry_flow_perf PRIVATE FLUX_FOUNDRY_PERF_CONTROLLED_LAYOUT=1)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang" AND NOT MSVC)
        target_compile_options(flux_foundry_flow_perf PRIVATE -falign-functions=64)
    endif()
endif()
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
        CUDA_EXTENSIONS OFF)

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
        CUDA_EXTENSIONS OFF)

    add_test(NAME cuda_image_demo COMMAND flux_foundry_cuda_image_demo)
    set_tests_properties(cuda_image_demo PROPERTIES
        LABELS "demo;extension;cuda"
        ENVIRONMENT "FLUX_FOUNDRY_CUDA_DEMO_NO_OPEN=1")
endif()

find_path(PORTAL_INCLUDE_DIR portal/async.h HINTS "${PROJECT_SOURCE_DIR}/../portal")
if(PORTAL_INCLUDE_DIR)
    find_package(Threads REQUIRED)
    flux_foundry_add_probe(flux_foundry_typed_async_demo typed_async_demo.cpp)
    target_include_directories(flux_foundry_typed_async_demo PRIVATE "${PORTAL_INCLUDE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(flux_foundry_typed_async_demo PRIVATE Threads::Threads)
    add_test(NAME typed_async_demo COMMAND flux_foundry_typed_async_demo)
    set_tests_properties(typed_async_demo PROPERTIES LABELS "smoke;demo;typed" TIMEOUT 20)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        foreach(strict 0 1)
            flux_foundry_add_probe(flux_foundry_typed_async_demo_noexc_${strict} typed_async_demo.cpp)
            target_include_directories(flux_foundry_typed_async_demo_noexc_${strict} PRIVATE "${PORTAL_INCLUDE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}")
            target_link_libraries(flux_foundry_typed_async_demo_noexc_${strict} PRIVATE Threads::Threads)
            target_compile_options(flux_foundry_typed_async_demo_noexc_${strict} PRIVATE -fno-exceptions)
            target_compile_definitions(flux_foundry_typed_async_demo_noexc_${strict} PRIVATE
                PORTAL_NO_EXCEPTION_STRICT=${strict} FLUX_FOUNDRY_NO_EXCEPTION_STRICT=${strict})
            add_test(NAME typed_async_demo_noexc_${strict} COMMAND flux_foundry_typed_async_demo_noexc_${strict})
            set_tests_properties(typed_async_demo_noexc_${strict} PROPERTIES LABELS "smoke;demo;typed;noexc" TIMEOUT 20)
        endforeach()
    endif()
endif()
if(TARGET flux_foundry_cuda_runtime_demo AND PORTAL_INCLUDE_DIR)
    find_package(Threads REQUIRED)
    find_package(CUDAToolkit REQUIRED)
    foreach(variant default erased noexc_0 noexc_1)
        if(variant MATCHES "^noexc_" AND NOT CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
            continue()
        endif()
        set(demo_target flux_foundry_cuda_typed_demo_${variant})
        if(variant STREQUAL "default")
            set(demo_target flux_foundry_cuda_typed_demo)
        endif()
        add_executable(${demo_target} cuda_typed_demo.cpp cuda_typed_device.cu)
        target_include_directories(${demo_target} PRIVATE "${PORTAL_INCLUDE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}")
        target_link_libraries(${demo_target} PRIVATE flux_foundry::flux_foundry Threads::Threads CUDA::cudart)
        set_target_properties(${demo_target} PROPERTIES CUDA_STANDARD 14 CUDA_STANDARD_REQUIRED ON CUDA_RUNTIME_LIBRARY Shared)
        if(variant STREQUAL "erased")
            target_compile_definitions(${demo_target} PRIVATE FF_TYPED_ERASED_DEMO=1)
        elseif(variant MATCHES "noexc_([01])")
            target_compile_definitions(${demo_target} PRIVATE
                PORTAL_NO_EXCEPTION_STRICT=${CMAKE_MATCH_1} FLUX_FOUNDRY_NO_EXCEPTION_STRICT=${CMAKE_MATCH_1})
            target_compile_options(${demo_target} PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions>)
            if(CMAKE_CUDA_COMPILER_ID STREQUAL "NVIDIA")
                target_compile_options(${demo_target} PRIVATE $<$<COMPILE_LANGUAGE:CUDA>:--no-exceptions>)
            else()
                target_compile_options(${demo_target} PRIVATE $<$<COMPILE_LANGUAGE:CUDA>:-fno-exceptions>)
            endif()
        endif()
        add_test(NAME cuda_typed_${variant} COMMAND ${demo_target})
        add_test(NAME cuda_typed_record_failure_${variant} COMMAND ${demo_target} --fail-record)
        set_tests_properties(cuda_typed_${variant} cuda_typed_record_failure_${variant}
            PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 60 LABELS "demo;extension;cuda")
    endforeach()
endif()

add_executable(flux_foundry_cuda_completion_condition cuda_completion_condition.cpp)
add_test(NAME cuda_completion_condition COMMAND flux_foundry_cuda_completion_condition)
set_tests_properties(cuda_completion_condition PROPERTIES LABELS "smoke;extension")

# Builders retain noexcept: reject throwing storage, not just throwing calls.
flux_foundry_add_probe(flux_foundry_builder_storage_contract builder_storage_contract.cpp)
set_target_properties(flux_foundry_builder_storage_contract
    PROPERTIES CXX_STANDARD 14 CXX_STANDARD_REQUIRED YES CXX_EXTENSIONS NO)
add_test(NAME builder_storage_contract COMMAND flux_foundry_builder_storage_contract)
set_tests_properties(builder_storage_contract PROPERTIES LABELS "smoke;contract")
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang" AND NOT MSVC)
    # C++14 cannot rely on guaranteed elision for node/blueprint returns.
    target_compile_options(flux_foundry_builder_storage_contract PRIVATE -fno-elide-constructors)
    foreach(strict 0 1)
        flux_foundry_add_probe(flux_foundry_builder_storage_contract_noexc_${strict} builder_storage_contract.cpp)
        set_target_properties(flux_foundry_builder_storage_contract_noexc_${strict}
            PROPERTIES CXX_STANDARD 14 CXX_STANDARD_REQUIRED YES CXX_EXTENSIONS NO)
        target_compile_options(flux_foundry_builder_storage_contract_noexc_${strict}
            PRIVATE -fno-exceptions -fno-elide-constructors)
        target_compile_definitions(flux_foundry_builder_storage_contract_noexc_${strict}
            PRIVATE FLUX_FOUNDRY_NO_EXCEPTION_STRICT=${strict})
        add_test(NAME builder_storage_contract_noexc_${strict}
            COMMAND flux_foundry_builder_storage_contract_noexc_${strict})
        set_tests_properties(builder_storage_contract_noexc_${strict}
            PROPERTIES LABELS "smoke;contract;noexc")
    endforeach()
    foreach(mode exceptions noexc_0 noexc_1)
        set(no_exceptions OFF)
        set(strict 0)
        if(mode MATCHES "noexc_([01])")
            set(no_exceptions ON)
            set(strict ${CMAKE_MATCH_1})
        endif()
        add_test(NAME builder_storage_rejection_${mode}
            COMMAND ${CMAKE_COMMAND}
                -DFF_CXX_COMPILER=${CMAKE_CXX_COMPILER}
                -DFF_SOURCE_DIR=${PROJECT_SOURCE_DIR}
                -DFF_STRICT=${strict}
                -DFF_NO_EXCEPTIONS=${no_exceptions}
                -P ${CMAKE_CURRENT_SOURCE_DIR}/builder_storage_rejection.cmake)
        set_tests_properties(builder_storage_rejection_${mode}
            # 81 compiler invocations per mode; allow slower ARM development hosts.
            PROPERTIES LABELS "smoke;contract;compile-fail" TIMEOUT 600)
    endforeach()
endif()

# Constant construction is optional; retain the existing C++14 runtime baseline.
foreach(std 14 17 20)
    if("cxx_std_${std}" IN_LIST CMAKE_CXX_COMPILE_FEATURES)
        foreach(probe constexpr_blueprint_demo const_transform_contract)
            set(target flux_foundry_${probe}_cxx${std})
            flux_foundry_add_probe(${target} ${probe}.cpp)
            flux_foundry_add_noexc_probe(${target}_noexc ${probe}.cpp)
            set_target_properties(${target} ${target}_noexc PROPERTIES
                CXX_STANDARD ${std} CXX_STANDARD_REQUIRED YES CXX_EXTENSIONS NO)
            add_test(NAME ${probe}_cxx${std} COMMAND ${target})
            add_test(NAME ${probe}_cxx${std}_noexc COMMAND ${target}_noexc)
            set_tests_properties(${probe}_cxx${std} PROPERTIES LABELS "smoke;contract" TIMEOUT 20)
            set_tests_properties(${probe}_cxx${std}_noexc PROPERTIES LABELS "smoke;contract;noexc" TIMEOUT 20)
        endforeach()
    endif()
endforeach()

# User checkpoints and realistic pipeline ownership across executor/backend handoffs.
foreach(mode enabled disabled noexc)
    if(mode STREQUAL "noexc")
        flux_foundry_add_noexc_probe(flux_foundry_vaccine_${mode} vaccine_contract.cpp)
        flux_foundry_add_noexc_probe(flux_foundry_pipeline_lifetime_${mode} pipeline_lifetime_contract.cpp)
    else()
        flux_foundry_add_probe(flux_foundry_vaccine_${mode} vaccine_contract.cpp)
        flux_foundry_add_probe(flux_foundry_pipeline_lifetime_${mode} pipeline_lifetime_contract.cpp)
    endif()
    if(mode STREQUAL "disabled")
        set(vaccine_enabled 0)
    else()
        set(vaccine_enabled 1)
    endif()
    foreach(probe vaccine pipeline_lifetime)
        set_target_properties(flux_foundry_${probe}_${mode} PROPERTIES CXX_STANDARD 14 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
        target_compile_definitions(flux_foundry_${probe}_${mode} PRIVATE FLUX_FOUNDRY_ENABLE_VACCINE=${vaccine_enabled})
        add_test(NAME ${probe}_${mode} COMMAND flux_foundry_${probe}_${mode})
        set_tests_properties(${probe}_${mode} PROPERTIES LABELS "smoke;contract;lifecycle" TIMEOUT 30)
        if(mode STREQUAL "noexc")
            set_property(TEST ${probe}_${mode} APPEND PROPERTY LABELS noexc)
        endif()
    endforeach()
    if(NOT mode STREQUAL "disabled")
        foreach(reason false default throw)
            if(mode STREQUAL "noexc" AND reason STREQUAL "throw")
                continue()
            endif()
            if(reason STREQUAL "default")
                set(expected "flux_foundry vaccine: expected failure at selected")
            elseif(reason STREQUAL "throw")
                set(expected "HOOK OK reason=1 contract=expected failure")
            else()
                set(expected "HOOK OK reason=0 contract=expected failure")
            endif()
            add_test(NAME vaccine_failure_${mode}_${reason}
                COMMAND ${CMAKE_COMMAND} -DFF_EXECUTABLE=$<TARGET_FILE:flux_foundry_vaccine_${mode}>
                    -DFF_MODE=${reason} "-DFF_EXPECT=${expected}" -P ${CMAKE_CURRENT_SOURCE_DIR}/vaccine_failure.cmake)
            set_tests_properties(vaccine_failure_${mode}_${reason} PROPERTIES LABELS "smoke;contract" TIMEOUT 20)
        endforeach()
    endif()
endforeach()

if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    add_test(NAME vaccine_disabled_codegen
        COMMAND ${CMAKE_COMMAND} -DFF_CXX_COMPILER=${CMAKE_CXX_COMPILER}
            -DFF_SOURCE_DIR=${PROJECT_SOURCE_DIR} -DFF_BINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}/vaccine-erasure
            -P ${CMAKE_CURRENT_SOURCE_DIR}/vaccine_erasure.cmake)
    set_tests_properties(vaccine_disabled_codegen PROPERTIES LABELS "smoke;contract" TIMEOUT 60)
endif()

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    add_test(NAME vaccine_predicate_rejection
        COMMAND ${CMAKE_COMMAND} -DFF_CXX_COMPILER=${CMAKE_CXX_COMPILER}
            -DFF_SOURCE_DIR=${PROJECT_SOURCE_DIR} -P ${CMAKE_CURRENT_SOURCE_DIR}/vaccine_rejection.cmake)
    set_tests_properties(vaccine_predicate_rejection PROPERTIES LABELS "smoke;contract;compile-fail" TIMEOUT 60)
endif()

find_package(Threads REQUIRED)
foreach(mode enabled disabled noexc)
    if(mode STREQUAL "noexc")
        flux_foundry_add_noexc_probe(flux_foundry_poison_${mode} poison_contract.cpp)
    else()
        flux_foundry_add_probe(flux_foundry_poison_${mode} poison_contract.cpp)
    endif()
    if(mode STREQUAL "disabled")
        set(enabled 0)
    else()
        set(enabled 1)
    endif()
    target_compile_definitions(flux_foundry_poison_${mode} PRIVATE FLUX_FOUNDRY_ENABLE_POISON=${enabled})
    target_link_libraries(flux_foundry_poison_${mode} PRIVATE Threads::Threads)
    set_target_properties(flux_foundry_poison_${mode} PROPERTIES CXX_STANDARD 14 CXX_STANDARD_REQUIRED ON)
    add_test(NAME poison_${mode} COMMAND flux_foundry_poison_${mode})
    set_tests_properties(poison_${mode} PROPERTIES LABELS "smoke;contract;lifecycle")
    if(enabled)
        foreach(case expired duplicate rejected inline-rejected missing late race reuse node destroyed)
            set(reason 0)
            if(case STREQUAL "duplicate" OR case STREQUAL "race")
                set(reason 1)
            elseif(case STREQUAL "rejected")
                set(reason 2)
            elseif(case STREQUAL "inline-rejected")
                set(reason 3)
            elseif(case STREQUAL "missing")
                set(reason 4)
            endif()
            add_test(NAME poison_${mode}_${case} COMMAND ${CMAKE_COMMAND}
                -DFF_EXECUTABLE=$<TARGET_FILE:flux_foundry_poison_${mode}> -DFF_MODE=${case}
                "-DFF_EXPECT=POISON reason=${reason}" -P ${CMAKE_CURRENT_SOURCE_DIR}/vaccine_failure.cmake)
            set_tests_properties(poison_${mode}_${case} PROPERTIES LABELS "smoke;contract;lifecycle")
        endforeach()
    endif()
endforeach()
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    add_test(NAME poison_disabled_codegen COMMAND ${CMAKE_COMMAND}
        -DFF_CXX_COMPILER=${CMAKE_CXX_COMPILER} -DFF_SOURCE_DIR=${PROJECT_SOURCE_DIR}
        -DFF_BINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}/poison-erasure
        -P ${CMAKE_CURRENT_SOURCE_DIR}/poison_erasure.cmake)
    set_tests_properties(poison_disabled_codegen PROPERTIES LABELS "smoke;contract")
endif()
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    add_test(NAME poison_selector_rejection COMMAND ${CMAKE_COMMAND}
        -DFF_CXX_COMPILER=${CMAKE_CXX_COMPILER} -DFF_SOURCE_DIR=${PROJECT_SOURCE_DIR}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/poison_rejection.cmake)
    set_tests_properties(poison_selector_rejection PROPERTIES LABELS "smoke;contract;compile-fail")
endif()
