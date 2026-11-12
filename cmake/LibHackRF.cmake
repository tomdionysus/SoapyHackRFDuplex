# SPDX-License-Identifier: GPL-3.0-or-later
#
# libhackrf as the imported target libhackrf::libhackrf: through pkg-config
# where it is on the path, otherwise by searching for the header and library
# (Homebrew on macOS puts the .pc file where a MacPorts pkg-config does not
# look).

find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(LIBHACKRF QUIET IMPORTED_TARGET libhackrf)
endif()

if(TARGET PkgConfig::LIBHACKRF)
    add_library(libhackrf::libhackrf ALIAS PkgConfig::LIBHACKRF)
    message(STATUS "libhackrf ${LIBHACKRF_VERSION} (pkg-config)")
else()
    find_path(LIBHACKRF_INCLUDE_DIR hackrf.h
              PATH_SUFFIXES libhackrf
              HINTS /usr/local/include /opt/local/include /opt/homebrew/include)
    find_library(LIBHACKRF_LIBRARY hackrf
                 HINTS /usr/local/lib /opt/local/lib /opt/homebrew/lib)
    if(NOT LIBHACKRF_INCLUDE_DIR OR NOT LIBHACKRF_LIBRARY)
        message(FATAL_ERROR "libhackrf not found: install the hackrf development files "
                            "(libhackrf-dev, or hackrf from Homebrew or MacPorts)")
    endif()
    add_library(libhackrf::libhackrf UNKNOWN IMPORTED)
    set_target_properties(libhackrf::libhackrf PROPERTIES
        IMPORTED_LOCATION "${LIBHACKRF_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${LIBHACKRF_INCLUDE_DIR}")
    message(STATUS "libhackrf ${LIBHACKRF_LIBRARY}")
endif()
