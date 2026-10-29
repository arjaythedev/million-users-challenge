cmake_minimum_required(VERSION 3.31)

# Dependencies: CPM.cmake (cmake/CPM.cmake) fetches the pinned ones and they are built from source and linked
# statically. Nothing but the compiler, CMake, git, liburing and glibc is taken from the system.

project(million_users_epoll_eastl LANGUAGES CXX)

option(FEED_BUILD_TESTS "Build and register the unit tests (fetches GoogleTest)" ON)

# CMake 4 refuses the ancient minimum versions some dependencies declare.
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Each dependency is pinned to a release tag. The sources are cached in build/_cpm, shared by all build directories.
set(CPM_SOURCE_CACHE "${CMAKE_SOURCE_DIR}/build/_cpm" CACHE PATH "")
include(cmake/CPM.cmake)

CPMAddPackage(NAME EASTL GITHUB_REPOSITORY electronicarts/EASTL GIT_TAG 3.27.01
        OPTIONS "EASTL_BUILD_TESTS OFF" "EASTL_BUILD_BENCHMARK OFF")
CPMAddPackage(NAME libdeflate GITHUB_REPOSITORY ebiggers/libdeflate GIT_TAG v1.26
        OPTIONS "LIBDEFLATE_BUILD_STATIC_LIB ON" "LIBDEFLATE_BUILD_SHARED_LIB OFF" "LIBDEFLATE_BUILD_GZIP OFF")
set(FEED_LIBDEFLATE libdeflate::libdeflate_static)
# StringZilla is header-only
CPMAddPackage(NAME StringZilla GITHUB_REPOSITORY ashvardanian/StringZilla GIT_TAG v5.3.2 DOWNLOAD_ONLY YES)
set(STRINGZILLA_INCLUDE_DIR "${StringZilla_SOURCE_DIR}/include")
# HMAC-SHA256 for the JWT check is all the crypto the server needs, so Mbed TLS' crypto library, not OpenSSL. 3.6 (the
# long-term-support line) has a stateless one-shot HMAC; 4.x goes through PSA's process-wide key store, which needs a
# lock configured to be used from several event loops.
CPMAddPackage(NAME mbedtls
        URL https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-3.6.7/mbedtls-3.6.7.tar.bz2
        URL_HASH SHA256=a7e8bcbec0e6f761b4af24f25677626b35f762f68eef79c08677a363212d11f6
        OPTIONS "ENABLE_PROGRAMS OFF" "ENABLE_TESTING OFF" "GEN_FILES OFF" "USE_SHARED_MBEDTLS_LIBRARY OFF"
        "USE_STATIC_MBEDTLS_LIBRARY ON" "MBEDTLS_FATAL_WARNINGS OFF")

find_package(Threads REQUIRED)

# liburing: the distribution's (liburing-dev), linked as its static archive.
find_path(FEED_URING_INCLUDE liburing.h REQUIRED)
find_library(FEED_URING_LIBRARY NAMES liburing.a REQUIRED)
add_library(feed_uring STATIC IMPORTED GLOBAL)
set_target_properties(feed_uring PROPERTIES IMPORTED_LOCATION "${FEED_URING_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${FEED_URING_INCLUDE}")

if(FEED_BUILD_TESTS)
    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
    CPMAddPackage(NAME googletest GITHUB_REPOSITORY google/googletest GIT_TAG v1.18.0)
endif()
# Profile-guided optimisation (gcc or clang). build.sh builds with "generate", runs pgo_train, then rebuilds with "use". The
# profile is made on the build machine and kept in the build directory; nothing is committed.
set(FEED_PGO "off" CACHE STRING "off, generate (instrumented build plus the pgo_train target) or use (needs a profile)")
set_property(CACHE FEED_PGO PROPERTY STRINGS off generate use)
if(NOT FEED_PGO STREQUAL "off")
    set(FEED_PGO_DIR "${CMAKE_BINARY_DIR}/pgo")
    if(FEED_PGO STREQUAL "generate")
        add_compile_options(-fprofile-generate=${FEED_PGO_DIR})
        add_link_options(-fprofile-generate=${FEED_PGO_DIR})
    # Applied to the server's own targets only: ThinLTO refuses to link the unit tests (no profile) with core (profile).
    elseif(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
        # build.sh merges the raw profiles with llvm-profdata into this one file
        set(FEED_PGO_USE -fprofile-use=${FEED_PGO_DIR}/default.profdata -Wno-profile-instr-missing
                -Wno-backend-plugin) # the trainer's own main() shares a name with ours: its profile is not meant for it
    else()
        # partial training: code the trainer does not reach (sockets, io_uring, the database) is still optimised for speed
        set(FEED_PGO_USE -fprofile-use=${FEED_PGO_DIR} -fprofile-correction -fprofile-partial-training -Wno-missing-profile)
    endif()
endif()

add_library(core STATIC)
target_sources(core
        PRIVATE
        src/api.cpp
        src/app.cpp
        src/eastl_hooks.cpp
        src/http.cpp
        src/json.cpp
        src/jwt.cpp
        src/pg_pool.cpp
        src/pg_wire.cpp
        src/response.cpp
        src/routes.cpp
        src/server.cpp
)
target_include_directories(core PUBLIC src ${STRINGZILLA_INCLUDE_DIR})
target_link_libraries(core PUBLIC EASTL feed_uring mbedcrypto ${FEED_LIBDEFLATE} Threads::Threads)
target_compile_options(core PRIVATE -Wall -Wextra ${FEED_PGO_USE})
# The C++ runtime is linked statically too (everything else already is); glibc stays dynamic.
target_link_options(core PUBLIC -static-libstdc++ -static-libgcc)

add_executable(server src/main.cpp)
target_link_libraries(server PRIVATE core)
target_compile_options(server PRIVATE ${FEED_PGO_USE})

if(FEED_PGO STREQUAL "generate")
    add_executable(pgo_train pgo/train.cpp)
    target_link_libraries(pgo_train PRIVATE core)
endif()

if(FEED_BUILD_TESTS)
    enable_testing()
    include(GoogleTest)
    add_executable(unit_tests)
    target_sources(unit_tests
            PRIVATE
            tests/api_test.cpp
            tests/app_test.cpp
            tests/http_test.cpp
            tests/json_test.cpp
            tests/jwt_test.cpp
            tests/pg_wire_test.cpp
            tests/poller_test.cpp
            tests/response_test.cpp
            tests/routes_test.cpp
    )
    target_link_libraries(unit_tests PRIVATE core GTest::gmock GTest::gtest_main)
    gtest_discover_tests(unit_tests)  # one ctest case per TEST, not one for the whole binary
endif()
