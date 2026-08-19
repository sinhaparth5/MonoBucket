list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(Dependencies)

# ---------------------------------------------------------------------------
# monobucket_core — everything except main(), so the test binary can link it.
# ---------------------------------------------------------------------------
set(MONOBUCKET_CORE_SOURCES
    src/cache/cache_factory.cpp
    src/cache/cache_provider.cpp
    src/cache/fallback_cache.cpp
    src/cache/memory_cache.cpp
    src/cache/redis_url.cpp
    src/core/config.cpp
    src/core/credentials.cpp
    src/core/env.cpp
    src/core/identity.cpp
    src/core/identity_migration.cpp
    src/core/io_executor.cpp
    src/core/lifecycle.cpp
    src/core/logging.cpp
    src/core/password.cpp
    src/s3/base64.cpp
    src/s3/bucket_handlers.cpp
    src/s3/checksum.cpp
    src/s3/chunked.cpp
    src/s3/bucket_policy.cpp
    src/s3/cors.cpp
    src/s3/handlers.cpp
    src/s3/multipart_handlers.cpp
    src/s3/object_handlers.cpp
    src/s3/operation.cpp
    src/s3/request.cpp
    src/s3/response.cpp
    src/s3/router.cpp
    src/s3/s3_error.cpp
    src/s3/sigv4.cpp
    src/s3/uri.cpp
    src/s3/xml.cpp
    src/storage/blob_store.cpp
    src/storage/checksum.cpp
    src/storage/codec.cpp
    src/storage/digest.cpp
    src/storage/durability.cpp
    src/storage/keyspace.cpp
    src/storage/quota.cpp
    src/storage/records.cpp
    src/storage/rocksdb_metadata_store.cpp
    src/storage/storage_engine.cpp
    src/server/console_api.cpp
    src/server/console_session.cpp
    src/server/metrics_history.cpp
    src/server/policy_reconcile.cpp
    src/server/server.cpp
    src/server/system_routes.cpp
)

add_library(monobucket_core STATIC ${MONOBUCKET_CORE_SOURCES})
add_library(MonoBucket::Core ALIAS monobucket_core)

target_include_directories(monobucket_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")

target_link_libraries(monobucket_core
    PUBLIC
        MonoBucket::Common
        monobucket_flags
        nlohmann_json::nlohmann_json
        Threads::Threads
    PRIVATE
        drogon
        OpenSSL::SSL
        OpenSSL::Crypto
        ZLIB::ZLIB
        ${MONOBUCKET_ROCKSDB_TARGET}
)

if(MONOBUCKET_ENABLE_REDIS)
    # The definition is PUBLIC because redis_cache.hpp compiles to nothing
    # without it, so every consumer must agree on whether the class exists.
    target_compile_definitions(monobucket_core PUBLIC MONOBUCKET_WITH_REDIS=1)
    target_sources(monobucket_core PRIVATE src/cache/redis_cache.cpp)
    target_link_libraries(monobucket_core PRIVATE hiredis::hiredis)
    if(MONOBUCKET_HIREDIS_FLAT_HEADERS)
        set_source_files_properties(src/cache/redis_cache.cpp PROPERTIES
            COMPILE_DEFINITIONS MONOBUCKET_HIREDIS_FLAT_HEADERS=1)
    endif()
endif()

# ---------------------------------------------------------------------------
# Embedded frontend assets (Phase 5).
#
# When MONOBUCKET_EMBED_FRONTEND is ON the SvelteKit static build is turned
# into a generated C++ translation unit. When it is OFF a stub with an empty
# asset table is generated instead, so the backend always compiles and links
# whether or not anyone has run `npm run build`.
# ---------------------------------------------------------------------------
set(MONOBUCKET_ASSETS_CPP "${CMAKE_CURRENT_BINARY_DIR}/generated/embedded_assets.cpp")

# Re-embed whenever the SvelteKit output changes. CONFIGURE_DEPENDS re-runs the
# glob on each build so newly fingerprinted files are picked up without a manual
# `cmake` re-run.
set(MONOBUCKET_ASSET_FILES "")
if(MONOBUCKET_EMBED_FRONTEND AND EXISTS "${MONOBUCKET_FRONTEND_DIST}")
    file(GLOB_RECURSE MONOBUCKET_ASSET_FILES CONFIGURE_DEPENDS "${MONOBUCKET_FRONTEND_DIST}/*")
elseif(MONOBUCKET_EMBED_FRONTEND)
    message(FATAL_ERROR
        "MONOBUCKET_EMBED_FRONTEND=ON but '${MONOBUCKET_FRONTEND_DIST}' does not exist.\n"
        "Build the dashboard first: cd frontend && npm ci && npm run build")
endif()

add_custom_command(
    OUTPUT "${MONOBUCKET_ASSETS_CPP}"
    COMMAND ${CMAKE_COMMAND}
            -DASSET_DIR=${MONOBUCKET_FRONTEND_DIST}
            -DOUTPUT_FILE=${MONOBUCKET_ASSETS_CPP}
            -DEMBED_ENABLED=${MONOBUCKET_EMBED_FRONTEND}
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/EmbedFrontend.cmake"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/cmake/EmbedFrontend.cmake" ${MONOBUCKET_ASSET_FILES}
    COMMENT "Generating embedded dashboard assets (embed=${MONOBUCKET_EMBED_FRONTEND})"
    VERBATIM)

add_library(monobucket_assets STATIC "${MONOBUCKET_ASSETS_CPP}" src/server/asset_store.cpp)
target_include_directories(monobucket_assets PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(monobucket_assets PUBLIC MonoBucket::Common monobucket_flags)

target_link_libraries(monobucket_core PUBLIC monobucket_assets)

# ---------------------------------------------------------------------------
# The single binary.
# ---------------------------------------------------------------------------
add_executable(monobucket src/main.cpp)
target_link_libraries(monobucket PRIVATE monobucket_core drogon)

if(MONOBUCKET_STATIC_LINK)
    target_link_options(monobucket PRIVATE -static-libstdc++ -static-libgcc)
endif()

if(MONOBUCKET_ENABLE_LTO)
    # Scoped to our own targets; see the note in the top-level CMakeLists.txt.
    set_property(TARGET monobucket monobucket_core monobucket_assets
                 PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)
endif()

if(CMAKE_BUILD_TYPE STREQUAL "Release")
    # Symbols are stripped in the Docker runtime stage; keep local builds
    # debuggable but drop dead sections either way.
    target_link_options(monobucket PRIVATE
        $<$<CXX_COMPILER_ID:GNU,Clang>:-Wl,--gc-sections>)
    target_compile_options(monobucket PRIVATE
        $<$<CXX_COMPILER_ID:GNU,Clang>:-ffunction-sections;-fdata-sections>)
endif()

install(TARGETS monobucket RUNTIME DESTINATION bin)

if(MONOBUCKET_BUILD_TESTS)
    add_subdirectory(tests)
endif()
