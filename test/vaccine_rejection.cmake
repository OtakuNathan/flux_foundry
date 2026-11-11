foreach(case 1 2 3 4)
    set(extra)
    if(case EQUAL 3)
        set(extra -fno-exceptions -DFLUX_FOUNDRY_NO_EXCEPTION_STRICT=1)
    endif()
    set(expected "vaccine predicate must accept const result_t")
    if(case EQUAL 4)
        set(expected "checkpoints do not permit async")
    endif()
    execute_process(COMMAND "${FF_CXX_COMPILER}" -std=c++14 -fsyntax-only
        -DFLUX_FOUNDRY_ENABLE_VACCINE=1 -DFF_REJECT_CASE=${case} ${extra}
        "-I${FF_SOURCE_DIR}" "${FF_SOURCE_DIR}/test/vaccine_reject.cpp"
        RESULT_VARIABLE status OUTPUT_VARIABLE out ERROR_VARIABLE err)
    if(status STREQUAL "0" OR NOT err MATCHES "${expected}")
        message(FATAL_ERROR "invalid predicate case ${case} was not rejected correctly: ${out}\n${err}")
    endif()
endforeach()
