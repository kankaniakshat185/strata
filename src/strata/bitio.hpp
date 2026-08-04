#pragma once

#include <cassert>
#include <cstdint>
#include <vector>

// MSB-first bit-level I/O over a byte buffer. This is the primitive the
// Gorilla encoder/decoder (gorilla.hpp) is built on: everything there is
// expressed as "write N bits" / "read N bits", not raw bytes.
namespace strata {

class BitWriter {
 public:
  void WriteBit(bool bit) {
    cur_ = uint8_t((cur_ << 1) | (bit ? 1 : 0));
    if (++cur_bits_ == 8) {
      buf_.push_back(cur_);
      cur_ = 0;
      cur_bits_ = 0;
    }
  }

  // Writes the low `nbits` bits of `value`, most significant first.
  void WriteBits(uint64_t value, int nbits) {
    for (int i = nbits - 1; i >= 0; --i) {
      WriteBit((value >> i) & 1u);
    }
  }

  // Pads the final byte with zero bits and returns the buffer. Call once,
  // after all bits for this stream have been written.
  const std::vector<uint8_t>& Finish() {
    if (cur_bits_ > 0) {
      cur_ = uint8_t(cur_ << (8 - cur_bits_));
      buf_.push_back(cur_);
      cur_ = 0;
      cur_bits_ = 0;
    }
    return buf_;
  }

 private:
  std::vector<uint8_t> buf_;
  uint8_t cur_ = 0;
  int cur_bits_ = 0;  // bits already placed into cur_ (0-7)
};

class BitReader {
 public:
  BitReader(const uint8_t* data, size_t byte_len)
      : data_(data), byte_len_(byte_len) {}

  bool ReadBit() {
    assert(byte_pos_ < byte_len_ && "BitReader: read past end of buffer");
    uint8_t byte = data_[byte_pos_];
    bool bit = (byte >> (7 - bit_pos_)) & 1u;
    if (++bit_pos_ == 8) {
      bit_pos_ = 0;
      ++byte_pos_;
    }
    return bit;
  }

  uint64_t ReadBits(int nbits) {
    uint64_t v = 0;
    for (int i = 0; i < nbits; ++i) {
      v = (v << 1) | (ReadBit() ? 1u : 0u);
    }
    return v;
  }

 private:
  const uint8_t* data_;
  size_t byte_len_;
  size_t byte_pos_ = 0;
  int bit_pos_ = 0;  // next bit to read within data_[byte_pos_], MSB first
};

}  // namespace strata
