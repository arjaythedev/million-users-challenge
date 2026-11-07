cmake_minimum_required(VERSION 3.25)

project(HttpServer LANGUAGES C CXX)

# ============================================================
# Configuration
# ============================================================

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE
      Release
      CACHE STRING "" FORCE)
endif()

option(HTTPSERVER_NATIVE_ARCH "Optimize for the build CPU" ON)
option(HTTPSERVER_FULL_STATIC "Fully static executable" OFF)

# ============================================================
# Compiler detection
# ============================================================

if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
  set(HTTPSERVER_GCC TRUE)
elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  set(HTTPSERVER_CLANG TRUE)
endif()

# ============================================================
# Ubuntu 24.04 system dependencies
# ============================================================

find_package(OpenSSL REQUIRED)

find_path(
  PQXX_INCLUDE_DIR
  NAMES pqxx/pqxx
  PATHS /usr/include
  NO_DEFAULT_PATH REQUIRED)

find_path(
  LIBPQ_INCLUDE_DIR
  NAMES libpq-fe.h
  PATHS /usr/include/postgresql
  NO_DEFAULT_PATH REQUIRED)

find_library(
  PQXX_LIBRARY
  NAMES pqxx-7.8
  PATHS /usr/lib/${CMAKE_LIBRARY_ARCHITECTURE}
        /lib/${CMAKE_LIBRARY_ARCHITECTURE}
  NO_DEFAULT_PATH REQUIRED)

find_library(
  LIBPQ_LIBRARY
  NAMES pq
  PATHS /usr/lib/${CMAKE_LIBRARY_ARCHITECTURE}
        /lib/${CMAKE_LIBRARY_ARCHITECTURE}
  NO_DEFAULT_PATH REQUIRED)

find_library(
  LIBURING_LIBRARY
  NAMES uring
  PATHS /usr/lib/${CMAKE_LIBRARY_ARCHITECTURE}
        /lib/${CMAKE_LIBRARY_ARCHITECTURE}
  NO_DEFAULT_PATH REQUIRED)

message(STATUS "Ubuntu system dependencies:")
message(STATUS "  libpqxx: ${PQXX_LIBRARY}")
message(STATUS "  libpq:   ${LIBPQ_LIBRARY}")
message(STATUS "  liburing: ${LIBURING_LIBRARY}")
message(STATUS "  OpenSSL: ${OPENSSL_LIBRARIES}")

# ============================================================
# libpqxx Ubuntu 24.04 ABI compatibility
# ============================================================

# Ubuntu 24.04 provides libpqxx 7.8.x.
#
# Force the legacy source_location feature-detection path before any libpqxx
# header is included.
#
# This compatibility workaround must be applied consistently to every
# translation unit that includes libpqxx headers.

set(PQXX_COMPAT_HEADER "${CMAKE_CURRENT_BINARY_DIR}/pqxx-ubuntu-compat.hpp")

file(
  WRITE "${PQXX_COMPAT_HEADER}"
  [=[
#include <version>
#undef __cpp_lib_source_location
]=])

# ============================================================
# picohttpparser
# ============================================================

add_library(picohttpparser STATIC dependencies/picohttpparser.c)

target_include_directories(picohttpparser
                           PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/dependencies)

# ============================================================
# Native architecture optimization
# ============================================================

if(HTTPSERVER_NATIVE_ARCH AND (HTTPSERVER_GCC OR HTTPSERVER_CLANG))
  # add_compile_options(-march=native -mtune=native)
endif()

# ============================================================
# Optimization flags
# ============================================================

# add_compile_options( -O3 -DNDEBUG -fstrict-aliasing -ffunction-sections
# -fdata-sections -fno-plt -pipe )

if(HTTPSERVER_GCC)
  # add_compile_options( -funroll-loops -fprefetch-loop-arrays
  # -fno-semantic-interposition )
endif()

# ============================================================
# Link-time optimization
# ============================================================

include(CheckIPOSupported)

check_ipo_supported(
  RESULT HTTPSERVER_IPO_SUPPORTED
  OUTPUT HTTPSERVER_IPO_ERROR
  LANGUAGES C CXX)

if(HTTPSERVER_IPO_SUPPORTED)
  message(STATUS "Link-time optimization enabled")
else()
  message(WARNING "IPO unavailable: ${HTTPSERVER_IPO_ERROR}")
endif()

# ============================================================
# Shared GCC exception runtime
# ============================================================

if(HTTPSERVER_GCC)
  add_link_options(-shared-libgcc)
endif()

# ============================================================
# Static executable (optional)
# ============================================================

if(HTTPSERVER_FULL_STATIC)
  add_link_options(-static)
endif()

# ============================================================
# Linker optimization
# ============================================================

add_link_options(-Wl,--gc-sections -Wl,--as-needed -Wl,-O1)

# ============================================================
# HttpServer library
# ============================================================

add_library(HttpServer STATIC src/Socket.cpp src/Http.cpp src/Json.cpp
                              src/HttpServer.cpp src/DBReader.cpp)

set_target_properties(
  HttpServer
  PROPERTIES LINKER_LANGUAGE CXX
             POSITION_INDEPENDENT_CODE OFF
             INTERPROCEDURAL_OPTIMIZATION ${HTTPSERVER_IPO_SUPPORTED})

target_compile_features(HttpServer PUBLIC cxx_std_23)

target_include_directories(
  HttpServer
  PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}
  PRIVATE ${PQXX_INCLUDE_DIR} ${LIBPQ_INCLUDE_DIR})

# Apply the compatibility header to HttpServer and consumers.
target_compile_options(HttpServer PUBLIC "-include${PQXX_COMPAT_HEADER}")

# Explicit Ubuntu library paths. Keep libpqxx before libpq.
target_link_libraries(
  HttpServer PRIVATE picohttpparser ${PQXX_LIBRARY} ${LIBPQ_LIBRARY}
                     ${LIBURING_LIBRARY} OpenSSL::SSL OpenSSL::Crypto)

# ============================================================
# Production executable
# ============================================================

add_executable(http_server src/main.cpp)

set_target_properties(
  http_server
  PROPERTIES OUTPUT_NAME "http_server"
             RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
             INTERPROCEDURAL_OPTIMIZATION ${HTTPSERVER_IPO_SUPPORTED})

target_compile_features(http_server PRIVATE cxx_std_23)

target_link_libraries(http_server PRIVATE HttpServer)

# ============================================================
# Configuration summary
# ============================================================

message(STATUS "========================================")
message(STATUS "HttpServer configuration")
message(STATUS "========================================")
message(STATUS "Build type:       ${CMAKE_BUILD_TYPE}")
message(STATUS "Compiler:         ${CMAKE_CXX_COMPILER_ID}")
message(STATUS "C++ standard:     ${CMAKE_CXX_STANDARD}")
message(STATUS "Native arch:      ${HTTPSERVER_NATIVE_ARCH}")
message(STATUS "LTO:              ${HTTPSERVER_IPO_SUPPORTED}")
message(STATUS "Full static:      ${HTTPSERVER_FULL_STATIC}")
message(STATUS "========================================")
