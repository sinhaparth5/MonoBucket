add_executable(monobucket_tests
    env_test.cpp
    config_test.cpp
    asset_store_test.cpp
    blob_store_test.cpp
    codec_test.cpp
    digest_test.cpp
    io_executor_test.cpp
    keyspace_test.cpp
    metadata_store_test.cpp
    storage_engine_test.cpp
)

# The temporary-directory fixture lives beside the tests that use it.
target_include_directories(monobucket_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")

target_link_libraries(monobucket_tests PRIVATE
    monobucket_core
    Catch2::Catch2WithMain)

include(Catch)
catch_discover_tests(monobucket_tests)
