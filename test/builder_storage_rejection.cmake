# GNU/Clang compile-fail coverage. A failure counts only when the intended
# construction-contract diagnostic is present, not just any compiler error.
foreach(kind RANGE 0 2)
    foreach(case RANGE 1 27)
        if(FF_NO_EXCEPTIONS AND case EQUAL 5)
            continue() # catch_exception does not exist without exceptions.
        endif()
        set(options -std=c++14 -fsyntax-only -I${FF_SOURCE_DIR}
            -DFLUX_FOUNDRY_NO_EXCEPTION_STRICT=${FF_STRICT}
            -DFF_STORAGE_CASE=${case} -DFF_STORAGE_KIND=${kind})
        if(FF_NO_EXCEPTIONS)
            list(APPEND options -fno-exceptions)
        endif()
        execute_process(
            COMMAND "${FF_CXX_COMPILER}" ${options}
                "${FF_SOURCE_DIR}/test/builder_storage_reject.cpp"
            RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE errors)
        if(kind EQUAL 1)
            set(expected "flow builder storage must be nothrow move-constructible")
        else()
            set(expected "flow builder storage must be nothrow-constructible from each supplied argument")
        endif()
        if(result EQUAL 0 OR NOT errors MATCHES "${expected}")
            message(FATAL_ERROR "Builder storage case ${case}, kind ${kind}: expected rejection missing\n${output}\n${errors}")
        endif()
    endforeach()
endforeach()
message(STATUS "All builder storage copy/move rejections produced the expected diagnostics")
