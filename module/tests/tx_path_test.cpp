// SPDX-License-Identifier: GPL-3.0-or-later
#include "tx_path.hpp"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <vector>

using namespace hackrfduplex;
using namespace std::chrono_literals;

namespace {

std::vector<std::int8_t> samples(std::size_t count, std::int8_t first) {
    std::vector<std::int8_t> out(count * 2);
    for (std::size_t i = 0; i < out.size(); ++i) {
        out[i] = static_cast<std::int8_t>(first + static_cast<std::int8_t>(i));
    }
    return out;
}

std::vector<std::uint8_t> pull(TxPath& path, std::size_t bytes) {
    std::vector<std::uint8_t> buffer(bytes, 0xAA);
    path.on_transfer(buffer);
    return buffer;
}

}  // namespace

TEST_CASE("before anything is written the board gets silence, not an underflow", "[tx]") {
    TxPath path(2, 8);
    CHECK(path.mtu() == 4);
    CHECK(pull(path, 8) == std::vector<std::uint8_t>(8, 0));
    CHECK_FALSE(path.take_underflow());
    CHECK(path.sent() == 4);
    CHECK(path.underflows() == 0);
}

TEST_CASE("a full block goes out as one transfer", "[tx]") {
    TxPath path(2, 8);
    const auto in = samples(4, 1);
    CHECK(path.write(in.data(), 4, Format::kCs8, false, 0us) == 4);
    const auto out = pull(path, 8);
    for (std::size_t i = 0; i < 8; ++i) {
        CHECK(static_cast<std::int8_t>(out[i]) == in[i]);
    }
    CHECK_FALSE(path.take_underflow());
}

TEST_CASE("a write takes at most the room left in the block", "[tx]") {
    TxPath path(2, 8);
    const auto in = samples(6, 0);
    CHECK(path.write(in.data(), 6, Format::kCs8, false, 0us) == 4);
    CHECK(path.write(in.data() + 8, 2, Format::kCs8, false, 0us) == 2);
    // The second block is half full and unpublished, so after the first
    // the board runs dry.
    pull(path, 8);
    const auto out = pull(path, 8);
    CHECK(out == std::vector<std::uint8_t>(8, 0));
    CHECK(path.take_underflow());
    CHECK_FALSE(path.take_underflow());
    CHECK(path.underflows() == 1);
}

TEST_CASE("the end of a burst sends the partial block and silence is then expected", "[tx]") {
    TxPath path(2, 8);
    const auto in = samples(2, 5);
    CHECK(path.write(in.data(), 2, Format::kCs8, true, 0us) == 2);
    auto out = pull(path, 8);
    CHECK(static_cast<std::int8_t>(out[0]) == 5);
    CHECK(static_cast<std::int8_t>(out[3]) == 8);
    CHECK(std::vector<std::uint8_t>(out.begin() + 4, out.end()) == std::vector<std::uint8_t>(4, 0));
    CHECK_FALSE(path.take_underflow());
    out = pull(path, 8);
    CHECK(out == std::vector<std::uint8_t>(8, 0));
    CHECK_FALSE(path.take_underflow());

    // A new burst makes running dry an underflow again.
    const auto more = samples(4, 0);
    CHECK(path.write(more.data(), 4, Format::kCs8, false, 0us) == 4);
    pull(path, 8);
    pull(path, 8);
    CHECK(path.take_underflow());
}

TEST_CASE("a transfer concatenates blocks and carries the rest over", "[tx]") {
    TxPath path(3, 8);
    const auto burst = samples(2, 0);
    const auto block = samples(4, 10);
    REQUIRE(path.write(burst.data(), 2, Format::kCs8, true, 0us) == 2);
    REQUIRE(path.write(block.data(), 4, Format::kCs8, false, 0us) == 4);
    auto out = pull(path, 8);
    CHECK(static_cast<std::int8_t>(out[0]) == 0);
    CHECK(static_cast<std::int8_t>(out[3]) == 3);
    CHECK(static_cast<std::int8_t>(out[4]) == 10);
    CHECK(static_cast<std::int8_t>(out[7]) == 13);
    CHECK_FALSE(path.take_underflow());
    out = pull(path, 8);
    CHECK(static_cast<std::int8_t>(out[0]) == 14);
    CHECK(static_cast<std::int8_t>(out[3]) == 17);
    CHECK(std::vector<std::uint8_t>(out.begin() + 4, out.end()) == std::vector<std::uint8_t>(4, 0));
    CHECK(path.take_underflow());
}

TEST_CASE("a writer waits for room and gives up at the timeout", "[tx]") {
    TxPath path(2, 8);
    const auto in = samples(4, 0);
    CHECK(path.write(in.data(), 4, Format::kCs8, false, 0us) == 4);
    CHECK(path.write(in.data(), 4, Format::kCs8, false, 0us) == 4);
    CHECK(path.write(in.data(), 4, Format::kCs8, false, 1ms) == 0);
    pull(path, 8);
    CHECK(path.write(in.data(), 4, Format::kCs8, false, 0us) == 4);
}

TEST_CASE("a write converts from the stream's format", "[tx]") {
    TxPath path(1, 8);
    const std::array<float, 8> in = {1.0f, -1.0f, 0.5f, 0.0f, 0.25f, -0.25f, 2.0f, -2.0f};
    CHECK(path.write(in.data(), 4, Format::kCf32, false, 0us) == 4);
    const auto out = pull(path, 8);
    const std::array<std::int8_t, 8> expected = {127, -128, 64, 0, 32, -32, 127, -128};
    for (std::size_t i = 0; i < 8; ++i) {
        CHECK(static_cast<std::int8_t>(out[i]) == expected[i]);
    }
}

TEST_CASE("reset forgets a half-written block and the counters", "[tx]") {
    TxPath path(2, 8);
    const auto in = samples(2, 0);
    CHECK(path.write(in.data(), 2, Format::kCs8, false, 0us) == 2);
    pull(path, 8);
    path.reset();
    CHECK(path.sent() == 0);
    CHECK(path.underflows() == 0);
    CHECK_FALSE(path.take_underflow());
    CHECK(pull(path, 8) == std::vector<std::uint8_t>(8, 0));
    CHECK_FALSE(path.take_underflow());
}
