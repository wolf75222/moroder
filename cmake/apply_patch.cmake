# Applies a patch once (cmake -P, from the dependency's source directory):
# skipped when it is already in. Portable replacement for `sh -c "git apply
# --reverse --check ... || git apply ..."`, which needs a POSIX shell.
execute_process(COMMAND git apply --reverse --check "${PATCH}" RESULT_VARIABLE already OUTPUT_QUIET ERROR_QUIET)
if(already EQUAL 0)
    return()
endif()
execute_process(COMMAND git apply "${PATCH}" RESULT_VARIABLE failed)
if(NOT failed EQUAL 0)
    message(FATAL_ERROR "could not apply ${PATCH}")
endif()
