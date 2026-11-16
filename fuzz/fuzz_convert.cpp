// SPDX-License-Identifier: GPL-3.0-or-later
//
// The sample conversions every read and write runs, over any samples in
// any format: an application's floats may be anything, NaN and infinity
// included. Native samples survive a round trip through every format.

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <vector>

#include "convert.hpp"

using namespace hackrfduplex;

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    if (size < 1) {
        return 0;
    }
    const Format formats[] = {Format::kCs8, Format::kCs16, Format::kCf32, Format::kCf64};
    const Format format = formats[data[0] % 4];
    ++data;
    --size;

    // The bytes as an application's samples in that format, into native.
    const std::size_t width = sample_bytes(format);
    const std::size_t samples = size / width;
    std::vector<std::uint8_t> raw(data, data + samples * width);
    std::vector<std::int8_t> native(2 * samples);
    to_native(raw.data(), format, native.data(), samples);

    // The bytes as native samples, out to the format and back unchanged.
    const std::size_t pairs = size / 2;
    std::vector<std::int8_t> board(data, data + pairs * 2);
    std::vector<std::uint8_t> out(pairs * width);
    from_native(board.data(), format, out.data(), pairs);
    std::vector<std::int8_t> back(pairs * 2);
    to_native(out.data(), format, back.data(), pairs);
    if (back != board) {
        std::abort();
    }
    return 0;
}
