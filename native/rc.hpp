// G0D SZIP — adaptive binary range coder (carryless, 11-bit probabilities).
// Original implementation for SuperZip; no third-party code.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace szip {

using Prob = uint16_t;

constexpr uint32_t kProbInit = 1024; // 50% prior
constexpr uint32_t kProbBits = 11;
constexpr uint32_t kProbMoveBits = 5;
constexpr uint32_t kTop = 1u << 24;

class RcEncoder {
public:
  explicit RcEncoder(std::vector<uint8_t> &out) : out_(out) {}

  uint64_t bits = 0;

  void encode_bit(Prob &p, uint32_t bit) {
    ++bits;
    const uint32_t bound = (range_ >> kProbBits) * static_cast<uint32_t>(p);
    if (bit == 0) {
      range_ = bound;
      p = static_cast<Prob>(p + ((2048u - p) >> kProbMoveBits));
    } else {
      low_ += bound;
      range_ -= bound;
      p = static_cast<Prob>(p - (p >> kProbMoveBits));
    }
    while (range_ < kTop) {
      range_ <<= 8;
      shift_low();
    }
  }

  void flush() {
    for (int i = 0; i < 5; ++i) shift_low();
  }

  // Encode with an external probability (no prob update); p in [1, 2047]
  // is P(bit == 0).
  void encode_bit_p(uint32_t p, uint32_t bit) {
    ++bits;
    const uint32_t bound = (range_ >> kProbBits) * p;
    if (bit == 0) {
      range_ = bound;
    } else {
      low_ += bound;
      range_ -= bound;
    }
    while (range_ < kTop) {
      range_ <<= 8;
      shift_low();
    }
  }

private:
  void shift_low() {
    const uint32_t high = static_cast<uint32_t>(low_ >> 32);
    if (static_cast<uint32_t>(low_) < 0xFF000000u || high != 0) {
      uint8_t temp = cache_;
      do {
        out_.push_back(static_cast<uint8_t>(temp + static_cast<uint8_t>(high)));
        temp = 0xFF;
      } while (--cache_size_ != 0);
      cache_ = static_cast<uint8_t>(static_cast<uint32_t>(low_) >> 24);
    }
    ++cache_size_;
    low_ = static_cast<uint64_t>(static_cast<uint32_t>(low_) << 8);
  }

  std::vector<uint8_t> &out_;
  uint64_t low_ = 0;
  uint32_t range_ = 0xFFFFFFFFu;
  uint8_t cache_ = 0;
  uint64_t cache_size_ = 1;
};

class RcDecoder {
public:
  RcDecoder(const uint8_t *in, size_t n) : in_(in), in_size_(n) {}

  // Reads the 5-byte prologue (first byte must be 0). False on truncation.
  bool init() {
    if (in_size_ < 5) return false;
    if (in_[0] != 0) return false;
    in_pos_ = 1;
    code_ = 0;
    for (int i = 0; i < 4; ++i) code_ = (code_ << 8) | next_byte();
    return ok_;
  }

  uint32_t decode_bit(Prob &p) {
    const uint32_t bound = (range_ >> kProbBits) * static_cast<uint32_t>(p);
    uint32_t bit;
    if (code_ < bound) {
      range_ = bound;
      p = static_cast<Prob>(p + ((2048u - p) >> kProbMoveBits));
      bit = 0;
    } else {
      code_ -= bound;
      range_ -= bound;
      p = static_cast<Prob>(p - (p >> kProbMoveBits));
      bit = 1;
    }
    while (range_ < kTop) {
      range_ <<= 8;
      code_ = (code_ << 8) | next_byte();
    }
    return bit;
  }

  // Decode with an external probability (no prob update); p in [1, 2047]
  // is P(bit == 0).
  uint32_t decode_bit_p(uint32_t p) {
    const uint32_t bound = (range_ >> kProbBits) * p;
    uint32_t bit;
    if (code_ < bound) {
      range_ = bound;
      bit = 0;
    } else {
      code_ -= bound;
      range_ -= bound;
      bit = 1;
    }
    while (range_ < kTop) {
      range_ <<= 8;
      code_ = (code_ << 8) | next_byte();
    }
    return bit;
  }

  bool ok() const { return ok_; }
  size_t pos() const { return in_pos_; }

private:
  uint8_t next_byte() {
    if (in_pos_ >= in_size_) {
      ok_ = false;
      return 0;
    }
    return in_[in_pos_++];
  }

  const uint8_t *in_;
  size_t in_size_;
  size_t in_pos_ = 0;
  uint32_t range_ = 0xFFFFFFFFu;
  uint32_t code_ = 0;
  bool ok_ = true;
};

} // namespace szip
