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

set(HTTPSERVER_STATIC_PG_PREFIX
    "/opt/httpserver-static/postgresql-18"
    CACHE PATH
          "Prefix containing the custom static PostgreSQL client libraries")

# ============================================================
# Compiler detection
# ============================================================

if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
  set(HTTPSERVER_GCC TRUE)
elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  set(HTTPSERVER_CLANG TRUE)
endif()

# ============================================================
# pkg-config
# ============================================================

if(HTTPSERVER_FULL_STATIC)

  # Always prioritize the custom PostgreSQL installation.
  set(ENV{PKG_CONFIG_PATH}
      "${HTTPSERVER_STATIC_PG_PREFIX}/lib/pkgconfig:$ENV{PKG_CONFIG_PATH}")

endif()

find_package(PkgConfig REQUIRED)

# ============================================================
# System dependencies
# ============================================================

if(HTTPSERVER_FULL_STATIC)

  pkg_check_modules(LIBPQ REQUIRED libpq)
  pkg_check_modules(LIBURING REQUIRED liburing)
  pkg_check_modules(LIBDEFLATE REQUIRED libdeflate)
  pkg_check_modules(OPENSSL_SSL REQUIRED libssl)
  pkg_check_modules(OPENSSL_CRYPTO REQUIRED libcrypto)

  find_package(Threads REQUIRED)

  # --------------------------------------------------------
  # Verify custom PostgreSQL installation
  # --------------------------------------------------------

  if(NOT LIBPQ_VERSION MATCHES "^18\\.")
    message(FATAL_ERROR "Expected PostgreSQL 18 libpq, found ${LIBPQ_VERSION}")
  endif()

  execute_process(
    COMMAND "${PKG_CONFIG_EXECUTABLE}" --variable=prefix libpq
    OUTPUT_VARIABLE LIBPQ_PREFIX
    OUTPUT_STRIP_TRAILING_WHITESPACE
    RESULT_VARIABLE LIBPQ_PREFIX_RESULT)

  if(NOT LIBPQ_PREFIX_RESULT EQUAL 0)
    message(FATAL_ERROR "Could not determine PostgreSQL libpq prefix")
  endif()

  file(REAL_PATH "${LIBPQ_PREFIX}" LIBPQ_PREFIX_REAL)

  file(REAL_PATH "${HTTPSERVER_STATIC_PG_PREFIX}" EXPECTED_LIBPQ_PREFIX_REAL)

  if(NOT LIBPQ_PREFIX_REAL STREQUAL EXPECTED_LIBPQ_PREFIX_REAL)
    message(
      FATAL_ERROR
        "Incorrect PostgreSQL installation selected.\n"
        "  Selected: ${LIBPQ_PREFIX_REAL}\n"
        "  Expected: ${EXPECTED_LIBPQ_PREFIX_REAL}")
  endif()

  # --------------------------------------------------------
  # Exact PostgreSQL static archives
  #
  # IMPORTANT: libpq.a must use the _shlib frontend variants. The regular
  # libpgcommon.a uses private encoding symbols.
  # --------------------------------------------------------

  set(HTTPSERVER_POSTGRESQL_LIBRARIES
      "${HTTPSERVER_STATIC_PG_PREFIX}/lib/libpq.a"
      "${HTTPSERVER_STATIC_PG_PREFIX}/lib/libpgcommon_shlib.a"
      "${HTTPSERVER_STATIC_PG_PREFIX}/lib/libpgport_shlib.a")

  foreach(archive IN LISTS HTTPSERVER_POSTGRESQL_LIBRARIES)

    if(NOT EXISTS "${archive}")
      message(FATAL_ERROR "Missing PostgreSQL static archive:\n" "  ${archive}")
    endif()

  endforeach()

  # --------------------------------------------------------
  # Verify public encoding symbols in libpgcommon_shlib.a
  # --------------------------------------------------------

  execute_process(
    COMMAND nm -g --defined-only
            "${HTTPSERVER_STATIC_PG_PREFIX}/lib/libpgcommon_shlib.a"
    OUTPUT_VARIABLE PGCOMMON_SYMBOLS
    RESULT_VARIABLE PGCOMMON_NM_RESULT)

  if(NOT PGCOMMON_NM_RESULT EQUAL 0)
    message(FATAL_ERROR "Could not inspect libpgcommon_shlib.a")
  endif()

  foreach(symbol pg_encoding_to_char pg_char_to_encoding)

    if(NOT PGCOMMON_SYMBOLS MATCHES "(^|[ \t])${symbol}(\n|$)")
      message(FATAL_ERROR "Missing public PostgreSQL symbol ${symbol} "
                          "in libpgcommon_shlib.a")
    endif()

  endforeach()

  # --------------------------------------------------------
  # Verify pkg-config static metadata
  # --------------------------------------------------------

  execute_process(
    COMMAND "${PKG_CONFIG_EXECUTABLE}" --static --libs libpq
    OUTPUT_VARIABLE LIBPQ_STATIC_FLAGS
    OUTPUT_STRIP_TRAILING_WHITESPACE
    RESULT_VARIABLE LIBPQ_FLAGS_RESULT)

  if(NOT LIBPQ_FLAGS_RESULT EQUAL 0)
    message(FATAL_ERROR "Could not retrieve static libpq linker flags")
  endif()

  separate_arguments(LIBPQ_STATIC_FLAG_LIST UNIX_COMMAND
                     "${LIBPQ_STATIC_FLAGS}")

  list(FIND LIBPQ_STATIC_FLAG_LIST "-lpgcommon_shlib" PGCOMMON_FLAG_INDEX)

  list(FIND LIBPQ_STATIC_FLAG_LIST "-lpgport_shlib" PGPORT_FLAG_INDEX)

  if(PGCOMMON_FLAG_INDEX EQUAL -1)
    message(FATAL_ERROR "libpq metadata does not include -lpgcommon_shlib:\n"
                        "${LIBPQ_STATIC_FLAGS}")
  endif()

  if(PGPORT_FLAG_INDEX EQUAL -1)
    message(FATAL_ERROR "libpq metadata does not include -lpgport_shlib:\n"
                        "${LIBPQ_STATIC_FLAGS}")
  endif()

  # Reject the incorrect archive variants.
  list(FIND LIBPQ_STATIC_FLAG_LIST "-lpgcommon" BAD_PGCOMMON_FLAG_INDEX)

  list(FIND LIBPQ_STATIC_FLAG_LIST "-lpgport" BAD_PGPORT_FLAG_INDEX)

  if(NOT BAD_PGCOMMON_FLAG_INDEX EQUAL -1 OR NOT BAD_PGPORT_FLAG_INDEX EQUAL -1)

    message(FATAL_ERROR "Incorrect PostgreSQL archive variants detected:\n"
                        "${LIBPQ_STATIC_FLAGS}")
  endif()

  # Reject optional dependencies disabled in the custom libpq.
  if(LIBPQ_STATIC_FLAGS MATCHES "gssapi|krb5|ldap|curl|pq-oauth|icu|lz4|zstd")

    message(FATAL_ERROR "Unexpected optional dependency in static libpq:\n"
                        "${LIBPQ_STATIC_FLAGS}")
  endif()

  message(STATUS "Static PostgreSQL:")
  message(STATUS "  Version: ${LIBPQ_VERSION}")
  message(STATUS "  Prefix:  ${LIBPQ_PREFIX_REAL}")
  message(STATUS "  Archives:")

  foreach(archive IN LISTS HTTPSERVER_POSTGRESQL_LIBRARIES)
    message(STATUS "    ${archive}")
  endforeach()

  message(STATUS "  Flags: ${LIBPQ_STATIC_FLAGS}")

  # --------------------------------------------------------
  # Include directories
  # --------------------------------------------------------

  set(HTTPSERVER_INCLUDE_DIRS
      ${LIBPQ_INCLUDE_DIRS} ${LIBURING_INCLUDE_DIRS} ${LIBDEFLATE_INCLUDE_DIRS}
      ${OPENSSL_SSL_INCLUDE_DIRS} ${OPENSSL_CRYPTO_INCLUDE_DIRS})

  # --------------------------------------------------------
  # Static library directories
  # --------------------------------------------------------

  set(HTTPSERVER_LIBRARY_DIRS
      ${LIBURING_STATIC_LIBRARY_DIRS} ${LIBDEFLATE_STATIC_LIBRARY_DIRS}
      ${OPENSSL_SSL_STATIC_LIBRARY_DIRS} ${OPENSSL_CRYPTO_STATIC_LIBRARY_DIRS})

  list(REMOVE_DUPLICATES HTTPSERVER_LIBRARY_DIRS)

  # --------------------------------------------------------
  # Other static dependencies
  #
  # PostgreSQL itself is linked by explicit archive path. Its internal archives
  # are NOT taken from LIBPQ_STATIC_LIBRARIES.
  # --------------------------------------------------------

  set(HTTPSERVER_SYSTEM_LIBRARIES
      ${LIBURING_STATIC_LIBRARIES}
      ${LIBDEFLATE_STATIC_LIBRARIES}
      ${OPENSSL_SSL_STATIC_LIBRARIES}
      ${OPENSSL_CRYPTO_STATIC_LIBRARIES}
      z
      m
      ${CMAKE_DL_LIBS}
      Threads::Threads)

  list(REMOVE_DUPLICATES HTTPSERVER_SYSTEM_LIBRARIES)

  set(HTTPSERVER_SYSTEM_LINK_OPTIONS
      ${LIBURING_STATIC_LDFLAGS_OTHER} ${LIBDEFLATE_STATIC_LDFLAGS_OTHER}
      ${OPENSSL_SSL_STATIC_LDFLAGS_OTHER}
      ${OPENSSL_CRYPTO_STATIC_LDFLAGS_OTHER})

  message(STATUS "Other static dependencies:")
  message(STATUS "  ${HTTPSERVER_SYSTEM_LIBRARIES}")

else()

  find_package(OpenSSL REQUIRED)
  find_package(PostgreSQL REQUIRED)

  pkg_check_modules(LIBURING REQUIRED IMPORTED_TARGET liburing)

  pkg_check_modules(LIBDEFLATE REQUIRED IMPORTED_TARGET libdeflate)

endif()

# ============================================================
# picohttpparser
# ============================================================

add_library(picohttpparser STATIC dependencies/picohttpparser.c)

target_include_directories(picohttpparser
                           PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/dependencies")

# ============================================================
# Native architecture optimization
# ============================================================

if(HTTPSERVER_NATIVE_ARCH AND (HTTPSERVER_GCC OR HTTPSERVER_CLANG))

  add_compile_options(-march=native -mtune=native)

endif()

# ============================================================
# Optimization flags
# ============================================================

add_compile_options(
  -O3
  -DNDEBUG
  -fstrict-aliasing
  -ffunction-sections
  -fdata-sections
  -fno-plt
  -pipe)

if(HTTPSERVER_GCC)

  add_compile_options(-funroll-loops -fprefetch-loop-arrays
                      -fno-semantic-interposition)

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
# Runtime and executable linking
# ============================================================

if(HTTPSERVER_FULL_STATIC)

  add_link_options(-static -static-libgcc -static-libstdc++)

else()

  if(HTTPSERVER_GCC)
    add_link_options(-shared-libgcc)
  endif()

endif()

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

target_include_directories(HttpServer PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")

# ============================================================
# Dependencies
# ============================================================

if(HTTPSERVER_FULL_STATIC)

  target_include_directories(HttpServer PRIVATE ${HTTPSERVER_INCLUDE_DIRS})

  target_link_directories(HttpServer PUBLIC ${HTTPSERVER_LIBRARY_DIRS})

  target_link_options(HttpServer PUBLIC ${HTTPSERVER_SYSTEM_LINK_OPTIONS})

  # Static library dependency order:
  #
  # libpq -> libpgcommon_shlib -> libpgport_shlib
  #
  # Exact paths ensure these are archives, never shared libraries.

  target_link_libraries(
    HttpServer PRIVATE picohttpparser ${HTTPSERVER_POSTGRESQL_LIBRARIES}
                       ${HTTPSERVER_SYSTEM_LIBRARIES})

else()

  target_link_libraries(
    HttpServer
    PRIVATE picohttpparser PostgreSQL::PostgreSQL PkgConfig::LIBURING
            PkgConfig::LIBDEFLATE OpenSSL::SSL OpenSSL::Crypto)

endif()

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

if(HTTPSERVER_FULL_STATIC)
  message(STATUS "PostgreSQL:       ${LIBPQ_VERSION}")
  message(STATUS "PostgreSQL mode:  STATIC")
endif()

message(STATUS "========================================")
