// G0D SZIP — BWT block codec (block type 2).  Original implementation.
// Cyclic-doubling suffix array over rotations + candidate entropy coders:
//   coder 0: raw BWT columns through the context mixer (MixLit)
//   coder 1: MTF ranks through the mixer
//   coder 2: raw BWT columns, gap-mode mixer      3: MTF ranks, gap-mode
// The encoder emits the smallest payload; the payload id selects the coder.
#pragma once

#include <cstdint>
#include <cstring>
#include <vector>

#include "mix.hpp"
#include "rc.hpp"

namespace szip {
namespace bwt {

// ------------------------------------------------- suffix array (doubling)
// Cyclic doubling over ROTATIONS (the Burrows-Wheeler order).  Suffix order
// with a virtual end-of-string sentinel is NOT equivalent for bordered
// strings (e.g. "0" vs "00"); the rotation order is what BWT needs.
static void build_sa(const uint8_t *s, uint32_t n, std::vector<uint32_t> &sa) {
  sa.clear();
  if (n == 0) return;
  sa.resize(n);
  std::vector<uint32_t> rank(n), next_rank(n);
  for (uint32_t i = 0; i < n; ++i) {
    sa[i] = i;
    rank[i] = (uint32_t)s[i] + 1; // 1..256
  }
  for (uint32_t k = 1;; k *= 2) {
    // stable counting sort by (rank[i], rank[(i+k)%n]): 2nd key then 1st
    auto key2 = [&](uint32_t i) {
      return rank[(i + k) % n];
    };
    uint32_t m = 0;
    for (uint32_t i = 0; i < n; ++i) {
      if (rank[i] > m) m = rank[i];
      if (key2(i) > m) m = key2(i);
    }
    std::vector<uint32_t> cnt(m + 1, 0);
    for (uint32_t i = 0; i < n; ++i) ++cnt[key2(i)];
    for (uint32_t v = 1; v <= m; ++v) cnt[v] += cnt[v - 1];
    std::vector<uint32_t> order(n);
    for (uint32_t i = n; i-- > 0;) order[--cnt[key2(i)]] = i;

    cnt.assign(m + 1, 0);
    for (uint32_t i = 0; i < n; ++i) ++cnt[rank[i]];
    for (uint32_t v = 1; v <= m; ++v) cnt[v] += cnt[v - 1];
    for (uint32_t i = n; i-- > 0;) {
      const uint32_t p = order[i];
      sa[--cnt[rank[p]]] = p;
    }
    // assign new ranks
    next_rank[sa[0]] = 1;
    uint32_t classes = 1;
    for (uint32_t i = 1; i < n; ++i) {
      const uint32_t a = sa[i - 1], b = sa[i];
      if (rank[a] != rank[b] || key2(a) != key2(b)) ++classes;
      next_rank[b] = classes;
    }
    rank.swap(next_rank);
    if (classes == n || k >= n) break;
  }
}

// ------------------------------------------------------------- BWT / inv
// bw[i] = s[(sa[i] + n - 1) % n]; primary = row with sa[i] == 0.
static void bwt_forward(const uint8_t *s, uint32_t n,
                        std::vector<uint8_t> &bw, uint32_t *primary) {
  std::vector<uint32_t> sa;
  build_sa(s, n, sa);
  bw.resize(n);
  for (uint32_t i = 0; i < n; ++i) {
    bw[i] = s[(sa[i] + n - 1) % n];
    if (sa[i] == 0) *primary = i;
  }
}

static int bwt_inverse(const uint8_t *bw, uint32_t n, uint32_t primary,
                       uint8_t *out) {
  if (primary >= n) return -1;
  uint32_t count[256] = {0};
  for (uint32_t i = 0; i < n; ++i) ++count[bw[i]];
  uint32_t first[256];
  uint32_t sum = 0;
  for (int c = 0; c < 256; ++c) {
    first[c] = sum;
    sum += count[c];
  }
  std::vector<uint32_t> next(n);
  uint32_t seen[256] = {0};
  for (uint32_t i = 0; i < n; ++i) {
    const uint8_t c = bw[i];
    next[i] = first[c] + seen[c]++;
  }
  uint32_t j = primary;
  for (uint32_t k = 0; k < n; ++k) {
    out[n - 1 - k] = bw[j];
    j = next[j];
  }
  return 0;
}

// ------------------------------------------------------------- coder 0 --
// raw BWT columns through the context mixer (byte stream semantics).
static void coder0_encode(const std::vector<uint8_t> &bw,
                          std::vector<uint8_t> &out) {
  MixLit mix(0);
  Hist hist;
  RcEncoder rc(out);
  for (uint8_t b : bw) {
    mix.encode(rc, hist, b, 0, 0); // pushes to hist internally
  }
  rc.flush();
}

static int coder0_decode(const uint8_t *in, uint32_t in_len,
                         std::vector<uint8_t> &bw, uint32_t n) {
  MixLit mix(0);
  Hist hist;
  RcDecoder rc(in, in_len);
  if (!rc.init()) return -1;
  bw.resize(n);
  for (uint32_t i = 0; i < n; ++i) {
    bw[i] = mix.decode(rc, hist, 0, 0); // caller must push (unlike encode)
    hist.push(bw[i]);
    if (!rc.ok()) return -1;
  }
  return 0;
}

// ------------------------------------------------------------- coder 1 --
// MTF ranks through the same context mixer (byte-stream semantics).
// MTF turns the BWT's character clustering into small ranks; the mixer's
// bigram/word contexts capture rank-run structure.  Encoder chooses the
// smaller of coder 0 (raw BWT columns) and coder 1 (MTF ranks).

static void mtf_forward(const std::vector<uint8_t> &bw,
                        std::vector<uint8_t> &ranks) {
  uint8_t list[256];
  for (uint32_t i = 0; i < 256; ++i) list[i] = (uint8_t)i;
  ranks.resize(bw.size());
  for (size_t i = 0; i < bw.size(); ++i) {
    const uint8_t b = bw[i];
    uint32_t r = 0;
    while (list[r] != b) ++r;
    for (uint32_t k = r; k > 0; --k) list[k] = list[k - 1];
    list[0] = b;
    ranks[i] = (uint8_t)r;
  }
}

static void mtf_inverse(const std::vector<uint8_t> &ranks,
                        std::vector<uint8_t> &bw) {
  uint8_t list[256];
  for (uint32_t k = 0; k < 256; ++k) list[k] = (uint8_t)k;
  bw.resize(ranks.size());
  for (size_t i = 0; i < ranks.size(); ++i) {
    const uint8_t b = list[ranks[i]];
    for (uint32_t k = ranks[i]; k > 0; --k) list[k] = list[k - 1];
    list[0] = b;
    bw[i] = b;
  }
}

static void coder1_encode(const std::vector<uint8_t> &bw,
                          std::vector<uint8_t> &out) {
  std::vector<uint8_t> ranks;
  mtf_forward(bw, ranks);
  MixLit mix(0);
  Hist hist;
  RcEncoder rc(out);
  for (uint8_t b : ranks) mix.encode(rc, hist, b, 0, 0);
  rc.flush();
}

static int coder1_decode(const uint8_t *in, uint32_t in_len,
                         std::vector<uint8_t> &bw, uint32_t n) {
  MixLit mix(0);
  Hist hist;
  RcDecoder rc(in, in_len);
  if (!rc.init()) return -1;
  std::vector<uint8_t> ranks(n);
  for (uint32_t i = 0; i < n; ++i) {
    ranks[i] = mix.decode(rc, hist, 0, 0); // caller must push (unlike encode)
    hist.push(ranks[i]);
    if (!rc.ok()) return -1;
  }
  mtf_inverse(ranks, bw);
  return 0;
}

// gap-mode variants (coder 2 = raw columns, coder 3 = MTF ranks)
static void coder2_encode(const std::vector<uint8_t> &bw,
                          std::vector<uint8_t> &out) {
  MixLit mix(1);
  Hist hist;
  RcEncoder rc(out);
  for (uint8_t b : bw) mix.encode(rc, hist, b, 0, 0);
  rc.flush();
}

static int coder2_decode(const uint8_t *in, uint32_t in_len,
                         std::vector<uint8_t> &bw, uint32_t n) {
  MixLit mix(1);
  Hist hist;
  RcDecoder rc(in, in_len);
  if (!rc.init()) return -1;
  bw.resize(n);
  for (uint32_t i = 0; i < n; ++i) {
    bw[i] = mix.decode(rc, hist, 0, 0);
    hist.push(bw[i]);
    if (!rc.ok()) return -1;
  }
  return 0;
}

static void coder3_encode(const std::vector<uint8_t> &bw,
                          std::vector<uint8_t> &out) {
  std::vector<uint8_t> ranks;
  mtf_forward(bw, ranks);
  MixLit mix(1);
  Hist hist;
  RcEncoder rc(out);
  for (uint8_t b : ranks) mix.encode(rc, hist, b, 0, 0);
  rc.flush();
}

static int coder3_decode(const uint8_t *in, uint32_t in_len,
                         std::vector<uint8_t> &bw, uint32_t n) {
  MixLit mix(1);
  Hist hist;
  RcDecoder rc(in, in_len);
  if (!rc.init()) return -1;
  std::vector<uint8_t> ranks(n);
  for (uint32_t i = 0; i < n; ++i) {
    ranks[i] = mix.decode(rc, hist, 0, 0);
    hist.push(ranks[i]);
    if (!rc.ok()) return -1;
  }
  mtf_inverse(ranks, bw);
  return 0;
}

// --------------------------------------------------------- block payload --
// payload = [u32 LE primary][u8 coder_id][RC stream]
inline void encode_block(const uint8_t *src, uint32_t n,
                         std::vector<uint8_t> &out) {
  out.clear();
  if (n == 0) return;
  if (n == 1) {
    out.push_back(0); // coder 0 degenerate: stream is 1 byte
    out.push_back(src[0]);
    return;
  }
  std::vector<uint8_t> bw;
  uint32_t primary = 0;
  bwt_forward(src, n, bw, &primary);

  std::vector<uint8_t> c[4];
  coder0_encode(bw, c[0]);
  coder1_encode(bw, c[1]);
  coder2_encode(bw, c[2]);
  coder3_encode(bw, c[3]);
  uint32_t best = 0;
  for (uint32_t k = 1; k < 4; ++k)
    if (c[k].size() < c[best].size()) best = k;

  out.push_back((uint8_t)(primary & 0xFF));
  out.push_back((uint8_t)((primary >> 8) & 0xFF));
  out.push_back((uint8_t)((primary >> 16) & 0xFF));
  out.push_back((uint8_t)((primary >> 24) & 0xFF));
  out.push_back((uint8_t)best);
  out.insert(out.end(), c[best].begin(), c[best].end());
}

inline int decode_block(const uint8_t *src, uint32_t src_len, uint8_t *dst,
                        uint32_t n) {
  if (n == 0) return 0;
  if (n == 1) {
    if (src_len != 2 || src[0] != 0) return -1;
    dst[0] = src[1];
    return 0;
  }
  if (src_len < 5) return -1;
  const uint32_t primary = (uint32_t)src[0] | ((uint32_t)src[1] << 8) |
                           ((uint32_t)src[2] << 16) | ((uint32_t)src[3] << 24);
  const uint8_t coder = src[4];
  std::vector<uint8_t> bw;
  int e;
  switch (coder) {
  case 0: e = coder0_decode(src + 5, src_len - 5, bw, n); break;
  case 1: e = coder1_decode(src + 5, src_len - 5, bw, n); break;
  case 2: e = coder2_decode(src + 5, src_len - 5, bw, n); break;
  case 3: e = coder3_decode(src + 5, src_len - 5, bw, n); break;
  default: return -1;
  }
  if (e) return e;
  return bwt_inverse(bw.data(), n, primary, dst);
}

} // namespace bwt
} // namespace szip
