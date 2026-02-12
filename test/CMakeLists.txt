set(FLUX_FOUNDRY_TEST_OUTPUT_DIR "${CMAKE_SOURCE_DIR}/test/bin")

if(CMAKE_CONFIGURATION_TYPES)
    foreach(cfg IN LISTS CMAKE_CONFIGURATION_TYPES)
        string(TOUPPER "${cfg}" cfg_upper)
        set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_${cfg_upper} "${FLUX_FOUNDRY_TEST_OUTPUT_DIR}")
    endforeach()
else()
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${FLUX_FOUNDRY_TEST_OUTPUT_DIR}")
endif()

function(lfnds_add_probe target source)
    add_executable(${target} ${source})
    target_include_directories(${target} PRIVATE "${CMAKE_SOURCE_DIR}")
endfunction()

function(lfnds_add_noexc_probe target source)
    lfnds_add_probe(${target} ${source})
    target_compile_definitions(${target} PRIVATE FLUEX_FOUNDRY_NO_EXCEPTION_STRICT=1)
    if(MSVC)
        target_compile_definitions(${target} PRIVATE _HAS_EXCEPTIONS=0)
        target_compile_options(${target} PRIVATE /EHs-c-)
    else()
        target_compile_options(${target} PRIVATE -fno-exceptions)
    endif()
endfunction()

# Smoke
lfnds_add_probe(lfnds_flow_smoke flow_smoke_test.cpp)
add_test(NAME flow_smoke COMMAND lfnds_flow_smoke)
set_tests_properties(flow_smoke PROPERTIES LABELS "smoke")

lfnds_add_probe(lfnds_flow_fullchain_smoke flow_fullchain_smoke.cpp)
add_test(NAME flow_fullchain_smoke COMMAND lfnds_flow_fullchain_smoke)
set_tests_properties(flow_fullchain_smoke PROPERTIES LABELS "smoke")

lfnds_add_noexc_probe(lfnds_flow_fullchain_smoke_noexc flow_fullchain_smoke_noexcept.cpp)
add_test(NAME flow_fullchain_smoke_noexc COMMAND lfnds_flow_fullchain_smoke_noexc)
set_tests_properties(flow_fullchain_smoke_noexc PROPERTIES LABELS "smoke;noexc")

# Stress
lfnds_add_probe(lfnds_flow_state_stress flow_state_stress.cpp)
add_test(NAME flow_state_stress COMMAND lfnds_flow_state_stress)
set_tests_properties(flow_state_stress PROPERTIES LABELS "stress" TIMEOUT 300)

# Perf (runtime benchmark probe)
lfnds_add_probe(lfnds_flow_perf flow_perf.cpp)
add_test(NAME flow_perf COMMAND lfnds_flow_perf)
set_tests_properties(flow_perf PROPERTIES LABELS "perf" TIMEOUT 300)
