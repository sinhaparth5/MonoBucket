add_executable(monobucket_tests
    env_test.cpp
    config_test.cpp
    asset_store_test.cpp
    blob_store_test.cpp
    cache_factory_test.cpp
    codec_test.cpp
    digest_test.cpp
    fallback_cache_test.cpp
    io_executor_test.cpp
    keyspace_test.cpp
    memory_cache_test.cpp
    metadata_store_test.cpp
    redis_cache_test.cpp
    redis_url_test.cpp
    s3_chunked_test.cpp
    s3_cors_test.cpp
    s3_error_test.cpp
    s3_operation_test.cpp
    s3_request_test.cpp
    s3_uri_test.cpp
    s3_xml_test.cpp
    sigv4_test.cpp
    storage_engine_test.cpp
)

# The temporary-directory fixture lives beside the tests that use it.
target_include_directories(monobucket_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")

target_link_libraries(monobucket_tests PRIVATE
    monobucket_core
    Catch2::Catch2WithMain)

include(Catch)
catch_discover_tests(monobucket_tests)
