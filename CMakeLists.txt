# SPDX-License-Identifier: GPL-3.0-or-later
cmake_minimum_required(VERSION 3.24)

# One number for the repository: this, the tag and the module's reported version.
project(hackrfduplex VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

option(HACKRFDUPLEX_BUILD_MODULE "Build the SoapySDR module (needs SoapySDR)" ON)
option(HACKRFDUPLEX_BUILD_TESTS "Build the unit tests (needs Catch2 3)" ON)
option(HACKRFDUPLEX_BUILD_TOOLS "Build hackrfduplex-check (needs SoapySDR)" ON)

add_compile_options(-Wall -Wextra -Wpedantic -Wconversion -Wshadow -Werror)
# Clang proves the locking annotations (module/src/mutex.hpp); GCC has no
# such analysis and ignores them.
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    add_compile_options(-Wthread-safety -Wthread-safety-beta)
endif()

include(GNUInstallDirs)
include(cmake/Version.cmake)
include(cmake/Instrumentation.cmake)
include(cmake/LibHackRF.cmake)

find_package(Threads REQUIRED)

if(HACKRFDUPLEX_BUILD_MODULE OR HACKRFDUPLEX_BUILD_TOOLS)
    find_package(SoapySDR 0.8 CONFIG REQUIRED)
endif()

if(HACKRFDUPLEX_BUILD_TESTS)
    find_package(Catch2 3 REQUIRED)
    include(CTest)
    include(Catch)
endif()

add_subdirectory(module)
if(HACKRFDUPLEX_BUILD_TOOLS)
    add_subdirectory(tools)
endif()

message(STATUS "hackrfduplex ${HACKRFDUPLEX_VERSION}: module ${HACKRFDUPLEX_BUILD_MODULE}, "
               "tests ${HACKRFDUPLEX_BUILD_TESTS}, tools ${HACKRFDUPLEX_BUILD_TOOLS}")
