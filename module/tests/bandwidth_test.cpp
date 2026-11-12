// SPDX-License-Identifier: GPL-3.0-or-later
#include "bandwidth.hpp"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>

using namespace hackrfduplex;

TEST_CASE("the filter table is the MAX2837's, ascending", "[bandwidth]") {
    const auto table = filter_bandwidths();
    CHECK(table.size() == 16);
    CHECK(std::is_sorted(table.begin(), table.end()));
    CHECK(table.front() == 1'750'000);
    CHECK(table.back() == 28'000'000);
}

TEST_CASE("a requested bandwidth rounds down to a filter, never below the narrowest",
          "[bandwidth]") {
    CHECK(round_down_bandwidth(1e6) == 1'750'000);
    CHECK(round_down_bandwidth(1.75e6) == 1'750'000);
    CHECK(round_down_bandwidth(3e6) == 2'500'000);
    CHECK(round_down_bandwidth(3.5e6) == 3'500'000);
    CHECK(round_down_bandwidth(28e6) == 28'000'000);
    CHECK(round_down_bandwidth(100e6) == 28'000'000);
}

TEST_CASE("the automatic bandwidth is the firmware's three quarters rule", "[bandwidth]") {
    CHECK(auto_bandwidth(0) == 0);
    CHECK(auto_bandwidth(-1) == 0);
    CHECK(auto_bandwidth(2e6) == 1'750'000);
    CHECK(auto_bandwidth(8e6) == 6'000'000);
    CHECK(auto_bandwidth(10e6) == 7'000'000);
    CHECK(auto_bandwidth(15.36e6) == 10'000'000);
    CHECK(auto_bandwidth(20e6) == 15'000'000);
}
