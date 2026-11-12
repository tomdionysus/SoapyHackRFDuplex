# SPDX-License-Identifier: GPL-3.0-or-later
#
# hackrfduplex-ft8: an example SoapySDR client for the pair that decodes
# FT8, receive only, or sends a frame and decodes it back (--loopback). The
# FT8 code comes from corvus-radioberry's ft8rx, a C++ port of
# kgoba/ft8_lib; the message text code is ft8_lib's own (MIT,
# src/ft8_lib/LICENSE). It needs liquid-dsp, so it builds with
# HACKRFDUPLEX_BUILD_EXAMPLES; it is not packaged.

enable_language(C)

# ft8_lib's C, kept as its authors wrote it: not held to this project's warnings.
add_library(ft8_lib STATIC src/ft8_lib/message.c src/ft8_lib/text.c)
target_include_directories(ft8_lib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_options(ft8_lib PRIVATE -w)
# It uses stpcpy, which strict C11 hides.
target_compile_definitions(ft8_lib PRIVATE _POSIX_C_SOURCE=200809L)
set_target_properties(ft8_lib PROPERTIES C_STANDARD 11 C_EXTENSIONS OFF)

add_library(ft8_core STATIC
    src/ft8/codec.cpp
    src/ft8/constants.cpp
    src/ft8/decoder.cpp
    src/decimator.cpp
    src/resampler.cpp
    src/message_text.cpp)
target_include_directories(ft8_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(ft8_core PUBLIC ft8_lib liquid::liquid)
# The borrowed C++ was written to GCC's -Wconversion, which leaves sign
# conversions out; Clang's includes them. Held to what it was written to.
set_source_files_properties(
    src/ft8/codec.cpp src/ft8/constants.cpp src/ft8/decoder.cpp src/decimator.cpp
    src/message_text.cpp
    PROPERTIES COMPILE_OPTIONS "$<$<CXX_COMPILER_ID:Clang,AppleClang>:-Wno-sign-conversion>")

add_executable(hackrfduplex-ft8 src/main.cpp)
target_link_libraries(hackrfduplex-ft8 PRIVATE ft8_core SoapySDR Threads::Threads)

if(HACKRFDUPLEX_BUILD_TESTS)
    add_executable(ft8_tests tests/ft8_test.cpp tests/resampler_test.cpp)
    target_link_libraries(ft8_tests PRIVATE ft8_core Catch2::Catch2WithMain)
    catch_discover_tests(ft8_tests)
endif()
