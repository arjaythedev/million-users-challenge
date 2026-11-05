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

# The benchmark builds on the actual target machine.
option(HTTPSERVER_NATIVE_ARCH "Optimize for the build CPU" ON)

# Fully static linking is optional because system libraries must provide
# compatible static archives.
option(HTTPSERVER_FULL_STATIC "Fully static executable" OFF)

# ============================================================
# Dependencies
# ============================================================

find_package(Boost REQUIRED)

if(NOT TARGET Boost::headers)
  add_library(Boost::headers INTERFACE IMPORTED)
  set_target_properties(Boost::headers PROPERTIES INTERFACE_INCLUDE_DIRECTORIES
                                                  "${Boost_INCLUDE_DIRS}")
endif()

find_package(OpenSSL REQUIRED)
find_package(libpqxx REQUIRED)
find_package(PkgConfig REQUIRED)

pkg_check_modules(liburing REQUIRED IMPORTED_TARGET liburing)

# ============================================================
# picohttpparser
# ============================================================

add_library(picohttpparser STATIC dependencies/picohttpparser.c)

target_include_directories(picohttpparser
                           PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/dependencies)

# ============================================================
# Compiler detection
# ============================================================

if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
  set(HTTPSERVER_GCC TRUE)
elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  set(HTTPSERVER_CLANG TRUE)
endif()

# ============================================================
# Maximum native architecture optimization
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
# Static GCC / C++ runtime
# ============================================================

if(HTTPSERVER_GCC OR HTTPSERVER_CLANG)

  add_link_options(-static-libgcc -static-libstdc++)

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

target_include_directories(HttpServer PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(
  HttpServer PRIVATE picohttpparser PkgConfig::liburing pqxx Boost::headers
                     OpenSSL::SSL OpenSSL::Crypto)

# ============================================================
# Production executable
# ============================================================

add_executable(http_server src/main.cpp)

set_target_properties(
  http_server
  PROPERTIES OUTPUT_NAME ".http_server"
             RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
             INTERPROCEDURAL_OPTIMIZATION ${HTTPSERVER_IPO_SUPPORTED})

target_link_libraries(http_server PRIVATE HttpServer)

target_compile_features(http_server PRIVATE cxx_std_23)

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
