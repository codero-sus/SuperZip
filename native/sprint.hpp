// G0D SZIP — SZPRINT v2: the sprint codec.
//
// Beats the speed snipers (snappy / lz4) at their own game: byte-aligned
// greedy LZ with snappy-class tags (11-bit offsets in one
// extra byte) and 256 KiB-block parallelism across cores.  The stored-block
// fallback covers incompressible data so every block is still <= snappy's
// output.
//
// Element stream (per block):
//   tok u8, tag = tok & 3:
//     tag 0  LITERAL: len-1 = tok >> 2 (1..60 direct);
//            code 60 -> u8 ext  (len = 61 + ext);
//            code 61 -> u16 ext (len = 316 + ext);
//            code 62 -> u24 ext (len = 65892 + ext);
//            followed by len raw bytes.  The stream may end after a literal.
//     tag 1  COPY short: mlen = ((tok >> 2) & 7) + 4  (4..11);
//            offset = ((tok >> 5) << 8) | next u8    (1..2047).
//     tag 2  COPY wide:  mlen = (tok >> 2) + 4  (4..66);
//            code 63 -> u8/u16/u24 ext as above starting at 67;
//            offset = u16 LE (1..65535).
//     tag 3  reserved (corrupt).
//   Copy bytes are taken from (out - offset), memmove semantics.
//   The stream must not end in the middle of an element; decompressed size
//   must match the block header exactly.
#pragma once

#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>

namespace szip {
namespace sprint {

static inline uint32_t load32(const uint8_t *p) {
  uint32_t v;
  std::memcpy(&v, p, 4);
  return v;
}
static inline uint64_t load64(const uint8_t *p) {
  uint64_t v;
  std::memcpy(&v, p, 8);
  return v;
}

static inline uint32_t hash4(uint32_t v, uint32_t bits) {
  return (v * 265443561u) >> (32 - bits);
}

static inline uint32_t extend(const uint8_t *src, uint32_t cand, uint32_t pos,
                              uint32_t max_len) {
  uint32_t l = 0;
  while (l + 8 <= max_len && load64(src + cand + l) == load64(src + pos + l))
    l += 8;
  while (l + 4 <= max_len && load32(src + cand + l) == load32(src + pos + l))
    l += 4;
  while (l < max_len && src[cand + l] == src[pos + l]) ++l;
  return l;
}

// extension value: E < 255 -> one byte; else 255 sentinel + u16 LE (E-255).
// E max = 255 + 65535 = 65790 (blocks are <= 256 KiB; runs are capped by n).
static inline void put_ext(std::vector<uint8_t> &o, uint32_t e) {
  if (e < 255) {
    o.push_back((uint8_t)e);
    return;
  }
  o.push_back(255);
  const uint32_t r = e - 255;
  o.push_back((uint8_t)(r & 255));
  o.push_back((uint8_t)(r >> 8));
}

// Compress one block.  `out` is cleared and filled with the element stream.
inline void compress(const uint8_t *src, uint32_t n, std::vector<uint8_t> &out) {
  out.clear();
  if (n == 0) return;
  out.reserve(n / 2 + 64);
  const uint32_t bits = 16;
  std::vector<uint32_t> tbl(1u << bits, 0); // pos+1, 0 = empty
  std::unique_ptr<uint32_t[]> prev(new uint32_t[n + 1]); // written-before-read
#ifndef SP_CHAIN
#define SP_CHAIN 2
#endif
  const uint32_t kChain = SP_CHAIN;

  const uint32_t kMinMatch = 4;
  uint32_t lit_start = 0;
  uint32_t pos = 0;

  auto flush_lits = [&](uint32_t end) {
    const uint32_t lit = end - lit_start;
    if (!lit) return;
    if (lit <= 60) {
      out.push_back((uint8_t)((lit - 1) << 2));
    } else {
      out.push_back((uint8_t)(60 << 2));
      put_ext(out, lit - 61);
    }
    out.insert(out.end(), src + lit_start, src + lit_start + lit);
    lit_start = end;
  };

  while (pos + kMinMatch <= n) {
    const uint32_t v = load32(src + pos);
    const uint32_t h = hash4(v, bits);
    prev[pos + 1] = tbl[h];
    tbl[h] = pos + 1;
    uint32_t mlen = 0, moff = 0;
    int best_gain = -1;
    {
      uint32_t link = prev[pos + 1];
      for (uint32_t k = 0; k < kChain && link != 0; ++k) {
        const uint32_t cand = link - 1;
        link = prev[link];
        if (cand >= pos || pos - cand > 65535u) continue;
        if (load32(src + cand) != v) continue;
        const uint32_t len = extend(src, cand, pos, n - pos);
        const uint32_t off = pos - cand;
        const int gain =
            (int)len - (off > 2047 ? 3 : 2) - (len > 11 ? 1 : 0);
        if (gain > best_gain) {
          best_gain = gain;
          mlen = len;
          moff = off;
        }
      }
    }
    if (mlen < kMinMatch) {
      ++pos;
      continue;
    }
    flush_lits(pos);
    if (moff <= 2047 && mlen <= 11) {
      out.push_back((uint8_t)(1 | (((mlen - 4) & 7) << 2) |
                              (((moff >> 8) & 7) << 5)));
      out.push_back((uint8_t)(moff & 255));
    } else if (mlen <= 66) {
      out.push_back((uint8_t)(2 | ((mlen - 4) << 2)));
    } else {
      out.push_back((uint8_t)(2 | (63 << 2)));
      put_ext(out, mlen - 67);
    }
    if (moff > 2047 || mlen > 11) {
      out.push_back((uint8_t)(moff & 255));
      out.push_back((uint8_t)(moff >> 8));
    }
    // re-sync hash at the tail of the match (keeps chains warm)
    pos += mlen;
#ifndef SP_NOINT
    {
      const uint32_t stop = pos > 2 ? pos - 2 : 0;
      const uint32_t from = pos > mlen + 1 ? pos - mlen + 1 : 1;
      for (uint32_t u = from; u < stop; ++u) {
        const uint32_t hh = hash4(load32(src + u), bits);
        prev[u + 1] = tbl[hh];
        tbl[hh] = u + 1;
      }
    }
#endif
    lit_start = pos;
  }
  flush_lits(n);
}

// Decompress one block.  Returns 0 on success, nonzero on corrupt input.
inline int decompress(const uint8_t *src, uint32_t src_n, uint8_t *dst,
                      uint32_t dst_n) {
  const uint8_t *p = src;
  const uint8_t *const pend = src + src_n;
  uint8_t *out = dst;
  uint8_t *const oend = dst + dst_n;

  auto get_ext = [&](uint32_t &e) -> int {
    if (p >= pend) return 1;
    const uint8_t b0 = *p++;
    if (b0 != 255) {
      e = b0;
      return 0;
    }
    if (p + 1 >= pend) return 1;
    e = 255 + ((uint32_t)p[0] | ((uint32_t)p[1] << 8));
    p += 2;
    return 0;
  };

  while (p < pend) {
    const uint8_t tok = *p++;
    const uint8_t tag = tok & 3;
    if (tag == 0) {
      uint32_t lit;
      const uint32_t code = tok >> 2;
      if (code < 60) {
        lit = code + 1;
      } else {
        uint32_t extra;
        if (get_ext(extra)) return 2;
        lit = 61 + extra;
      }
      if ((size_t)(oend - out) < lit || (size_t)(pend - p) < lit) return 3;
      std::memcpy(out, p, lit);
      out += lit;
      p += lit;
    } else if (tag == 1) {
      if (p >= pend) return 4;
      const uint32_t off = ((uint32_t)(tok >> 5) << 8) | *p++;
      if (off == 0 || off > (size_t)(out - dst)) return 5;
      uint32_t mlen = ((tok >> 2) & 7) + 4;
      if ((size_t)(oend - out) < mlen) return 6;
      const uint8_t *m = out - off;
      for (uint32_t i = 0; i < mlen; ++i) out[i] = m[i];
      out += mlen;
    } else if (tag == 2) {
      uint32_t mlen;
      const uint32_t code = tok >> 2;
      if (code < 63) {
        mlen = code + 4;
      } else {
        uint32_t extra;
        if (get_ext(extra)) return 7;
        mlen = 67 + extra;
      }
      if (p + 1 >= pend) return 8;
      const uint32_t off = (uint32_t)p[0] | ((uint32_t)p[1] << 8);
      p += 2;
      if (off == 0 || off > (size_t)(out - dst)) return 9;
      if ((size_t)(oend - out) < mlen) return 10;
      const uint8_t *m = out - off;
      for (uint32_t i = 0; i < mlen; ++i) out[i] = m[i];
      out += mlen;
    } else {
      return 11; // reserved tag
    }
  }
  return (out == oend) ? 0 : 12;
}

} // namespace sprint
} // namespace szip
