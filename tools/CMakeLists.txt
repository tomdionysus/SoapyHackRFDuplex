# SPDX-License-Identifier: GPL-3.0-or-later
#
# hackrfduplex-check: streams through the module as any SoapySDR client
# would and reports what it saw.

add_executable(hackrfduplex-check check.cpp)
target_include_directories(hackrfduplex-check SYSTEM PRIVATE ${SoapySDR_INCLUDE_DIRS})
target_link_libraries(hackrfduplex-check PRIVATE hackrfduplex_core ${SoapySDR_LIBRARIES} Threads::Threads)
install(TARGETS hackrfduplex-check RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES hackrfduplex-check.1 DESTINATION ${CMAKE_INSTALL_MANDIR}/man1)
