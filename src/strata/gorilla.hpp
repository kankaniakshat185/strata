#pragma once

#include <cstdint>
#include <utility>
#include <vector>

// Gorilla-style compression for one series' (timestamp, value) points, per
// the bit-encoding spec in STRATA_DESIGN.md: the first point is stored
// raw, every subsequent point as a delta-of-delta timestamp + XOR'd value.
namespace strata::gorilla {

// Encodes `points` (already sorted by timestamp) into a byte-padded
// bitstream.
std::vector<uint8_t> EncodeSeries(
    const std::vector<std::pair<int64_t, double>>& points);

// Decodes exactly `point_count` points back out of a bitstream produced by
// EncodeSeries. The caller must know point_count in advance (it comes from
// the block's series index) since the stream itself carries no count or
// length prefix.
std::vector<std::pair<int64_t, double>> DecodeSeries(const uint8_t* data,
                                                       size_t byte_len,
                                                       uint32_t point_count);

}  // namespace strata::gorilla
