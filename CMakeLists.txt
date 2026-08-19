cmake_minimum_required(VERSION 3.25)

# ---------------------------------------------------------------------------
# MonoBucket — single-binary, S3-compatible object storage server.
#
# Versioning is CalVer: YYYY.0M.MICRO (see CHANGELOG.md). CMake's project()
# only accepts plain integers, so the zero-padded display string is kept
# alongside it and is what ends up in binaries, headers and Docker tags.
# ---------------------------------------------------------------------------
set(MONOBUCKET_VERSION_YEAR  2026)
set(MONOBUCKET_VERSION_MONTH 08)
set(MONOBUCKET_VERSION_MICRO 7)
set(MONOBUCKET_VERSION "${MONOBUCKET_VERSION_YEAR}.${MONOBUCKET_VERSION_MONTH}.${MONOBUCKET_VERSION_MICRO}")

# The padded month is correct in the version *string* but is an invalid octal
# literal in C++ ("08"), so integer uses get an unpadded copy.
string(REGEX REPLACE "^0+(.)" "\\1" MONOBUCKET_VERSION_MONTH_INT "${MONOBUCKET_VERSION_MONTH}")

project(MonoBucket
        VERSION 2026.8.7
        DESCRIPTION "Single-binary S3-compatible object storage with an embedded admin dashboard"
        HOMEPAGE_URL "https://github.com/sinhaparth5/MonoBucket"
        LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------
option(MONOBUCKET_BUILD_TESTS     "Build the Catch2 unit test suite"            ON)
option(MONOBUCKET_EMBED_FRONTEND  "Embed the built SvelteKit dashboard"         OFF)
option(MONOBUCKET_ENABLE_REDIS    "Build the optional Redis cache backend"      OFF)
option(MONOBUCKET_STATIC_LINK     "Link libstdc++/libgcc statically"            OFF)
option(MONOBUCKET_ENABLE_LTO      "Enable link-time optimisation in Release"    OFF)
option(MONOBUCKET_ENABLE_ASAN     "Build with AddressSanitizer + UBSan"         OFF)

set(MONOBUCKET_FRONTEND_DIST "${CMAKE_SOURCE_DIR}/frontend/build"
    CACHE PATH "Directory holding the static SvelteKit build output")

# ---------------------------------------------------------------------------
# Global compile settings
# ---------------------------------------------------------------------------
add_library(monobucket_flags INTERFACE)
target_compile_options(monobucket_flags INTERFACE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall;-Wextra;-Wpedantic;-Wshadow;-Wnon-virtual-dtor>
    $<$<AND:$<CONFIG:Release>,$<CXX_COMPILER_ID:GNU,Clang>>:-O3>
)

if(MONOBUCKET_ENABLE_ASAN)
    target_compile_options(monobucket_flags INTERFACE -fsanitize=address,undefined -fno-omit-frame-pointer -g)
    target_link_options(monobucket_flags INTERFACE -fsanitize=address,undefined)
endif()

# Alpine ships `fortify-headers`, whose _FORTIFY_SOURCE wrappers are
# always_inline. Under LTO, GCC re-compiles callers such as libstdc++'s
# __to_xstring (behind std::to_string) and cannot honour always_inline for a
# body it considers replaceable at link time, so the link dies with
# "inlining failed in call to always_inline 'vsnprintf'". The workarounds in
# circulation disable _FORTIFY_SOURCE; on a network-facing server the
# hardening is worth more than LTO, so we drop LTO on musl instead.
execute_process(COMMAND "${CMAKE_CXX_COMPILER}" -dumpmachine
                OUTPUT_VARIABLE MONOBUCKET_TARGET_TRIPLE
                OUTPUT_STRIP_TRAILING_WHITESPACE
                ERROR_QUIET)
if(MONOBUCKET_TARGET_TRIPLE MATCHES "musl")
    set(MONOBUCKET_LIBC_MUSL ON)
endif()

if(MONOBUCKET_ENABLE_LTO AND MONOBUCKET_LIBC_MUSL)
    message(STATUS "LTO disabled: incompatible with musl's fortify headers (see CMakeLists.txt)")
    set(MONOBUCKET_ENABLE_LTO OFF)
endif()

if(MONOBUCKET_ENABLE_LTO)
    include(CheckIPOSupported)
    check_ipo_supported(RESULT MONOBUCKET_IPO_OK OUTPUT MONOBUCKET_IPO_MSG)
    if(NOT MONOBUCKET_IPO_OK)
        message(WARNING "LTO requested but unavailable: ${MONOBUCKET_IPO_MSG}")
        set(MONOBUCKET_ENABLE_LTO OFF)
    endif()
    # Deliberately not setting CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE: that
    # is inherited by every target including the vendored ones, and Trantor
    # declares a pre-3.9 policy floor so CMake warns that it is ignoring the
    # property there (CMP0069). LTO is applied per-target in backend/ instead,
    # which is also the correct scope — we want it across our own translation
    # units, not forced onto third-party code.
endif()

# ---------------------------------------------------------------------------
# Subprojects
# ---------------------------------------------------------------------------
if(MONOBUCKET_BUILD_TESTS)
    # Must be called from the top level for `ctest` to see the suite; the tests
    # themselves are added from backend/CMakeLists.txt so they inherit the
    # module path Catch2 installs.
    enable_testing()
endif()

add_subdirectory(common)
add_subdirectory(backend)

# ---------------------------------------------------------------------------
message(STATUS "----------------------------------------------------------")
message(STATUS "MonoBucket ${MONOBUCKET_VERSION} (${CMAKE_BUILD_TYPE})")
message(STATUS "  compiler        : ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "  tests           : ${MONOBUCKET_BUILD_TESTS}")
message(STATUS "  embed frontend  : ${MONOBUCKET_EMBED_FRONTEND}")
message(STATUS "  redis backend   : ${MONOBUCKET_ENABLE_REDIS}")
message(STATUS "  sanitizers      : ${MONOBUCKET_ENABLE_ASAN}")
message(STATUS "  lto             : ${MONOBUCKET_ENABLE_LTO}")
message(STATUS "----------------------------------------------------------")
