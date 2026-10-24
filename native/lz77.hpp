// G0D SZIP — LZ77 hash-chain match finder. Original implementation.
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdint>
#include <vector>

#include "szip.h"

namespace szip {

constexpr uint32_t kMinMatch = SZIP_MIN_MATCH;   // 3
constexpr uint32_t kMaxMatch = SZIP_MAX_MATCH; // 2048
constexpr uint32_t kWindowLog = 20;              // 1 MiB
constexpr uint32_t kWindow = 1u << kWindowLog;
constexpr uint32_t kHashBits = 18;
constexpr uint32_t kHashSize = 1u << kHashBits;
#ifndef SZ_CHAIN
#define SZ_CHAIN 400
#endif
#ifndef SZ_NICE
#define SZ_NICE 128
#endif
constexpr uint32_t kMaxChain = SZ_CHAIN;
constexpr uint32_t kNiceLen = SZ_NICE;

struct Match {
  uint32_t len = 0;
  uint32_t off = 0; // 1-based
};

class MatchFinder {
public:
  // prev_ is indexed modulo the block size; call prepare() once per block.
  void prepare(uint32_t block_len) {
    head_.assign(kHashSize, 0);
    prev_.assign(block_len ? block_len : 1, 0);
  }

  void insert(const uint8_t *data, uint32_t n, uint32_t pos) {
    if (pos + kMinMatch > n) return;
    const uint32_t h = hash3(data + pos);
    prev_[pos % prev_.size()] = head_[h];
    head_[h] = pos + 1; // store pos+1 so 0 means empty
  }

  Match find(const uint8_t *data, uint32_t n, uint32_t pos) const {
    Match best;
    int64_t best_score = INT64_MIN;
    if (pos + kMinMatch > n) return best;

    const uint32_t max_len = std::min<uint32_t>(kMaxMatch, n - pos);
    const uint32_t limit =
        (pos > kWindow) ? pos - kWindow : 0;

    uint32_t cand = head_[hash3(data + pos)];
    for (uint32_t depth = 0; depth < kMaxChain && cand != 0; ++depth) {
      const uint32_t mpos = cand - 1;
      if (mpos < limit) break;
      if (mpos >= pos) {
        // current position (or lazy look-ahead) is in the chain: skip it
        cand = prev_[mpos % prev_.size()];
        continue;
      }

      // cheap reject: first 3 bytes must match
      if (data[mpos] != data[pos] || data[mpos + 1] != data[pos + 1] ||
          data[mpos + 2] != data[pos + 2]) {
        cand = prev_[mpos % prev_.size()];
        continue;
      }

      uint32_t len = 3;
      while (len < max_len && data[mpos + len] == data[pos + len]) ++len;

      // price-aware: prefer longer matches, but a much nearer match can win
      const uint32_t off = pos - mpos;
      uint32_t bl = 0;
      for (uint32_t t = off; t; t >>= 1) ++bl;
      const int64_t score = (int64_t)len * 8 - (int64_t)bl * 3;
      if (best.len == 0 || score > best_score) {
        best.len = len;
        best.off = off;
        best_score = score;
      }
      if (len >= kNiceLen || len == max_len) break;
      cand = prev_[mpos % prev_.size()];
    }
    return best;
  }

private:
  static uint32_t hash3(const uint8_t *p) {
    const uint32_t v = static_cast<uint32_t>(p[0]) |
                       (static_cast<uint32_t>(p[1]) << 8) |
                       (static_cast<uint32_t>(p[2]) << 16);
    return (v * 2654435761u) >> (32 - kHashBits);
  }

  std::vector<uint32_t> head_;
  std::vector<uint32_t> prev_;
};

} // namespace szip
