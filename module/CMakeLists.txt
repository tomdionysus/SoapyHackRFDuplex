# SPDX-License-Identifier: GPL-3.0-or-later
#
# The duplex pair, its libhackrf binding, the SoapySDR module and the tests.

# Everything that does not touch hardware or SoapySDR: formats, rings, the
# gain and filter models, the two stream paths and the pair itself. Tested
# against fake boards.
add_library(hackrfduplex_core STATIC
    src/format.cpp
    src/convert.cpp
    src/block_ring.cpp
    src/gain.cpp
    src/bandwidth.cpp
    src/rx_path.cpp
    src/tx_path.cpp
    src/pair.cpp
    src/selection.cpp)
target_include_directories(hackrfduplex_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_BINARY_DIR}/generated)
target_link_libraries(hackrfduplex_core PUBLIC Threads::Threads)

# The Board over libhackrf, and the device listing.
add_library(hackrfduplex_hackrf STATIC src/hackrf_board.cpp)
target_link_libraries(hackrfduplex_hackrf PUBLIC hackrfduplex_core libhackrf::libhackrf)

if(HACKRFDUPLEX_BUILD_MODULE)
    # SoapySDR's helper makes the MODULE library and installs it into the
    # SoapySDR modules directory under the install prefix.
    SOAPY_SDR_MODULE_UTIL(
        TARGET HackRFDuplexSupport
        SOURCES src/soapy_device.cpp src/registration.cpp
        LIBRARIES hackrfduplex_hackrf)
    # SoapySDR's headers are not held to this tree's warning flags.
    target_include_directories(HackRFDuplexSupport SYSTEM PRIVATE ${SoapySDR_INCLUDE_DIRS})
endif()

if(HACKRFDUPLEX_BUILD_TESTS)
    add_executable(hackrfduplex_tests
        tests/fake_board.cpp
        tests/convert_test.cpp
        tests/block_ring_test.cpp
        tests/gain_test.cpp
        tests/bandwidth_test.cpp
        tests/rx_path_test.cpp
        tests/tx_path_test.cpp
        tests/pair_test.cpp
        tests/selection_test.cpp)
    target_link_libraries(hackrfduplex_tests PRIVATE hackrfduplex_core Catch2::Catch2WithMain)
    catch_discover_tests(hackrfduplex_tests)
endif()
