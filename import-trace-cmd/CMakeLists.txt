cmake_minimum_required(VERSION 3.18) # for file(ARCHIVE_EXTRACT)
project(import-trace-cmd CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

add_compile_options(-Wall -Wextra -Wpedantic)

find_package(PkgConfig REQUIRED)
pkg_check_modules(TRACECMD REQUIRED IMPORTED_TARGET libtracecmd libtraceevent)

set(DUCKDB_VERSION "1.5.2")
set(DUCKDB_URL
    "https://install.duckdb.org/v${DUCKDB_VERSION}/libduckdb-linux-amd64.zip")
set(DUCKDB_DIR "${CMAKE_BINARY_DIR}/_deps/duckdb")
set(DUCKDB_ZIP "${CMAKE_BINARY_DIR}/_deps/libduckdb-linux-amd64.zip")

if(NOT EXISTS "${DUCKDB_DIR}/duckdb.hpp")
  if(NOT EXISTS "${DUCKDB_ZIP}")
    message(STATUS "Downloading DuckDB ${DUCKDB_VERSION}")
    file(
      DOWNLOAD "${DUCKDB_URL}" "${DUCKDB_ZIP}"
      SHOW_PROGRESS
      STATUS download_status)
    list(GET download_status 0 status_code)
    if(NOT status_code EQUAL 0)
      list(GET download_status 1 status_msg)
      message(FATAL_ERROR "DuckDB download failed: ${status_msg}")
    endif()
  endif()
  file(ARCHIVE_EXTRACT INPUT "${DUCKDB_ZIP}" DESTINATION "${DUCKDB_DIR}")
endif()

add_executable(import-trace-cmd main.cpp)
target_include_directories(import-trace-cmd SYSTEM PRIVATE ${DUCKDB_DIR})
target_link_libraries(import-trace-cmd PRIVATE PkgConfig::TRACECMD
                                               ${DUCKDB_DIR}/libduckdb.so)
target_link_options(import-trace-cmd PRIVATE -Wl,-rpath,${DUCKDB_DIR})
