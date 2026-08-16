# ---------------------------------------------------------------------------
# Third-party dependencies.
#
# Policy: prefer a system/vcpkg package when one is already present, otherwise
# fall back to FetchContent with a pinned tag. This keeps CI and the Docker
# build hermetic while letting local developers reuse an installed toolchain.
# ---------------------------------------------------------------------------

include(FetchContent)

find_package(Threads REQUIRED)
find_package(OpenSSL 3.0 REQUIRED)   # SigV4 HMAC-SHA256, TLS, content digests
find_package(ZLIB REQUIRED)          # gzip responses for the embedded dashboard

# Drogon (and its transitive deps) still declare cmake_minimum_required(3.5),
# which CMake 4.x refuses outright. Lowering the floor for subprojects keeps
# them configurable until upstream bumps it.
if(CMAKE_VERSION VERSION_GREATER_EQUAL 4.0)
    set(CMAKE_POLICY_VERSION_MINIMUM 3.5 CACHE STRING "Compat floor for vendored deps")
endif()

# --- nlohmann_json ---------------------------------------------------------
find_package(nlohmann_json 3.11 QUIET)
if(NOT nlohmann_json_FOUND)
    FetchContent_Declare(nlohmann_json
        URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
        URL_HASH SHA256=d6c65aca6b1ed68e7a182f4757257b107ae403032760ed6ef121c9d55e81757d
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
    set(JSON_BuildTests OFF CACHE INTERNAL "")
    set(JSON_Install OFF CACHE INTERNAL "")
    FetchContent_MakeAvailable(nlohmann_json)
endif()

# --- Drogon (HTTP server + event loop + thread pool) -----------------------
find_package(Drogon 1.9 QUIET)
if(NOT Drogon_FOUND)
    # Drogon does not vendor jsoncpp or libuuid; both must come from the
    # system even when Drogon itself is fetched. Check up front so the failure
    # is one actionable message instead of a FindJsoncpp stack trace.
    find_path(MONOBUCKET_JSONCPP_INCLUDE_DIR json/json.h PATH_SUFFIXES jsoncpp)
    find_path(MONOBUCKET_UUID_INCLUDE_DIR uuid/uuid.h)

    set(_missing "")
    if(NOT MONOBUCKET_JSONCPP_INCLUDE_DIR)
        list(APPEND _missing "jsoncpp")
    endif()
    if(NOT MONOBUCKET_UUID_INCLUDE_DIR AND NOT WIN32 AND NOT APPLE)
        list(APPEND _missing "libuuid")
    endif()

    if(_missing)
        list(JOIN _missing ", " _missing_list)
        message(FATAL_ERROR
            "Drogon needs system development headers that are not installed: ${_missing_list}\n"
            "  Debian/Ubuntu : sudo apt install -y libjsoncpp-dev uuid-dev libssl-dev "
            "zlib1g-dev libbrotli-dev libc-ares-dev\n"
            "  Alpine        : apk add jsoncpp-dev util-linux-dev openssl-dev zlib-dev "
            "brotli-dev c-ares-dev\n"
            "  Fedora/RHEL   : sudo dnf install jsoncpp-devel libuuid-devel openssl-devel "
            "zlib-devel brotli-devel c-ares-devel\n"
            "  macOS         : brew install jsoncpp ossp-uuid openssl brotli c-ares")
    endif()

    set(BUILD_CTL          OFF CACHE INTERNAL "")
    set(BUILD_EXAMPLES     OFF CACHE INTERNAL "")
    set(BUILD_ORM          OFF CACHE INTERNAL "")
    set(BUILD_DROGON_SHARED OFF CACHE INTERNAL "")
    set(BUILD_TESTING      OFF CACHE INTERNAL "")
    set(BUILD_DOC          OFF CACHE INTERNAL "")
    set(BUILD_BROTLI       ON  CACHE INTERNAL "")
    set(BUILD_YAML_CONFIG  OFF CACHE INTERNAL "")
    FetchContent_Declare(drogon
        GIT_REPOSITORY https://github.com/drogonframework/drogon.git
        GIT_TAG        v1.9.9
        GIT_SHALLOW    TRUE
        GIT_PROGRESS   TRUE)
    # Drogon and Trantor still declare a pre-3.10 policy floor, which CMake
    # reports as a deprecation warning we can do nothing about. Silence it for
    # the vendored configure only, then restore the default so our own
    # deprecations stay visible.
    set(CMAKE_WARN_DEPRECATED OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(drogon)
    set(CMAKE_WARN_DEPRECATED ON CACHE BOOL "" FORCE)

    # Drogon's headers do not compile warning-clean under our flags (-Wshadow
    # in particular). Treating them as system headers keeps our own warnings
    # visible instead of buried in third-party noise.
    if(TARGET drogon)
        set_target_properties(drogon PROPERTIES SYSTEM ON)
        # Drogon's own sources use its own deprecated enumerators
        # (CT_APPLICATION_X_JAVASCRIPT). Not our warning to fix.
        target_compile_options(drogon PRIVATE $<$<CXX_COMPILER_ID:GNU,Clang>:-Wno-deprecated-declarations>)
    endif()
    if(TARGET trantor)
        set_target_properties(trantor PROPERTIES SYSTEM ON)
        target_compile_options(trantor PRIVATE $<$<CXX_COMPILER_ID:GNU,Clang>:-Wno-deprecated-declarations>)
    endif()
endif()

# --- RocksDB (embedded metadata store) -------------------------------------
#
# Not vendored. RocksDB is a ~40 MB static archive that pulls in gflags,
# snappy, lz4, zstd and bz2; building it from source would dominate both the
# image build time and the toolchain surface. Distributions ship it, so we
# require it the same way we require jsoncpp.
find_package(RocksDB CONFIG QUIET)

if(RocksDB_FOUND)
    # Prefer the shared target. The static one carries an INTERFACE_LINK_LIBRARIES
    # list of compression-library config packages (Snappy::snappy, lz4::lz4,
    # zstd::zstd, gflags) that must then all be findable; the shared library has
    # already resolved them internally.
    if(TARGET RocksDB::rocksdb-shared)
        set(MONOBUCKET_ROCKSDB_TARGET RocksDB::rocksdb-shared)
    elseif(TARGET RocksDB::rocksdb)
        set(MONOBUCKET_ROCKSDB_TARGET RocksDB::rocksdb)
    endif()
endif()

if(NOT MONOBUCKET_ROCKSDB_TARGET)
    # Alpine's rocksdb-dev installs no CMake config package, only the library
    # and headers. Fall back to a plain search before giving up.
    find_path(MONOBUCKET_ROCKSDB_INCLUDE_DIR rocksdb/db.h)
    find_library(MONOBUCKET_ROCKSDB_LIBRARY NAMES rocksdb)

    if(MONOBUCKET_ROCKSDB_INCLUDE_DIR AND MONOBUCKET_ROCKSDB_LIBRARY)
        add_library(MonoBucket::RocksDB UNKNOWN IMPORTED)
        set_target_properties(MonoBucket::RocksDB PROPERTIES
            IMPORTED_LOCATION "${MONOBUCKET_ROCKSDB_LIBRARY}"
            INTERFACE_INCLUDE_DIRECTORIES "${MONOBUCKET_ROCKSDB_INCLUDE_DIR}")
        set(MONOBUCKET_ROCKSDB_TARGET MonoBucket::RocksDB)
    else()
        message(FATAL_ERROR
            "RocksDB was not found. MonoBucket stores object metadata in it.\n"
            "  Debian/Ubuntu : sudo apt install -y librocksdb-dev\n"
            "  Alpine        : apk add rocksdb-dev\n"
            "  Fedora/RHEL   : sudo dnf install rocksdb-devel\n"
            "  macOS         : brew install rocksdb\n"
            "Set -DCMAKE_PREFIX_PATH=<prefix> if it is installed somewhere unusual.")
    endif()
endif()

message(STATUS "RocksDB: ${MONOBUCKET_ROCKSDB_TARGET}")

# --- hiredis (optional Redis cache backend) --------------------------------
if(MONOBUCKET_ENABLE_REDIS)
    find_package(hiredis QUIET)
    if(NOT hiredis_FOUND)
        set(DISABLE_TESTS ON CACHE INTERNAL "")
        set(ENABLE_SSL    OFF CACHE INTERNAL "")
        FetchContent_Declare(hiredis
            GIT_REPOSITORY https://github.com/redis/hiredis.git
            GIT_TAG        v1.2.0
            GIT_SHALLOW    TRUE)
        FetchContent_MakeAvailable(hiredis)

        # An installed hiredis puts its headers under include/hiredis/; the
        # build tree leaves them flat at the source root. Rather than guess,
        # the source asks.
        set(MONOBUCKET_HIREDIS_FLAT_HEADERS ON)
    endif()

    # sds.h uses C flexible array members, which -Wpedantic rejects in C++.
    # Same treatment as Drogon: not our warning to fix, and burying our own
    # under it would be worse.
    if(TARGET hiredis)
        set_target_properties(hiredis PROPERTIES SYSTEM ON)
    endif()
endif()

# --- Catch2 (tests only) ---------------------------------------------------
if(MONOBUCKET_BUILD_TESTS)
    find_package(Catch2 3.5 QUIET)
    if(NOT Catch2_FOUND)
        FetchContent_Declare(Catch2
            GIT_REPOSITORY https://github.com/catchorg/Catch2.git
            GIT_TAG        v3.8.1
            GIT_SHALLOW    TRUE)
        FetchContent_MakeAvailable(Catch2)
        list(APPEND CMAKE_MODULE_PATH "${catch2_SOURCE_DIR}/extras")
    endif()
endif()
