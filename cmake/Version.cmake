# SPDX-License-Identifier: GPL-3.0-or-later
#
# Sets HACKRFDUPLEX_VERSION from `git describe` at configure time, falling
# back to the project version, and writes a header with it into the build
# tree.

find_package(Git QUIET)
set(HACKRFDUPLEX_VERSION "${PROJECT_VERSION}")
if(GIT_FOUND)
    execute_process(
        COMMAND ${GIT_EXECUTABLE} describe --tags --always --dirty
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        OUTPUT_VARIABLE _hd_describe
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
        RESULT_VARIABLE _hd_describe_result)
    if(_hd_describe_result EQUAL 0 AND _hd_describe)
        set(HACKRFDUPLEX_VERSION "${_hd_describe}")
    endif()
endif()

configure_file(${CMAKE_CURRENT_LIST_DIR}/version.hpp.in
               ${CMAKE_BINARY_DIR}/generated/hackrfduplex/version.hpp @ONLY)
