// G0D SZIP — IEEE CRC-32 (poly 0xEDB88320). Original implementation.
#pragma once

#include <cstddef>
#include <cstdint>

namespace szip {

class Crc32 {
public:
  Crc32() {
    for (uint32_t i = 0; i < 256; ++i) {
      uint32_t c = i;
      for (int k = 0; k < 8; ++k)
        c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
      table_[i] = c;
    }
  }

  uint32_t update(uint32_t crc, const uint8_t *data, size_t n) const {
    crc ^= 0xFFFFFFFFu;
    for (size_t i = 0; i < n; ++i)
      crc = table_[(crc ^ data[i]) & 0xFFu] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFFu;
  }

private:
  uint32_t table_[256];
};

inline const Crc32 &crc32_table() {
  static const Crc32 table;
  return table;
}

} // namespace szip
