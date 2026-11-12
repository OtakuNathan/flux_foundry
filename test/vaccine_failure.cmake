execute_process(COMMAND "${FF_EXECUTABLE}" "${FF_MODE}"
    RESULT_VARIABLE status OUTPUT_VARIABLE out ERROR_VARIABLE err TIMEOUT 10)
if(NOT status STREQUAL "86")
    message(FATAL_ERROR "contract checkpoint did not terminate promptly: ${status}\n${out}\n${err}")
endif()
if(err MATCHES "UNREACHABLE|ERROR: AddressSanitizer|runtime error:|WARNING: ThreadSanitizer" OR NOT err MATCHES "${FF_EXPECT}")
    message(FATAL_ERROR "missing/wrong contract diagnostic: ${status}\n${out}\n${err}")
endif()
