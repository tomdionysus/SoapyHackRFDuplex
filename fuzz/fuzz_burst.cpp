// SPDX-License-Identifier: GPL-3.0-or-later
//
// The calibration burst's search, over whatever the receiver hands it:
// the whole-capture search and the one that runs as the capture fills,
// fed in pieces of any size. Where both find the burst, they agree.

#include <fuzzer/FuzzedDataProvider.h>

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <span>
#include <vector>

#include "burst.hpp"

using namespace hackrfduplex;

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    FuzzedDataProvider in(data, size);
    const std::size_t piece = in.ConsumeIntegralInRange<std::size_t>(1, 70000);
    // A burst somewhere in it, now and then, so the search has one to find.
    const bool with_burst = in.ConsumeBool();
    const std::vector<std::uint8_t> rest = in.ConsumeRemainingBytes<std::uint8_t>();
    std::vector<std::int8_t> iq(rest.begin(), rest.end());
    iq.resize(iq.size() & ~std::size_t{1});
    if (with_burst) {
        const std::vector<std::int8_t> burst = burst::samples();
        const std::size_t at = iq.empty() ? 0 : (iq.size() / 2) & ~std::size_t{1};
        iq.insert(iq.begin() + static_cast<std::ptrdiff_t>(at), burst.begin(), burst.end());
    }

    const burst::Found whole = burst::find(iq);
    burst::Finder finder;
    std::size_t held = 0;
    while (held < iq.size()) {
        held = std::min(iq.size(), held + 2 * piece);
        finder.advance(std::span<const std::int8_t>(iq.data(), held));
    }
    finder.finish(iq);
    const burst::Found grown = finder.result();
    if (whole.found && (whole.offset + burst::kChips) * 2 > iq.size()) {
        std::abort();
    }
    if (grown.found && (grown.offset + burst::kChips) * 2 > iq.size()) {
        std::abort();
    }
    return 0;
}
