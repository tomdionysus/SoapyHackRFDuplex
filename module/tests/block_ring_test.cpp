// SPDX-License-Identifier: GPL-3.0-or-later
#include "block_ring.hpp"

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <stdexcept>
#include <thread>

using namespace hackrfduplex;
using namespace std::chrono_literals;

TEST_CASE("a ring needs slots and bytes", "[ring]") {
    CHECK_THROWS_AS(BlockRing(0, 8), std::invalid_argument);
    CHECK_THROWS_AS(BlockRing(2, 0), std::invalid_argument);
    const BlockRing ring(3, 8);
    CHECK(ring.slots() == 3);
    CHECK(ring.block_bytes() == 8);
    CHECK(ring.size() == 0);
}

TEST_CASE("a ring reset to use fewer of its slots holds only those", "[ring]") {
    BlockRing ring(4, 8);
    ring.reset(2);
    CHECK(ring.slots() == 4);
    CHECK(ring.in_use() == 2);
    for (int k = 0; k < 2; ++k) {
        Block* block = ring.acquire();
        REQUIRE(block != nullptr);
        CHECK(ring.index_of(block) < 2);
        ring.publish();
    }
    CHECK(ring.acquire() == nullptr);
    CHECK_FALSE(ring.wait_free(0us));
    // And around again, within the two.
    ring.pop();
    Block* again = ring.acquire();
    REQUIRE(again != nullptr);
    CHECK(ring.index_of(again) == 0);

    ring.reset(4);
    CHECK(ring.in_use() == 4);
    for (int k = 0; k < 4; ++k) {
        REQUIRE(ring.acquire() != nullptr);
        ring.publish();
    }
    CHECK(ring.acquire() == nullptr);
    ring.reset();
    CHECK(ring.in_use() == 4);
    CHECK_THROWS_AS(ring.reset(0), std::invalid_argument);
    CHECK_THROWS_AS(ring.reset(5), std::invalid_argument);
}

TEST_CASE("blocks pass from producer to consumer in order", "[ring]") {
    BlockRing ring(2, 8);
    REQUIRE(ring.front() == nullptr);

    Block* first = ring.acquire();
    REQUIRE(first != nullptr);
    CHECK(first->bytes.size() == 8);
    first->size = 8;
    first->position = 0;
    ring.publish();
    CHECK(ring.size() == 1);

    Block* second = ring.acquire();
    REQUIRE(second != nullptr);
    CHECK(second != first);
    second->size = 4;
    second->position = 4;
    ring.publish();
    CHECK(ring.size() == 2);
    CHECK(ring.acquire() == nullptr);

    REQUIRE(ring.front() == first);
    ring.pop();
    REQUIRE(ring.front() == second);
    CHECK(ring.front()->position == 4);
    ring.pop();
    CHECK(ring.front() == nullptr);
    CHECK(ring.size() == 0);

    // The freed slot is clean for its next use.
    Block* third = ring.acquire();
    REQUIRE(third == first);
    CHECK(third->size == 0);
    CHECK_FALSE(third->end_of_burst);
}

TEST_CASE("waits report what the other side did", "[ring]") {
    BlockRing ring(1, 8);
    CHECK_FALSE(ring.wait_front(1ms));
    CHECK(ring.wait_free(1ms));

    ring.acquire();
    ring.publish();
    CHECK(ring.wait_front(0us));
    CHECK_FALSE(ring.wait_free(1ms));

    std::thread consumer([&] {
        std::this_thread::sleep_for(5ms);
        ring.pop();
    });
    CHECK(ring.wait_free(1s));
    consumer.join();
}

TEST_CASE("wake releases a waiter, which reports false", "[ring]") {
    BlockRing ring(1, 8);
    bool result = true;
    std::thread waiter([&] { result = ring.wait_front(5s); });
    std::this_thread::sleep_for(5ms);
    const auto before = std::chrono::steady_clock::now();
    ring.wake();
    waiter.join();
    CHECK_FALSE(result);
    CHECK(std::chrono::steady_clock::now() - before < 1s);
}

TEST_CASE("reset empties the ring", "[ring]") {
    BlockRing ring(2, 8);
    ring.acquire()->size = 8;
    ring.publish();
    ring.acquire()->end_of_burst = true;
    ring.publish();
    ring.reset();
    CHECK(ring.size() == 0);
    CHECK(ring.front() == nullptr);
    Block* block = ring.acquire();
    REQUIRE(block != nullptr);
    CHECK(block->size == 0);
    CHECK_FALSE(block->end_of_burst);
}
