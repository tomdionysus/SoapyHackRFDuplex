// SPDX-License-Identifier: GPL-3.0-or-later
#include "rx_path.hpp"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <vector>

using namespace hackrfduplex;
using namespace std::chrono_literals;

namespace {

// A transfer of `samples` samples whose bytes count up from `first`.
std::vector<std::uint8_t> transfer(std::size_t samples, std::uint8_t first) {
    std::vector<std::uint8_t> bytes(samples * 2);
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        bytes[i] = static_cast<std::uint8_t>(first + i);
    }
    return bytes;
}

}  // namespace

TEST_CASE("an empty RX path times out", "[rx]") {
    RxPath path(2, 8);
    CHECK(path.mtu() == 4);
    std::array<std::int8_t, 8> dst{};
    const RxRead read = path.read(dst.data(), 4, Format::kCs8, 1ms);
    CHECK(read.samples == 0);
    CHECK_FALSE(read.overflow);
}

TEST_CASE("a transfer is read back with its position", "[rx]") {
    RxPath path(2, 8);
    path.on_transfer(transfer(4, 10));
    std::array<std::uint8_t, 16> dst{};
    const RxRead read = path.read(dst.data(), 10, Format::kCs8, 0us);
    CHECK(read.samples == 4);
    CHECK(read.position == 0);
    CHECK_FALSE(read.overflow);
    CHECK(std::vector<std::uint8_t>(dst.begin(), dst.begin() + 8) == transfer(4, 10));
    CHECK(path.received() == 4);
    CHECK(path.dropped() == 0);
}

TEST_CASE("reads smaller than a transfer continue where they left off", "[rx]") {
    RxPath path(2, 8);
    path.on_transfer(transfer(4, 0));
    path.on_transfer(transfer(4, 8));
    std::array<std::uint8_t, 16> dst{};

    RxRead read = path.read(dst.data(), 3, Format::kCs8, 0us);
    CHECK(read.samples == 3);
    CHECK(read.position == 0);
    CHECK(dst[0] == 0);
    CHECK(dst[5] == 5);

    read = path.read(dst.data(), 3, Format::kCs8, 0us);
    CHECK(read.samples == 1);
    CHECK(read.position == 3);
    CHECK(dst[0] == 6);
    CHECK(dst[1] == 7);

    read = path.read(dst.data(), 3, Format::kCs8, 0us);
    CHECK(read.samples == 3);
    CHECK(read.position == 4);
    CHECK(dst[0] == 8);
}

TEST_CASE("a transfer that finds the ring full is dropped and the gap reported once", "[rx]") {
    RxPath path(2, 8);
    path.on_transfer(transfer(4, 0));
    path.on_transfer(transfer(4, 8));
    path.on_transfer(transfer(4, 16));  // dropped
    CHECK(path.received() == 12);
    CHECK(path.dropped() == 4);

    std::array<std::uint8_t, 8> dst{};
    RxRead read = path.read(dst.data(), 4, Format::kCs8, 0us);
    CHECK(read.position == 0);
    read = path.read(dst.data(), 4, Format::kCs8, 0us);
    CHECK(read.position == 4);
    CHECK_FALSE(read.overflow);

    path.on_transfer(transfer(4, 24));
    read = path.read(dst.data(), 4, Format::kCs8, 0us);
    CHECK(read.samples == 0);
    CHECK(read.overflow);
    CHECK(read.position == 12);

    read = path.read(dst.data(), 4, Format::kCs8, 0us);
    CHECK(read.samples == 4);
    CHECK_FALSE(read.overflow);
    CHECK(read.position == 12);
    CHECK(dst[0] == 24);
}

TEST_CASE("a read converts into the stream's format", "[rx]") {
    RxPath path(1, 8);
    const std::array<std::uint8_t, 8> bytes = {0x80, 0x00, 0x40, 0x7F, 0, 0, 0, 0};
    path.on_transfer(bytes);
    std::array<float, 8> dst{};
    const RxRead read = path.read(dst.data(), 4, Format::kCf32, 0us);
    CHECK(read.samples == 4);
    CHECK(dst[0] == -1.0f);
    CHECK(dst[1] == 0.0f);
    CHECK(dst[2] == 0.5f);
    CHECK(dst[3] == 127.0f / 128.0f);
}

TEST_CASE("a short transfer is delivered at its own size", "[rx]") {
    RxPath path(1, 8);
    path.on_transfer(transfer(2, 0));
    std::array<std::uint8_t, 8> dst{};
    const RxRead read = path.read(dst.data(), 4, Format::kCs8, 0us);
    CHECK(read.samples == 2);
    path.on_transfer(transfer(4, 4));
    const RxRead next = path.read(dst.data(), 4, Format::kCs8, 0us);
    CHECK(next.position == 2);
    CHECK_FALSE(next.overflow);
}

TEST_CASE("reset starts a new stream", "[rx]") {
    RxPath path(1, 8);
    path.on_transfer(transfer(4, 0));
    path.on_transfer(transfer(4, 0));
    path.reset();
    CHECK(path.received() == 0);
    CHECK(path.dropped() == 0);
    std::array<std::uint8_t, 8> dst{};
    CHECK(path.read(dst.data(), 4, Format::kCs8, 0us).samples == 0);
    path.on_transfer(transfer(4, 0));
    const RxRead read = path.read(dst.data(), 4, Format::kCs8, 0us);
    CHECK(read.samples == 4);
    CHECK(read.position == 0);
    CHECK_FALSE(read.overflow);
}
