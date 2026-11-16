// SPDX-License-Identifier: GPL-3.0-or-later
//
// The libhackrf binding, against fake_libhackrf: what it asks of libhackrf,
// what it makes of the answers, and how it recovers when a board does not
// stream or its streaming engine does not follow.

#include "hackrf_board.hpp"

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "fake_libhackrf.hpp"

using namespace hackrfduplex;
namespace fake = hackrfduplex::test::fake_hackrf;

namespace {

const std::string kRx = "0000000000000000a06063c8dc340e01";
const std::string kTx = "0000000000000000a06063c8dc340e02";

// Two boards on the bus, the binding's view of them freshly made.
struct Bus {
    Bus() {
        fake::reset();
        for (const std::string& serial : {kTx, kRx}) {
            fake::Device d;
            d.serial = serial;
            fake::state().devices.push_back(d);
        }
    }
    ~Bus() { fake::reset(); }
    Bus(const Bus&) = delete;
    Bus& operator=(const Bus&) = delete;

    static fake::Device& rx() { return fake::device(kRx); }
    static void fail_next(const std::string& function, int result = HACKRF_ERROR_LIBUSB) {
        fake::state().fail_next[function] = result;
    }
};

bool throws_naming(const std::function<void()>& call, const std::string& text) {
    try {
        call();
    } catch (const BoardError& e) {
        return std::string(e.what()).find(text) != std::string::npos;
    }
    return false;
}

}  // namespace

TEST_CASE("libhackrf is set up once for overlapping sessions", "[hackrf]") {
    const Bus bus;
    {
        const HackrfSession first;
        const HackrfSession second;
        CHECK(fake::state().inits == 1);
        CHECK(fake::state().exits == 0);
    }
    CHECK(fake::state().exits == 1);
    fake::state().init_result = HACKRF_ERROR_LIBUSB;
    CHECK(throws_naming([] { const HackrfSession session; }, "hackrf_init: HACKRF_ERROR_LIBUSB"));
    // A session that failed to start does not end libhackrf.
    CHECK(fake::state().exits == 1);
}

TEST_CASE("the boards are listed in serial order without opening them", "[hackrf]") {
    const Bus bus;
    fake::state().list_unreadable = true;
    const std::vector<ListedBoard> boards = list_boards();
    REQUIRE(boards.size() == 2);
    CHECK(boards[0].serial == kRx);
    CHECK(boards[1].serial == kTx);
    CHECK(boards[0].name == "HackRF One");
    CHECK_FALSE(Bus::rx().open);
    CHECK(fake::state().inits == 1);
    CHECK(fake::state().exits == 1);
    // libhackrf without a list lists nothing.
    fake::state().list_fails = true;
    CHECK(list_boards().empty());
    CHECK(libhackrf_version() == "fake-libhackrf");
}

TEST_CASE("a board opens by serial and learns its firmware's abilities", "[hackrf]") {
    const Bus bus;
    Bus::rx().transfer_size = 262144;
    const HackrfBoard board(kRx);
    CHECK(board.serial() == kRx);
    CHECK(Bus::rx().open);
    CHECK(board.transfer_bytes() == 262144);
    CHECK(
        throws_naming([] { const HackrfBoard none("ffff"); }, "open ffff: HACKRF_ERROR_NOT_FOUND"));
    Bus::fail_next("hackrf_usb_api_version_read");
    CHECK(throws_naming([] { const HackrfBoard b(kTx); }, "usb api version"));
    Bus::fail_next("hackrf_board_id_read");
    CHECK(throws_naming([] { const HackrfBoard b(kTx); }, "board id"));
}

TEST_CASE("a board's identity reads from the board", "[hackrf]") {
    const Bus bus;
    Bus::rx().clkin = 1;
    HackrfBoard board(kRx);
    BoardInfo info = board.info();
    CHECK(info.serial == kRx);
    CHECK(info.board == "HackRF One");
    CHECK(info.firmware == "2026.01.3");
    CHECK(info.usb_api == "1.10");
    CHECK(info.revision == "r10");
    CHECK(info.external_clock);
    // CLK3's control byte reads zero here: powered up, so CLKOUT is on.
    CHECK(info.clock_out);
    Bus::rx().si5351[19] = 0x80;
    CHECK_FALSE(board.info().clock_out);

    // Reads that fail leave what they would have said unknown.
    Bus::fail_next("hackrf_board_rev_read");
    Bus::fail_next("hackrf_get_clkin_status");
    Bus::fail_next("hackrf_si5351c_read");
    info = board.info();
    CHECK(info.revision == "unknown");
    CHECK_FALSE(info.external_clock);
    CHECK_FALSE(info.clock_out);

    for (const char* function :
         {"hackrf_board_id_read", "hackrf_version_string_read", "hackrf_usb_api_version_read"}) {
        CAPTURE(function);
        Bus::fail_next(function);
        CHECK_THROWS_AS(board.info(), BoardError);
    }
}

TEST_CASE("firmware older than USB API 1.06 says less", "[hackrf]") {
    const Bus bus;
    Bus::rx().usb_api = 0x0104;
    Bus::rx().clkin = 1;
    HackrfBoard board(kRx);
    const BoardInfo info = board.info();
    CHECK(info.usb_api == "1.04");
    CHECK(info.revision == "unknown");
    CHECK_FALSE(info.external_clock);
    CHECK_FALSE(board.start_switches_reference());
    CHECK(board.stream_health().shortfalls == 0);
    CHECK_FALSE(board.stream_health().stopped_itself);
    CHECK_FALSE(board.set_tx_underrun_limit(1000));
    // It streams without its streaming engine being asked.
    board.start_rx([](auto) {});
    CHECK(board.streaming());
    board.stop();
    for (const std::string& call : fake::state().calls) {
        CHECK(call.rfind("hackrf_get_m0_state", 0) != 0);
    }
}

TEST_CASE("each setting reaches the board, and a refusal names it", "[hackrf]") {
    const Bus bus;
    HackrfBoard board(kRx);
    const fake::Device& d = Bus::rx();
    board.set_frequency(915'000'000);
    board.set_sample_rate(8e6);
    board.set_bandwidth(5'000'000);
    board.set_amp(true);
    board.set_lna_gain(16);
    board.set_vga_gain(20);
    board.set_tx_vga_gain(30);
    board.set_bias_tee(true);
    board.set_clock_out(true);
    board.set_hardware_sync(true);
    CHECK(d.frequency_hz == 915'000'000);
    CHECK(d.sample_rate_hz == 8e6);
    CHECK(d.bandwidth_hz == 5'000'000);
    CHECK(d.amp == 1);
    CHECK(d.lna_db == 16);
    CHECK(d.vga_db == 20);
    CHECK(d.tx_vga_db == 30);
    CHECK(d.antenna == 1);
    CHECK(d.clkout == 1);
    CHECK(d.hw_sync == 1);
    board.set_amp(false);
    board.set_bias_tee(false);
    board.set_clock_out(false);
    board.set_hardware_sync(false);
    CHECK(d.amp == 0);
    CHECK(d.antenna == 0);
    CHECK(d.clkout == 0);
    CHECK(d.hw_sync == 0);

    const std::vector<std::pair<std::string, std::function<void()>>> calls = {
        {"hackrf_set_freq", [&] { board.set_frequency(1); }},
        {"hackrf_set_sample_rate", [&] { board.set_sample_rate(2e6); }},
        {"hackrf_set_baseband_filter_bandwidth", [&] { board.set_bandwidth(1'750'000); }},
        {"hackrf_set_amp_enable", [&] { board.set_amp(true); }},
        {"hackrf_set_lna_gain", [&] { board.set_lna_gain(8); }},
        {"hackrf_set_vga_gain", [&] { board.set_vga_gain(8); }},
        {"hackrf_set_txvga_gain", [&] { board.set_tx_vga_gain(8); }},
        {"hackrf_set_antenna_enable", [&] { board.set_bias_tee(true); }},
        {"hackrf_set_clkout_enable", [&] { board.set_clock_out(true); }},
        {"hackrf_set_hw_sync_mode", [&] { board.set_hardware_sync(true); }},
        {"hackrf_set_tx_underrun_limit", [&] { board.set_tx_underrun_limit(10); }},
    };
    for (const auto& [function, call] : calls) {
        CAPTURE(function);
        Bus::fail_next(function);
        CHECK(throws_naming(call, "HACKRF_ERROR_LIBUSB"));
    }
}

TEST_CASE("the transmit underrun limit goes to libhackrf in bytes, at most its range", "[hackrf]") {
    const Bus bus;
    HackrfBoard board(kRx);
    CHECK(board.set_tx_underrun_limit(1000));
    CHECK(Bus::rx().tx_underrun_limit == 2000);
    CHECK(board.set_tx_underrun_limit(UINT32_MAX));
    CHECK(Bus::rx().tx_underrun_limit == UINT32_MAX);
}

TEST_CASE("a reference switch is seen by board id: PLL B before r9, the PLL A multiplier from",
          "[hackrf]") {
    const Bus bus;
    // Before r9: on CLKIN when CLK0 takes PLL B.
    {
        HackrfBoard board(kRx);
        Bus::rx().clkin = 1;
        CHECK(board.start_switches_reference());
        Bus::rx().si5351[16] = 0x20;
        CHECK_FALSE(board.start_switches_reference());
        Bus::rx().clkin = 0;
        CHECK(board.start_switches_reference());
        Bus::fail_next("hackrf_si5351c_read");
        CHECK(throws_naming([&] { board.start_switches_reference(); }, "read clock control"));
        Bus::fail_next("hackrf_get_clkin_status");
        CHECK(throws_naming([&] { board.start_switches_reference(); }, "clkin status"));
    }
    // From r9: on CLKIN when PLL A's multiplier is set for 10 MHz in.
    Bus::rx().board_id = BOARD_ID_HACKRF1_R9;
    Bus::rx().si5351.clear();
    HackrfBoard board(kRx);
    Bus::rx().clkin = 1;
    CHECK(board.start_switches_reference());
    Bus::rx().si5351[29] = 0x26;
    CHECK_FALSE(board.start_switches_reference());
    Bus::fail_next("hackrf_si5351c_read");
    CHECK(throws_naming([&] { board.start_switches_reference(); }, "read pll multiplier"));
}

TEST_CASE("a receiving board hands each transfer's data to its handler", "[hackrf]") {
    const Bus bus;
    HackrfBoard board(kRx);
    std::vector<std::vector<std::uint8_t>> got;
    board.start_rx(
        [&](std::span<const std::uint8_t> bytes) { got.emplace_back(bytes.begin(), bytes.end()); });
    CHECK(board.streaming());
    CHECK(Bus::rx().mode == 1);
    std::vector<std::uint8_t> transfer = {1, 2, 3, 4, 5, 6, 7, 8};
    CHECK(fake::deliver_rx(kRx, transfer, 6) == 0);
    // A transfer with nothing in it is not passed on.
    CHECK(fake::deliver_rx(kRx, transfer, 0) == 0);
    REQUIRE(got.size() == 1);
    CHECK(got[0] == std::vector<std::uint8_t>{1, 2, 3, 4, 5, 6});
    CHECK(throws_naming([&] { board.start_rx([](auto) {}); }, "already streaming"));
    CHECK(throws_naming([&] { board.start_tx([](auto) {}); }, "already streaming"));
    board.stop();
    CHECK_FALSE(board.streaming());
    CHECK(Bus::rx().mode == 0);
    // Stopping a board that does not stream does nothing.
    const std::size_t calls = fake::state().calls.size();
    board.stop();
    CHECK(fake::state().calls.size() == calls);
}

TEST_CASE("a transmitting board fills each whole transfer from its handler", "[hackrf]") {
    const Bus bus;
    HackrfBoard board(kRx);
    board.start_tx([](std::span<std::uint8_t> bytes) {
        for (std::uint8_t& b : bytes) {
            b = 7;
        }
    });
    CHECK(Bus::rx().mode == 2);
    std::vector<std::uint8_t> buffer(16, 0);
    int valid = 0;
    CHECK(fake::pull_tx(kRx, buffer, valid) == 0);
    CHECK(valid == 16);
    CHECK(buffer == std::vector<std::uint8_t>(16, 7));
    board.stop();
}

TEST_CASE("a start whose transfers do not run is tried once more on a fresh handle", "[hackrf]") {
    const Bus bus;
    HackrfBoard board(kRx);
    Bus::rx().dead_starts = 1;
    board.start_rx([](auto) {});
    CHECK(board.streaming());
    CHECK(Bus::rx().opens == 2);
    CHECK(Bus::rx().closes == 1);
    board.stop();

    // Twice dead: the start fails, naming libhackrf's account.
    Bus::rx().dead_starts = 2;
    CHECK(throws_naming([&] { board.start_rx([](auto) {}); },
                        "start rx: board did not start streaming "
                        "(HACKRF_ERROR_STREAMING_THREAD_ERR)"));
    CHECK_FALSE(board.streaming());
}

TEST_CASE("a reopen that fails leaves no handle until the next start opens one", "[hackrf]") {
    const Bus bus;
    HackrfBoard board(kRx);
    Bus::rx().dead_starts = 1;
    Bus::fail_next("hackrf_open_by_serial");
    CHECK(throws_naming([&] { board.start_rx([](auto) {}); }, "open " + kRx));
    // Nothing is handed to libhackrf without a handle.
    CHECK(throws_naming([&] { board.set_frequency(1); }, "no handle"));
    CHECK_FALSE(board.streaming());
    CHECK_NOTHROW(board.stop());
    // The next start opens the board again.
    board.start_rx([](auto) {});
    CHECK(board.streaming());
    CHECK_NOTHROW(board.set_frequency(1));
    board.stop();
}

TEST_CASE("a start is done only once the streaming engine has taken it up", "[hackrf]") {
    const Bus bus;
    HackrfBoard board(kRx);
    // The last stream left counts behind; a start the engine has not taken
    // up still shows them.
    Bus::rx().m0.num_shortfalls = 1477;
    Bus::rx().m0_follows = false;
    const auto start = std::chrono::steady_clock::now();
    CHECK(throws_naming([&] { board.start_rx([](auto) {}); },
                        "start rx: the board's streaming engine did not start within 500 ms"));
    CHECK(std::chrono::steady_clock::now() - start >= std::chrono::milliseconds(500));
    CHECK_FALSE(board.streaming());
    CHECK(Bus::rx().mode == 0);

    // Idle again on counts the start reset: taken up, and stopped since.
    Bus::rx().m0_follows = true;
    board.start_rx([](auto) {});
    board.stop();
    Bus::rx().m0_follows = false;
    Bus::rx().m0.num_shortfalls = 3;
    Bus::fail_next("hackrf_start_rx");
    CHECK(throws_naming([&] { board.start_rx([](auto) {}); }, "start rx: HACKRF_ERROR_LIBUSB"));
    Bus::rx().m0 = hackrf_m0_state{};
    // A reading that cannot be had fails the start.
    Bus::fail_next("hackrf_get_m0_state");
    CHECK(throws_naming([&] { board.start_rx([](auto) {}); }, "read m0 state"));
}

TEST_CASE("a stop is done only once the streaming engine is idle", "[hackrf]") {
    const Bus bus;
    HackrfBoard board(kRx);
    board.start_tx([](auto) {});
    Bus::rx().m0_follows = false;
    CHECK(throws_naming([&] { board.stop(); }, "did not go idle within 500 ms"));
    // libhackrf has stopped the transfers either way.
    CHECK_FALSE(board.streaming());
}

TEST_CASE("a stop fails only when neither direction would stop", "[hackrf]") {
    const Bus bus;
    HackrfBoard board(kRx);
    board.start_rx([](auto) {});
    Bus::fail_next("hackrf_stop_rx");
    CHECK_NOTHROW(board.stop());
    board.start_rx([](auto) {});
    Bus::fail_next("hackrf_stop_rx");
    Bus::fail_next("hackrf_stop_tx");
    // The board was put to idle by hand: only the calls failed.
    Bus::rx().m0.active_mode = 0;
    CHECK(throws_naming([&] { board.stop(); }, "stop: HACKRF_ERROR_LIBUSB"));
    CHECK_FALSE(board.streaming());
}

TEST_CASE("the streaming engine's counts and error are the stream's health", "[hackrf]") {
    const Bus bus;
    HackrfBoard board(kRx);
    board.start_rx([](auto) {});
    Bus::rx().m0.num_shortfalls = 12;
    StreamHealth health = board.stream_health();
    CHECK(health.shortfalls == 12);
    CHECK_FALSE(health.stopped_itself);
    Bus::rx().m0.error = 1;
    health = board.stream_health();
    CHECK(health.stopped_itself);
    Bus::fail_next("hackrf_get_m0_state");
    CHECK(throws_naming([&] { board.stream_health(); }, "read m0 state"));
    Bus::rx().m0.error = 0;
    board.stop();
}

TEST_CASE("a board going out of scope stops and closes, whatever the stop says", "[hackrf]") {
    const Bus bus;
    {
        HackrfBoard board(kRx);
        board.start_rx([](auto) {});
        Bus::rx().m0_follows = false;
    }
    CHECK_FALSE(Bus::rx().open);
    CHECK(Bus::rx().closes == 1);
    CHECK(Bus::rx().mode == 0);
}
