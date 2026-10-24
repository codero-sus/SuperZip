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

  // Up to kmax distinct-distance candidates, best-score first. Each entry
  // carries its own maximal length at that distance. kmax == 1 reproduces
  // find() exactly. Also force-includes the longest and nearest matches.
  uint32_t find_multi(const uint8_t *data, uint32_t n, uint32_t pos,
                      Match *out, uint32_t kmax) const {
    if (pos + kMinMatch > n) return 0;
    const uint32_t max_len = std::min<uint32_t>(kMaxMatch, n - pos);
    const uint32_t limit = (pos > kWindow) ? pos - kWindow : 0;

    Match all[kMaxChain];
    int64_t sc[kMaxChain];
    uint32_t cnt = 0;
    uint32_t longest_i = 0, nearest_i = 0;

    uint32_t cand = head_[hash3(data + pos)];
    for (uint32_t depth = 0; depth < kMaxChain && cand != 0; ++depth) {
      const uint32_t mpos = cand - 1;
      if (mpos < limit) break;
      if (mpos >= pos) {
        cand = prev_[mpos % prev_.size()];
        continue;
      }
      if (data[mpos] != data[pos] || data[mpos + 1] != data[pos + 1] ||
          data[mpos + 2] != data[pos + 2]) {
        cand = prev_[mpos % prev_.size()];
        continue;
      }
      uint32_t len = 3;
      while (len < max_len && data[mpos + len] == data[pos + len]) ++len;
      const uint32_t off = pos - mpos;
      uint32_t bl = 0;
      for (uint32_t t = off; t; t >>= 1) ++bl;
      const int64_t score = (int64_t)len * 8 - (int64_t)bl * 3;
      all[cnt] = {len, off};
      sc[cnt] = score;
      if (len > all[longest_i].len) longest_i = cnt;
      if (off < all[nearest_i].off) nearest_i = cnt;
      ++cnt;
      if ((len >= kNiceLen || len == max_len) && cnt >= kmax) break;
      cand = prev_[mpos % prev_.size()];
    }
    if (cnt == 0) return 0;

    // selection: top kmax by score (first-seen wins ties), then force-include
    // the longest and nearest candidates for diversity
    bool used[kMaxChain] = {false};
    uint32_t pick_order[2 + 32];
    uint32_t np = 0;
    for (uint32_t t = 0; t < cnt && np < kmax; ++t) {
      int64_t bs = INT64_MIN;
      uint32_t bi = UINT32_MAX;
      for (uint32_t i = 0; i < cnt; ++i)
        if (!used[i] && sc[i] > bs) { bs = sc[i]; bi = i; }
      if (bi == UINT32_MAX) break;
      used[bi] = true;
      pick_order[np++] = bi;
    }
    for (uint32_t extra : {longest_i, nearest_i}) {
      if (np >= kmax) break;
      if (!used[extra]) { used[extra] = true; pick_order[np++] = extra; }
    }
    // trim to kmax but keep the forced extras: drop the worst-score non-forced
    while (np > kmax) {
      uint32_t drop = 0;
      for (uint32_t j = 1; j < np; ++j)
        if (sc[pick_order[j]] < sc[pick_order[drop]]) drop = j;
      for (uint32_t j = drop; j + 1 < np; ++j) pick_order[j] = pick_order[j + 1];
      --np;
    }
    for (uint32_t j = 0; j < np; ++j) out[j] = all[pick_order[j]];
    return np;
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
