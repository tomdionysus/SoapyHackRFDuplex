# SPDX-License-Identifier: GPL-3.0-or-later
#
# Sanitizer and coverage builds.

option(HACKRFDUPLEX_SANITIZE "Build with AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
option(HACKRFDUPLEX_COVERAGE "Build with gcov instrumentation and add a coverage target" OFF)

if(HACKRFDUPLEX_SANITIZE AND HACKRFDUPLEX_COVERAGE)
    message(FATAL_ERROR "HACKRFDUPLEX_SANITIZE and HACKRFDUPLEX_COVERAGE are separate builds")
endif()

if(HACKRFDUPLEX_SANITIZE)
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer
                        -fno-sanitize-recover=all)
    add_link_options(-fsanitize=address,undefined)
endif()

if(HACKRFDUPLEX_COVERAGE)
    add_compile_options(--coverage -O0)
    add_link_options(--coverage)

    find_program(LCOV_EXECUTABLE lcov REQUIRED)
    find_program(GENHTML_EXECUTABLE genhtml REQUIRED)

    set(_cov_dir ${CMAKE_BINARY_DIR}/coverage)
    # Unit-tested sources only: the tests, generated files and the libhackrf
    # and SoapySDR glue, which only hardware exercises, are excluded.
    set(HACKRFDUPLEX_COVERAGE_EXCLUDE
        "*/tests/*"
        "*/generated/*"
        "*/hackrf_board.cpp"
        "*/soapy_device.cpp"
        "*/registration.cpp"
        "*/tools/*"
        CACHE STRING "lcov patterns excluded from the coverage report")

    add_custom_target(coverage
        COMMAND ${CMAKE_COMMAND} -E rm -rf ${_cov_dir}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${_cov_dir}
        COMMAND find ${CMAKE_BINARY_DIR} -name *.gcda -delete
        COMMAND ${CMAKE_CTEST_COMMAND} --test-dir ${CMAKE_BINARY_DIR} --output-on-failure
        COMMAND ${LCOV_EXECUTABLE} --quiet --capture --directory ${CMAKE_BINARY_DIR}
                --ignore-errors mismatch,inconsistent
                --output-file ${_cov_dir}/all.info
        COMMAND ${LCOV_EXECUTABLE} --quiet --extract ${_cov_dir}/all.info
                "${CMAKE_SOURCE_DIR}/*" --ignore-errors inconsistent
                --output-file ${_cov_dir}/project.info
        COMMAND ${LCOV_EXECUTABLE} --quiet --remove ${_cov_dir}/project.info
                ${HACKRFDUPLEX_COVERAGE_EXCLUDE} --ignore-errors unused,unused,inconsistent
                --output-file ${_cov_dir}/coverage.info
        COMMAND ${GENHTML_EXECUTABLE} --quiet --output-directory ${_cov_dir}/html
                ${_cov_dir}/coverage.info
        COMMAND ${LCOV_EXECUTABLE} --summary ${_cov_dir}/coverage.info
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
        COMMENT "Running tests and writing the coverage report to ${_cov_dir}/html"
        VERBATIM)
endif()
