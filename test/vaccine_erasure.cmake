# Ignore non-runtime symbol names and build IDs; retain all linked code/data.
file(MAKE_DIRECTORY "${FF_BINARY_DIR}")
foreach(markers 0 1)
    execute_process(COMMAND "${FF_CXX_COMPILER}" -std=c++14 -O3 -DNDEBUG
        -DFLUX_FOUNDRY_ENABLE_VACCINE=0 -DFF_WITH_MARKERS=${markers}
        "-I${FF_SOURCE_DIR}" "${FF_SOURCE_DIR}/test/vaccine_erasure_probe.cpp"
        -Wl,--build-id=none -s -o "${FF_BINARY_DIR}/markers-${markers}"
        RESULT_VARIABLE status OUTPUT_VARIABLE out ERROR_VARIABLE err)
    if(NOT status STREQUAL "0")
        message(FATAL_ERROR "erasure probe failed to compile: ${out}\n${err}")
    endif()
endforeach()
file(SHA256 "${FF_BINARY_DIR}/markers-0" plain)
file(SHA256 "${FF_BINARY_DIR}/markers-1" marked)
if(NOT plain STREQUAL marked)
    message(FATAL_ERROR "disabled markers changed the optimized executable; inspect code generation")
endif()
message(STATUS "Disabled vaccine/position executables are byte-identical")
