# ---------------------------------------------------------------------------
# Idempotently apply a patch to a fetched dependency.
#
# Run as a PATCH_COMMAND via `cmake -DPATCH_FILE=... -P ApplyPatch.cmake`. A
# plain `git apply` is not usable directly there: FetchContent re-runs the
# patch step on re-configures and over an already-patched tree, and CMake
# launches the command without a shell, so `||` cannot express the fallback.
#
# The order matters. A reverse --check succeeding means the patch is already
# in, so there is nothing to do. Only when it has *not* been applied do we try
# to apply it, and a failure there is fatal — a patch that no longer applies
# means the upstream source moved, and silently building an unpatched
# dependency is exactly the outcome this script exists to prevent.
# ---------------------------------------------------------------------------

if(NOT DEFINED PATCH_FILE)
    message(FATAL_ERROR "ApplyPatch.cmake requires -DPATCH_FILE=<path>")
endif()

if(NOT EXISTS "${PATCH_FILE}")
    message(FATAL_ERROR "Patch file does not exist: ${PATCH_FILE}")
endif()

find_program(GIT_EXECUTABLE git REQUIRED)

execute_process(
    COMMAND "${GIT_EXECUTABLE}" apply --reverse --check "${PATCH_FILE}"
    RESULT_VARIABLE already_applied
    OUTPUT_QUIET ERROR_QUIET)

if(already_applied EQUAL 0)
    message(STATUS "Patch already applied: ${PATCH_FILE}")
    return()
endif()

execute_process(
    COMMAND "${GIT_EXECUTABLE}" apply "${PATCH_FILE}"
    RESULT_VARIABLE applied
    ERROR_VARIABLE apply_error)

if(NOT applied EQUAL 0)
    message(FATAL_ERROR
        "Failed to apply ${PATCH_FILE}:\n${apply_error}\n"
        "The upstream source has probably moved. Check whether the defect the "
        "patch addresses is still present, then refresh or remove the patch.")
endif()

message(STATUS "Applied patch: ${PATCH_FILE}")
