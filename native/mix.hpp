// G0D SZIP — integer context-mixing literal coder (SZLZRC v3 "GOD stage").
// Original implementation; deterministic fixed-point math only.
#pragma once

#include <cstdint>
#include <cstdlib>
#include <vector>
#include <mutex>
#include <sys/mman.h>
#include <unistd.h>
#include <new>
#if defined(__SSE2__)
#include <emmintrin.h>
#endif

// ---- speed: tree init dominates encoder "emit" wall (470 MB per MixLit).
// (1) SIMD fill for the u16 init pattern (libstdc++ std::fill is scalar),
// (2) thread-local block pool so repeated construct/destruct cycles reuse
//     warm pages instead of re-mapping 470 MB every time.  Exact: the bytes
//     written are identical to assign(n, kProbInit).
inline void szip_fill16(uint16_t *p, size_t n, uint16_t v) {
#if defined(__SSE2__)
  __m128i pat = _mm_set1_epi16((short)v);
  size_t i = 0;
  for (; i + 8 <= n; i += 8) _mm_storeu_si128((__m128i *)(p + i), pat);
  std::fill(p + i, p + n, v);
#else
  std::fill(p, p + n, v);
#endif
}

namespace poolalloc_detail {
struct Blk { void *p; size_t bytes; };
// One GLOBAL warm cache (mutex-guarded, hard cap ~512 MB): emit threads
// cycle construct/destruct sequentially, so a single recycled block per
// size class removes the re-fault cost without ballooning a 3.9 GB VM.
inline std::vector<Blk> &pool() {
  static std::vector<Blk> p;
  return p;
}
inline std::mutex &pmutex() {
  static std::mutex m;
  return m;
}
} // namespace poolalloc_detail

template <class T> struct PoolAlloc {
  using value_type = T;
  PoolAlloc() = default;
  template <class U> PoolAlloc(const PoolAlloc<U> &) {}
  T *allocate(size_t n) {
    const size_t kHP = (size_t)2u << 20;
    const size_t bytes = (n * sizeof(T) + kHP - 1) & ~(kHP - 1);
    {
      std::lock_guard<std::mutex> lk(poolalloc_detail::pmutex());
      auto &pool = poolalloc_detail::pool();
      for (size_t i = 0; i < pool.size(); ++i) {
        if (pool[i].bytes == bytes) {
          T *p = (T *)pool[i].p;
          pool[i] = pool.back();
          pool.pop_back();
          return p;
        }
      }
    }
    void *p = nullptr;
    if (posix_memalign(&p, kHP, bytes) != 0 || !p) {
      p = std::malloc(n * sizeof(T));
      if (!p) throw std::bad_alloc();
      return (T *)p;
    }
#if defined(MADV_HUGEPAGE)
    madvise(p, bytes, MADV_HUGEPAGE);
#endif
    return (T *)p;
  }
  void deallocate(T *p, size_t n) noexcept {
    const size_t kHP = (size_t)2u << 20;
    const size_t bytes = (n * sizeof(T) + kHP - 1) & ~(kHP - 1);
    std::lock_guard<std::mutex> lk(poolalloc_detail::pmutex());
    auto &pool = poolalloc_detail::pool();
    size_t total = 0;
    for (auto &b : pool) total += b.bytes;
    if (pool.size() < 2 && total + bytes <= (size_t)512u << 20) {
      pool.push_back({p, bytes});
      return;
    }
    std::free(p);
  }
};
template <class T, class U>
bool operator==(const PoolAlloc<T> &, const PoolAlloc<U> &) { return true; }
template <class T, class U>
bool operator!=(const PoolAlloc<T> &, const PoolAlloc<U> &) { return false; }
#include <vector>

#include "rc.hpp"
#include "tables.hpp"

namespace szip {

double g_enc_price_sum = 0;

#ifndef SZ_WSHIFT
#define SZ_WSHIFT 13
#endif
constexpr int32_t kWShift = SZ_WSHIFT;
#ifndef SZ_ERRSHIFT
#define SZ_ERRSHIFT 18
#endif
#ifndef SZ_WSHIFT_B
#define SZ_WSHIFT_B 13
#endif
#ifndef SZ_ERRSHIFT_B
#define SZ_ERRSHIFT_B 20
#endif
#ifndef SZ_TREESHIFT
#define SZ_TREESHIFT 5
#endif
constexpr int32_t kErrShift = SZ_ERRSHIFT;
constexpr int32_t kTreeShift = SZ_TREESHIFT;
#ifndef SZ_TREESHIFT_B
#define SZ_TREESHIFT_B 4
#endif
#ifndef SZ_T5B
#define SZ_T5B 1
#endif
#ifndef SZ_NO5_B
#define SZ_NO5_B 0
#endif
#ifndef SZ_WCLAMP
#define SZ_WCLAMP 20
#endif
constexpr int32_t kWClamp = 1 << SZ_WCLAMP;

#ifndef SZ_MB
#define SZ_MB 0
#endif
#ifndef SZ_MBW
#define SZ_MBW 0
#endif
#ifndef SZ_WS
#define SZ_WS 1
#endif
#ifndef SZ_STATE
#define SZ_STATE 0
#endif
#ifndef SZ_WSTATE
#define SZ_WSTATE 0
#endif
#ifndef SZ_MB2
#define SZ_MB2 0
#endif
#ifndef SZ_O1EXACT
#define SZ_O1EXACT 1
#endif
#ifndef SZ_BNS
#define SZ_BNS 0
#endif
#ifndef SZ_T5GAP
#define SZ_T5GAP 0
#endif
#ifndef SZ_O1GAP
#define SZ_O1GAP 0
#endif
#ifndef SZ_GAPALL
#define SZ_GAPALL 0
#endif

// Rolling history of the last 5 raw bytes (hist[0] = most recent).
struct Hist {
  uint8_t b[5] = {0, 0, 0, 0, 0};
  uint8_t wpos = 0; // bytes since last space, clamped to 7
  uint8_t bns = 0;  // last non-whitespace byte
  void push(uint8_t v) {
    b[4] = b[3];
    b[3] = b[2];
    b[2] = b[1];
    b[1] = b[0];
    b[0] = v;
    const bool ws = (v == 32) || (v == 9) || (v == 10) || (v == 13);
    const bool wsb = SZ_WS ? ws : (v == 32);
    wpos = wsb ? 0 : (wpos < 7 ? (uint8_t)(wpos + 1) : wpos);
    if (!ws) bns = v;
  }
};

#ifndef SZ_O3BITS
#define SZ_O3BITS 18
#endif
#ifndef SZ_O4BITS
#define SZ_O4BITS 18
#endif
#ifndef SZ_O5BITS
#define SZ_O5BITS 18
#endif

#ifndef SZ_WCTX
#define SZ_WCTX 64
#endif
constexpr uint32_t kWCtx = SZ_WSTATE ? 256u : (uint32_t)SZ_WCTX;

#ifndef SZ_ORDER5
#define SZ_ORDER5 1
#endif

struct MixLit {
  std::vector<Prob, PoolAlloc<Prob>> t[6];
  int32_t w[kWCtx][6];
  uint32_t gap_mode = 0;
  int32_t ts_ = SZ_TREESHIFT;
  int32_t wsh_ = SZ_WSHIFT;
  int32_t esh_ = SZ_ERRSHIFT;
  // "light" pricing mixer: same model shape, cache-resident tree sizes.
  // Used by the DP pricer only — the emitter/decoder keep the full mixer,
  // so light-mode pricing changes search decisions, never the format.
  bool light_ = false;
  uint32_t m1_ = 65535, m2_ = (1u << SZ_O3BITS) - 1, m3_ = (1u << SZ_O4BITS) - 1,
           m4_ = (1u << SZ_O5BITS) - 1, m5_ = 65535;

  explicit MixLit(uint32_t mode = 0, bool light = false) : gap_mode(mode), light_(light) {
    ts_ = gap_mode ? SZ_TREESHIFT_B : SZ_TREESHIFT;
    wsh_ = gap_mode ? SZ_WSHIFT_B : SZ_WSHIFT;
    esh_ = gap_mode ? SZ_ERRSHIFT_B : SZ_ERRSHIFT;
    uint32_t r1 = SZ_O1EXACT ? 65536u : 8192u, r2 = 1u << SZ_O3BITS,
             r3 = 1u << SZ_O4BITS, r4 = 1u << SZ_O5BITS, r5 = 65536u;
    if (light_) { // ~24 MB total: fits L3 on real boxes, kills page walks
      r1 = 4096; r2 = 1u << 14; r3 = 1u << 14; r4 = 0; r5 = 8192;
    }
    m1_ = r1 - 1; m2_ = r2 - 1; m3_ = r3 - 1; m4_ = r4 ? r4 - 1 : 0; m5_ = r5 - 1;
    { const size_t nn = (size_t)((SZ_STATE ? 96u : 32u) * 256); t[0].resize(nn); szip_fill16(t[0].data(), nn, (uint16_t)kProbInit); }
    { const size_t nn = (size_t)r1 * 256; t[1].resize(nn); szip_fill16(t[1].data(), nn, (uint16_t)kProbInit); }
    { const size_t nn = (size_t)r2 * 256; t[2].resize(nn); szip_fill16(t[2].data(), nn, (uint16_t)kProbInit); }
    { const size_t nn = (size_t)r3 * 256; t[3].resize(nn); szip_fill16(t[3].data(), nn, (uint16_t)kProbInit); }
    if (SZ_ORDER5 && r4) { const size_t nn = (size_t)r4 * 256; t[4].resize(nn); szip_fill16(t[4].data(), nn, (uint16_t)kProbInit); }
    { const size_t nn = (size_t)r5 * 256; t[5].resize(nn); szip_fill16(t[5].data(), nn, (uint16_t)kProbInit); } // (wpos, prev, pprev>>3) word shape
    for (auto &row : w)
      for (auto &x : row) x = (1 << kWShift) / 6; // start as equal mix
  }

#ifndef SZ_WCTX_WPOS
#define SZ_W_CTX_WPOS 0
#endif
  static uint32_t wctx(uint32_t bitpos, const Hist &h, uint8_t st) {
    if (SZ_WCTX <= 8 && !SZ_WSTATE) return bitpos;
    uint32_t band = SZ_W_CTX_WPOS ? h.wpos : ((uint32_t)h.b[0] >> 5);
    if (SZ_WSTATE) band += 8u * st;
    return bitpos + 8 * band;
  }


  static uint32_t h1(uint8_t a) { return (uint32_t)a >> 3; }
  static uint32_t h2(uint8_t a, uint8_t b, uint8_t wp, uint8_t mb, uint8_t st,
                     uint8_t bns) {
    if (SZ_O1EXACT) {
      if (SZ_O1GAP) return (((uint32_t)a << 8) | bns); // gap order-1 (mode B)
      return (((uint32_t)a << 8) | b);                  // 65536 exact contexts
    }
    const bool use_mb = SZ_MB || (SZ_MB2 && st != 0);
    const uint32_t bb = SZ_BNS ? bns : b;
    return ((uint32_t)a * 2654435761u ^ (uint32_t)bb * 40503u ^
            (uint32_t)wp * 2246822519u ^
            (use_mb ? (uint32_t)mb * 805459861u : 0u)) >> 19;
  }
  static uint32_t h3(uint8_t a, uint8_t b, uint8_t c, uint8_t wp, uint8_t mb) {
    return ((uint32_t)a * 2654435761u ^ (uint32_t)b * 40503u ^
            (uint32_t)c * 805459861u ^ (uint32_t)wp * 2246822519u ^
            (SZ_MB ? (uint32_t)mb * 3266489917u : 0u)) >> (32 - SZ_O3BITS);
  }
  static uint32_t h4(uint8_t a, uint8_t b, uint8_t c, uint8_t d, uint8_t wp) {
    return ((uint32_t)a * 2654435761u ^ (uint32_t)b * 40503u ^
            (uint32_t)c * 805459861u ^ (uint32_t)d * 1013904223u ^
            (uint32_t)wp * 2246822519u) >> (32 - SZ_O4BITS);
  }
  static uint32_t h5(uint8_t a, uint8_t b, uint8_t c, uint8_t d, uint8_t e,
                     uint8_t wp) {
    return ((uint32_t)a * 2654435761u ^ (uint32_t)b * 40503u ^
            (uint32_t)c * 805459861u ^ (uint32_t)d * 1013904223u ^
            (uint32_t)e * 2246822519u ^ (uint32_t)wp * 3266489917u) >> (32 - SZ_O5BITS);
  }
  uint32_t hw(const Hist &h, uint8_t mb) const {
    if (SZ_T5GAP || gap_mode) {
      if (SZ_T5B == 1) return (((uint32_t)h.b[0] << 8) | h.bns); // exact gap o1
      return (((uint32_t)h.wpos << 13) | ((uint32_t)h.b[0] << 5) | ((uint32_t)h.bns >> 3)); // word-shape gap
    }
    const uint32_t last = SZ_MBW ? mb : h.b[1];
    return (((uint32_t)h.wpos << 13) | ((uint32_t)h.b[0] << 5) | (last >> 3));
  }

  // p0 = P(bit == 0) in [1, 2047]
  uint32_t mix(uint32_t wk, const Prob *n0, const Prob *n1, const Prob *n2,
               const Prob *n3, const Prob *n4, const Prob *n5) const {
    const int64_t x0 = kStretch[*n0], x1 = kStretch[*n1];
    const int64_t x2 = kStretch[*n2], x3 = kStretch[*n3];
    const int64_t x4 = n4 ? kStretch[*n4] : 0;
    const int64_t x5 = kStretch[*n5];
    int64_t x = (w[wk][0] * x0 + w[wk][1] * x1 + w[wk][2] * x2 +
                 w[wk][3] * x3 + w[wk][4] * x4 + w[wk][5] * x5) >>
                kWShift;
    if (x > SZIP_SQUASH_BIAS) x = SZIP_SQUASH_BIAS;
    if (x < -SZIP_SQUASH_BIAS) x = -SZIP_SQUASH_BIAS;
    return (uint32_t)kSquash[(x + SZIP_SQUASH_BIAS) >> SZIP_SQUASH_SHIFT];
  }

  void train(uint32_t wk, Prob *n0, Prob *n1, Prob *n2, Prob *n3, Prob *n4,
             Prob *n5, uint32_t bit, uint32_t p) {
    if (bit == 0) {
      *n0 = (Prob)(*n0 + ((2048 - *n0) >> ts_));
      *n1 = (Prob)(*n1 + ((2048 - *n1) >> ts_));
      *n2 = (Prob)(*n2 + ((2048 - *n2) >> ts_));
      *n3 = (Prob)(*n3 + ((2048 - *n3) >> ts_));
      if (n4) *n4 = (Prob)(*n4 + ((2048 - *n4) >> ts_));
      *n5 = (Prob)(*n5 + ((2048 - *n5) >> ts_));
    } else {
      *n0 = (Prob)(*n0 - (*n0 >> ts_));
      *n1 = (Prob)(*n1 - (*n1 >> ts_));
      *n2 = (Prob)(*n2 - (*n2 >> ts_));
      *n3 = (Prob)(*n3 - (*n3 >> ts_));
      if (n4) *n4 = (Prob)(*n4 - (*n4 >> ts_));
      *n5 = (Prob)(*n5 - (*n5 >> ts_));
    }
    const int32_t err = (int32_t)(bit == 0 ? 2048 - p : 0 - p);
    const int64_t xs[6] = {kStretch[*n0], kStretch[*n1], kStretch[*n2],
                           kStretch[*n3], n4 ? kStretch[*n4] : 0,
                           kStretch[*n5]};
    for (int i = 0; i < 6; ++i) {
      int32_t nw = w[wk][i] + (int32_t)((err * xs[i]) >> esh_);
      if (nw > kWClamp) nw = kWClamp;
      if (nw < -kWClamp) nw = -kWClamp;
      w[wk][i] = nw;
    }
  }

  // Update models for one raw byte (no coding). Called for bytes covered by
  // matches too, so prices stay calibrated.
  void train_byte(Hist &h, uint8_t b, uint8_t mb, uint8_t st) {
    const uint32_t c0 = h1(h.b[0]) + (SZ_STATE ? 32u * st : 0u);
    const uint32_t c1 = h2(h.b[0], h.b[1], h.wpos, mb, st, h.bns);
    const uint32_t c2 = h3(h.b[0], h.b[1], h.b[2], h.wpos, mb);
    const uint32_t c3 = h4(h.b[0], h.b[1], h.b[2], h.b[3], h.wpos);
    const uint32_t c5 = SZ_ORDER5 ? h5(h.b[0], h.b[1], h.b[2], h.b[3], h.b[4], h.wpos) : 0;
    const uint32_t cw = hw(h, mb);
    uint32_t sym = 1;
    for (int k = 7; k >= 0; --k) {
      const uint32_t bit = (b >> k) & 1;
      Prob *n0 = &t[0][c0 * 256 + sym], *n1 = &t[1][(c1 & m1_) * 256 + sym];
      Prob *n2 = &t[2][(c2 & m2_) * 256 + sym], *n3 = &t[3][(c3 & m3_) * 256 + sym];
      Prob *n4 = (SZ_ORDER5 && !light_ && !(gap_mode && SZ_NO5_B)) ? &t[4][(c5 & m4_) * 256 + sym] : nullptr;
      Prob *n5 = &t[5][(cw & m5_) * 256 + sym];
      const uint32_t wk = wctx((uint32_t)(7 - k), h, st);
      const uint32_t p = mix(wk, n0, n1, n2, n3, n4, n5);
      train(wk, n0, n1, n2, n3, n4, n5, bit, p);
      sym = (sym << 1) | bit;
    }
    h.push(b);
  }

  void encode(RcEncoder &rc, Hist &h, uint8_t b, uint8_t mb, uint8_t st) {
    const uint32_t c0 = h1(h.b[0]) + (SZ_STATE ? 32u * st : 0u);
    const uint32_t c1 = h2(h.b[0], h.b[1], h.wpos, mb, st, h.bns);
    const uint32_t c2 = h3(h.b[0], h.b[1], h.b[2], h.wpos, mb);
    const uint32_t c3 = h4(h.b[0], h.b[1], h.b[2], h.b[3], h.wpos);
    const uint32_t c5 = SZ_ORDER5 ? h5(h.b[0], h.b[1], h.b[2], h.b[3], h.b[4], h.wpos) : 0;
    const uint32_t cw = hw(h, mb);
    uint32_t sym = 1;
    for (int k = 7; k >= 0; --k) {
      const uint32_t bit = (b >> k) & 1;
      Prob *n0 = &t[0][c0 * 256 + sym], *n1 = &t[1][(c1 & m1_) * 256 + sym];
      Prob *n2 = &t[2][(c2 & m2_) * 256 + sym], *n3 = &t[3][(c3 & m3_) * 256 + sym];
      Prob *n4 = (SZ_ORDER5 && !light_ && !(gap_mode && SZ_NO5_B)) ? &t[4][(c5 & m4_) * 256 + sym] : nullptr;
      Prob *n5 = &t[5][(cw & m5_) * 256 + sym];
      const uint32_t wk = wctx((uint32_t)(7 - k), h, st);
      const uint32_t p = mix(wk, n0, n1, n2, n3, n4, n5);
#if SZ_STATS
      g_enc_price_sum += (double)kBitPrice[bit == 0 ? p : 2048 - p];
#endif
      rc.encode_bit_p(p, bit);
      train(wk, n0, n1, n2, n3, n4, n5, bit, p);
      sym = (sym << 1) | bit;
    }
    h.push(b);
  }

  uint8_t decode(RcDecoder &rc, Hist &h, uint8_t mb, uint8_t st) {
    const uint32_t c0 = h1(h.b[0]) + (SZ_STATE ? 32u * st : 0u);
    const uint32_t c1 = h2(h.b[0], h.b[1], h.wpos, mb, st, h.bns);
    const uint32_t c2 = h3(h.b[0], h.b[1], h.b[2], h.wpos, mb);
    const uint32_t c3 = h4(h.b[0], h.b[1], h.b[2], h.b[3], h.wpos);
    const uint32_t c5 = SZ_ORDER5 ? h5(h.b[0], h.b[1], h.b[2], h.b[3], h.b[4], h.wpos) : 0;
    const uint32_t cw = hw(h, mb);
    uint32_t sym = 1;
    for (int k = 0; k < 8; ++k) {
      Prob *n0 = &t[0][c0 * 256 + sym], *n1 = &t[1][(c1 & m1_) * 256 + sym];
      Prob *n2 = &t[2][(c2 & m2_) * 256 + sym], *n3 = &t[3][(c3 & m3_) * 256 + sym];
      Prob *n4 = (SZ_ORDER5 && !light_ && !(gap_mode && SZ_NO5_B)) ? &t[4][(c5 & m4_) * 256 + sym] : nullptr;
      Prob *n5 = &t[5][(cw & m5_) * 256 + sym];
      const uint32_t wk = wctx((uint32_t)k, h, st);
      const uint32_t p = mix(wk, n0, n1, n2, n3, n4, n5);
      const uint32_t bit = rc.decode_bit_p(p);
      train(wk, n0, n1, n2, n3, n4, n5, bit, p);
      sym = (sym << 1) | bit;
    }
    return (uint8_t)(sym & 0xFFu);
  }

  // Exact cost in 1/16 bits of coding `b` from history `h`. Mirrors encode()
  // bit-for-bit (intra-byte adaptation included).  The 8 bit-steps touch 8
  // distinct nodes per table (unique sym path) and 8 distinct weight rows
  // (bitpos 0..7), so the adaptation walk runs entirely on local copies:
  // no state is mutated and nothing needs snapshot/restore.
  uint32_t peek(const Hist &h, uint8_t b, uint8_t mb, uint8_t st) {
    const uint32_t c0 = h1(h.b[0]) + (SZ_STATE ? 32u * st : 0u);
    const uint32_t c1 = h2(h.b[0], h.b[1], h.wpos, mb, st, h.bns);
    const uint32_t c2 = h3(h.b[0], h.b[1], h.b[2], h.wpos, mb);
    const uint32_t c3 = h4(h.b[0], h.b[1], h.b[2], h.b[3], h.wpos);
    const uint32_t c5 = SZ_ORDER5 ? h5(h.b[0], h.b[1], h.b[2], h.b[3], h.b[4], h.wpos) : 0;
    const uint32_t cw = hw(h, mb);

    Prob n[8][6];
    int32_t wl[8][6];
    uint32_t wks[8];
    uint32_t sym = 1;
    for (int k = 7; k >= 0; --k) {
      const int step = 7 - k;
      const Prob *srcs[6] = {
          &t[0][c0 * 256 + sym], &t[1][(c1 & m1_) * 256 + sym],
          &t[2][(c2 & m2_) * 256 + sym], &t[3][(c3 & m3_) * 256 + sym],
          (SZ_ORDER5 && !light_ && !(gap_mode && SZ_NO5_B)) ? &t[4][(c5 & m4_) * 256 + sym]
                                                 : nullptr,
          &t[5][(cw & m5_) * 256 + sym]};
      for (int j = 0; j < 6; ++j) n[step][j] = srcs[j] ? *srcs[j] : (Prob)0;
      wks[step] = wctx((uint32_t)(7 - k), h, st);
      for (int j = 0; j < 6; ++j) wl[step][j] = w[wks[step]][j];
      sym = (sym << 1) | ((b >> k) & 1u);
    }

    uint32_t price = 0;
    for (int k = 7; k >= 0; --k) {
      const int step = 7 - k;
      const uint32_t bit = (b >> k) & 1;
      Prob *n0 = &n[step][0], *n1 = &n[step][1], *n2 = &n[step][2];
      Prob *n3 = &n[step][3], *n4 = &n[step][4], *n5 = &n[step][5];
      const int32_t x0 = kStretch[*n0], x1 = kStretch[*n1];
      const int32_t x2 = kStretch[*n2], x3 = kStretch[*n3];
      const int32_t x4 = n4 ? kStretch[*n4] : 0;
      const int32_t x5 = kStretch[*n5];
      int32_t x = (int32_t)((wl[step][0] * (int64_t)x0 + wl[step][1] * (int64_t)x1 +
                             wl[step][2] * (int64_t)x2 + wl[step][3] * (int64_t)x3 +
                             wl[step][4] * (int64_t)x4 + wl[step][5] * (int64_t)x5) >>
                            kWShift);
      if (x > SZIP_SQUASH_BIAS) x = SZIP_SQUASH_BIAS;
      if (x < -SZIP_SQUASH_BIAS) x = -SZIP_SQUASH_BIAS;
      const uint32_t p = (uint32_t)kSquash[(x + SZIP_SQUASH_BIAS) >> SZIP_SQUASH_SHIFT];
      price += kBitPrice[bit == 0 ? p : 2048 - p];
      // train the local copies exactly like train() does
      if (bit == 0) {
        for (int j = 0; j < 6; ++j)
          if (j != 4 || n4)
            n[step][j] = (Prob)(n[step][j] + ((2048 - n[step][j]) >> ts_));
      } else {
        for (int j = 0; j < 6; ++j)
          if (j != 4 || n4)
            n[step][j] = (Prob)(n[step][j] - (n[step][j] >> ts_));
      }
      const int32_t err = (int32_t)(bit == 0 ? 2048 - p : 0 - p);
      const int32_t xs[6] = {kStretch[n[step][0]], kStretch[n[step][1]],
                             kStretch[n[step][2]], kStretch[n[step][3]],
                             n4 ? kStretch[n[step][4]] : 0,
                             kStretch[n[step][5]]};
      for (int j = 0; j < 6; ++j) {
        int32_t nw = wl[step][j] + (int32_t)((err * (int64_t)xs[j]) >> esh_);
        if (nw > kWClamp) nw = kWClamp;
        if (nw < -kWClamp) nw = -kWClamp;
        wl[step][j] = nw;
      }
    }
    return price;
  }
};

} // namespace szip
