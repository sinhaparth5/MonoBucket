# Header-only definitions shared between the storage engine, the S3 protocol
# layer and (via generated JSON) the SvelteKit dashboard.

configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/include/monobucket/version.hpp.in"
    "${CMAKE_CURRENT_BINARY_DIR}/generated/monobucket/version.hpp"
    @ONLY)

add_library(monobucket_common INTERFACE)
add_library(MonoBucket::Common ALIAS monobucket_common)

target_include_directories(monobucket_common INTERFACE
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${CMAKE_CURRENT_BINARY_DIR}/generated")

target_link_libraries(monobucket_common INTERFACE monobucket_flags)
