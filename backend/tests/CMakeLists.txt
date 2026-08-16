add_executable(monobucket_tests
    env_test.cpp
    config_test.cpp
    asset_store_test.cpp
)

target_link_libraries(monobucket_tests PRIVATE
    monobucket_core
    Catch2::Catch2WithMain)

include(Catch)
catch_discover_tests(monobucket_tests)
