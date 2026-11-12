foreach(case 1 2 3)
    execute_process(COMMAND "${FF_CXX_COMPILER}" -std=c++14 -fsyntax-only
        -DFLUX_FOUNDRY_ENABLE_POISON=1 -DFF_REJECT_CASE=${case} "-I${FF_SOURCE_DIR}"
        "${FF_SOURCE_DIR}/test/poison_reject.cpp" RESULT_VARIABLE status OUTPUT_QUIET ERROR_QUIET)
    if(status STREQUAL "0")
        message(FATAL_ERROR "invalid poison selector ${case} compiled")
    endif()
endforeach()
