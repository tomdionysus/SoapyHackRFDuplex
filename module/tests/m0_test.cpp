// SPDX-License-Identifier: GPL-3.0-or-later
#include "m0.hpp"

#include <catch2/catch_test_macros.hpp>

namespace hackrfduplex::test {

namespace {

M0Reading idle_after(std::uint32_t bytes, std::uint32_t shortfalls, std::uint32_t error = 0) {
    return M0Reading{kM0ModeIdle, bytes, shortfalls, error};
}

}  // namespace

TEST_CASE("an M0 still idle on the last stream's counts has not taken up a start", "[m0]") {
    // The firmware acknowledges a start before its M4 passes it on, and
    // the M0 keeps the counts of the stream before until it takes it up.
    const M0Reading before = idle_after(123456, 4196);
    CHECK_FALSE(m0_took_up_start(before, before));
    const M0Reading stopped_itself = idle_after(5000, 3, 2);
    CHECK_FALSE(m0_took_up_start(stopped_itself, stopped_itself));
    const M0Reading fresh = idle_after(0, 0);
    CHECK_FALSE(m0_took_up_start(fresh, fresh));
}

TEST_CASE("an M0 in a streaming mode has taken up the start", "[m0]") {
    const M0Reading before = idle_after(123456, 4196);
    CHECK(m0_took_up_start(before, M0Reading{kM0ModeRx, 0, 0, 0}));
    CHECK(m0_took_up_start(before, M0Reading{kM0ModeTxStart, 0, 0, 0}));
    CHECK(m0_took_up_start(before, M0Reading{kM0ModeTxRun, 4096, 0, 0}));
}

TEST_CASE("an M0 idle again on new counts took up the start and stopped since", "[m0]") {
    // It started and stopped itself over a shortfall between two reads:
    // the counts were reset at the start, so they differ from before.
    const M0Reading before = idle_after(123456, 4196);
    CHECK(m0_took_up_start(before, idle_after(32768, 1, 2)));
    CHECK(m0_took_up_start(before, idle_after(123456, 0)));
    CHECK(m0_took_up_start(before, idle_after(0, 4196)));
    CHECK(m0_took_up_start(idle_after(0, 0), idle_after(0, 0, 1)));
}

TEST_CASE("an M0 is idle only in its idle mode", "[m0]") {
    CHECK(m0_idle(idle_after(5, 5)));
    CHECK_FALSE(m0_idle(M0Reading{kM0ModeRx, 0, 0, 0}));
    CHECK_FALSE(m0_idle(M0Reading{kM0ModeTxRun, 0, 0, 0}));
}

}  // namespace hackrfduplex::test
