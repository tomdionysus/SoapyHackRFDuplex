# SPDX-License-Identifier: GPL-3.0-or-later
#
# Sanitizer and coverage builds.

option(HACKRFDUPLEX_SANITIZE "Build with AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
option(HACKRFDUPLEX_TSAN "Build with ThreadSanitizer" OFF)
option(HACKRFDUPLEX_COVERAGE "Build with gcov instrumentation and add a coverage target" OFF)

set(_instrumented 0)
foreach(_option HACKRFDUPLEX_SANITIZE HACKRFDUPLEX_TSAN HACKRFDUPLEX_COVERAGE)
    if(${_option})
        math(EXPR _instrumented "${_instrumented} + 1")
    endif()
endforeach()
if(_instrumented GREATER 1)
    message(FATAL_ERROR
        "HACKRFDUPLEX_SANITIZE, HACKRFDUPLEX_TSAN and HACKRFDUPLEX_COVERAGE are separate builds")
endif()

if(HACKRFDUPLEX_SANITIZE)
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer
                        -fno-sanitize-recover=all)
    add_link_options(-fsanitize=address,undefined)
endif()

if(HACKRFDUPLEX_TSAN)
    add_compile_options(-fsanitize=thread -fno-omit-frame-pointer)
    add_link_options(-fsanitize=thread)
endif()

if(HACKRFDUPLEX_COVERAGE)
    # Atomic counters: the tests run several threads through the same
    # code, and plain counters lose increments and can even read negative.
    add_compile_options(--coverage -O0 -fprofile-update=atomic)
    add_link_options(--coverage)

    find_program(LCOV_EXECUTABLE lcov REQUIRED)
    find_program(GENHTML_EXECUTABLE genhtml REQUIRED)

    set(_cov_dir ${CMAKE_BINARY_DIR}/coverage)
    # Everything but what only hardware, SoapySDR's loader or a person runs:
    # the tests themselves, generated files, the SoapySDR registration, the
    # check tool, the FT8 client's main loop and ft8_lib's C, kept as its
    # authors wrote it. The SoapySDR translation is tested through the fake
    # boards and the libhackrf binding through a fake libhackrf, and both
    # are counted.
    set(HACKRFDUPLEX_COVERAGE_EXCLUDE
        "*/tests/*"
        "*/generated/*"
        "*/registration.cpp"
        "*/tools/*"
        "*/examples/ft8/src/main.cpp"
        "*/examples/ft8/src/ft8_lib/*")
    # Branches as the source has them: the compiler's exception-unwinding
    # edges, which no test can choose to take, are left out, as are the
    # branches it adds on a line with no condition (the clean-up loop of an
    # initialiser list, say) and a line holding only a closing brace, where
    # gcov counts the unwinding code too. LCOV_EXCL_LINE marks a line no
    # input can reach, such as the return after a switch that returns for
    # every value.
    set(_lcov_rc --rc branch_coverage=1 --rc no_exception_branch=1
                 --filter branch,brace,blank,region)

    add_custom_target(coverage
        COMMAND ${CMAKE_COMMAND} -E rm -rf ${_cov_dir}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${_cov_dir}
        COMMAND find ${CMAKE_BINARY_DIR} -name *.gcda -delete
        COMMAND ${CMAKE_CTEST_COMMAND} --test-dir ${CMAKE_BINARY_DIR} --output-on-failure
        COMMAND ${LCOV_EXECUTABLE} --quiet ${_lcov_rc} --capture --directory ${CMAKE_BINARY_DIR}
                --ignore-errors mismatch,mismatch,inconsistent
                --output-file ${_cov_dir}/all.info
        COMMAND ${LCOV_EXECUTABLE} --quiet ${_lcov_rc} --extract ${_cov_dir}/all.info
                "${CMAKE_SOURCE_DIR}/*" --ignore-errors inconsistent
                --output-file ${_cov_dir}/project.info
        COMMAND ${LCOV_EXECUTABLE} --quiet ${_lcov_rc} --remove ${_cov_dir}/project.info
                ${HACKRFDUPLEX_COVERAGE_EXCLUDE} --ignore-errors unused,unused,inconsistent
                --output-file ${_cov_dir}/coverage.info
        COMMAND ${GENHTML_EXECUTABLE} --quiet ${_lcov_rc} --output-directory ${_cov_dir}/html
                ${_cov_dir}/coverage.info
        COMMAND ${LCOV_EXECUTABLE} ${_lcov_rc} --list ${_cov_dir}/coverage.info
        COMMAND ${LCOV_EXECUTABLE} ${_lcov_rc} --summary ${_cov_dir}/coverage.info
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
        COMMENT "Running tests and writing the coverage report to ${_cov_dir}/html"
        VERBATIM)
endif()
