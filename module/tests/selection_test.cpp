// SPDX-License-Identifier: GPL-3.0-or-later
#include "selection.hpp"

#include <catch2/catch_test_macros.hpp>
#include <stdexcept>

using namespace hackrfduplex;

namespace {

const std::vector<ListedBoard> kTwo = {
    {"0000000000000000708061dc214f244b", "HackRF One"},
    {"000000000000000064a868dc340e7957", "HackRF One"},
};

}  // namespace

TEST_CASE("two boards and no serials pair themselves in serial order", "[selection]") {
    const Selection s = select_pair({}, kTwo);
    CHECK(s.rx == "000000000000000064a868dc340e7957");
    CHECK(s.tx == "0000000000000000708061dc214f244b");
}

TEST_CASE("serials match by suffix and the other board takes the other role", "[selection]") {
    Selection s = select_pair({{"rx_serial", "244b"}}, kTwo);
    CHECK(s.rx == "0000000000000000708061dc214f244b");
    CHECK(s.tx == "000000000000000064a868dc340e7957");

    s = select_pair({{"tx_serial", "708061dc214f244b"}}, kTwo);
    CHECK(s.tx == "0000000000000000708061dc214f244b");
    CHECK(s.rx == "000000000000000064a868dc340e7957");

    s = select_pair({{"rx_serial", "7957"}, {"tx_serial", "244b"}}, kTwo);
    CHECK(s.rx == "000000000000000064a868dc340e7957");
    CHECK(s.tx == "0000000000000000708061dc214f244b");
}

TEST_CASE("a selection that cannot be made says why", "[selection]") {
    CHECK_THROWS_AS(select_pair({}, {}), std::invalid_argument);
    CHECK_THROWS_AS(select_pair({}, {kTwo[0]}), std::invalid_argument);
    CHECK_THROWS_AS(select_pair({{"rx_serial", "ffff"}}, kTwo), std::invalid_argument);
    CHECK_THROWS_AS(select_pair({{"rx_serial", "244b"}, {"tx_serial", "244b"}}, kTwo),
                    std::invalid_argument);
    const std::vector<ListedBoard> three = {
        kTwo[0], kTwo[1], {"00000000000000000000000000000003", "HackRF One"}};
    CHECK_THROWS_AS(select_pair({}, three), std::invalid_argument);
    CHECK_THROWS_AS(select_pair({{"rx_serial", "244b"}}, three), std::invalid_argument);
    // A suffix shared by two boards is ambiguous.
    const std::vector<ListedBoard> twins = {{"00000000000000000000000000000a11", "HackRF One"},
                                            {"00000000000000000000000000000b11", "HackRF One"}};
    CHECK_THROWS_AS(select_pair({{"rx_serial", "11"}}, twins), std::invalid_argument);
}

TEST_CASE("discovery offers every ordered pair, or the one asked for", "[selection]") {
    const auto all = enumerate_pairs({}, kTwo);
    REQUIRE(all.size() == 2);
    CHECK(all[0].rx == kTwo[0].serial);
    CHECK(all[0].tx == kTwo[1].serial);
    CHECK(all[1].rx == kTwo[1].serial);
    CHECK(all[1].tx == kTwo[0].serial);

    const auto one = enumerate_pairs({{"rx_serial", "7957"}}, kTwo);
    REQUIRE(one.size() == 1);
    CHECK(one[0].rx == kTwo[1].serial);

    CHECK(enumerate_pairs({{"rx_serial", "nope"}}, kTwo).empty());
    CHECK(enumerate_pairs({}, {kTwo[0]}).empty());
}

TEST_CASE("labels trim the zero padding", "[selection]") {
    CHECK(short_serial("0000000000000000708061dc214f244b") == "708061dc214f244b");
    CHECK(short_serial("0000") == "0");
    CHECK(short_serial("") == "");
    CHECK(pair_label(Selection{kTwo[0].serial, kTwo[1].serial}) ==
          "HackRF Duplex RX 708061dc214f244b TX 64a868dc340e7957");
}
