// G0D SZIP — integer context-mixing literal coder (SZLZRC v3 "GOD stage").
// Original implementation; deterministic fixed-point math only.
#pragma once

#include <cstdint>
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
  std::vector<Prob> t[6];
  int32_t w[kWCtx][6];
  uint32_t gap_mode = 0;
  int32_t ts_ = SZ_TREESHIFT;
  int32_t wsh_ = SZ_WSHIFT;
  int32_t esh_ = SZ_ERRSHIFT;

  explicit MixLit(uint32_t mode = 0) : gap_mode(mode) {
    ts_ = gap_mode ? SZ_TREESHIFT_B : SZ_TREESHIFT;
    wsh_ = gap_mode ? SZ_WSHIFT_B : SZ_WSHIFT;
    esh_ = gap_mode ? SZ_ERRSHIFT_B : SZ_ERRSHIFT;
    t[0].assign((SZ_STATE ? 96u : 32u) * 256, static_cast<Prob>(kProbInit));
    t[1].assign((SZ_O1EXACT ? 65536u : 8192u) * 256, static_cast<Prob>(kProbInit));
    t[2].assign((1u << SZ_O3BITS) * 256, static_cast<Prob>(kProbInit));
    t[3].assign((1u << SZ_O4BITS) * 256, static_cast<Prob>(kProbInit));
    if (SZ_ORDER5) t[4].assign((1u << SZ_O5BITS) * 256, static_cast<Prob>(kProbInit));
    t[5].assign(65536 * 256, static_cast<Prob>(kProbInit)); // (wpos, prev, pprev>>3) word shape
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
      Prob *n0 = &t[0][c0 * 256 + sym], *n1 = &t[1][c1 * 256 + sym];
      Prob *n2 = &t[2][c2 * 256 + sym], *n3 = &t[3][c3 * 256 + sym];
      Prob *n4 = (SZ_ORDER5 && !(gap_mode && SZ_NO5_B)) ? &t[4][c5 * 256 + sym] : nullptr;
      Prob *n5 = &t[5][cw * 256 + sym];
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
      Prob *n0 = &t[0][c0 * 256 + sym], *n1 = &t[1][c1 * 256 + sym];
      Prob *n2 = &t[2][c2 * 256 + sym], *n3 = &t[3][c3 * 256 + sym];
      Prob *n4 = (SZ_ORDER5 && !(gap_mode && SZ_NO5_B)) ? &t[4][c5 * 256 + sym] : nullptr;
      Prob *n5 = &t[5][cw * 256 + sym];
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
      Prob *n0 = &t[0][c0 * 256 + sym], *n1 = &t[1][c1 * 256 + sym];
      Prob *n2 = &t[2][c2 * 256 + sym], *n3 = &t[3][c3 * 256 + sym];
      Prob *n4 = (SZ_ORDER5 && !(gap_mode && SZ_NO5_B)) ? &t[4][c5 * 256 + sym] : nullptr;
      Prob *n5 = &t[5][cw * 256 + sym];
      const uint32_t wk = wctx((uint32_t)k, h, st);
      const uint32_t p = mix(wk, n0, n1, n2, n3, n4, n5);
      const uint32_t bit = rc.decode_bit_p(p);
      train(wk, n0, n1, n2, n3, n4, n5, bit, p);
      sym = (sym << 1) | bit;
    }
    return (uint8_t)(sym & 0xFFu);
  }

  // Exact cost in 1/16 bits of coding `b` from history `h`. Mirrors encode()
  // bit-for-bit (intra-byte adaptation included) via a shadow training walk,
  // then restores all touched state.
  uint32_t peek(const Hist &h, uint8_t b, uint8_t mb, uint8_t st) {
    const uint32_t c0 = h1(h.b[0]) + (SZ_STATE ? 32u * st : 0u);
    const uint32_t c1 = h2(h.b[0], h.b[1], h.wpos, mb, st, h.bns);
    const uint32_t c2 = h3(h.b[0], h.b[1], h.b[2], h.wpos, mb);
    const uint32_t c3 = h4(h.b[0], h.b[1], h.b[2], h.b[3], h.wpos);
    const uint32_t c5 = SZ_ORDER5 ? h5(h.b[0], h.b[1], h.b[2], h.b[3], h.b[4], h.wpos) : 0;
    const uint32_t cw = hw(h, mb);

    // snapshot the exact state this byte will touch (8 steps x 6 models)
    Prob *nodes[8][6];
    Prob saved_n[8][6];
    uint32_t wks[8];
    int32_t saved_w[8][6];
    uint32_t sym = 1;
    for (int k = 7; k >= 0; --k) {
      const int step = 7 - k;
      nodes[step][0] = &t[0][c0 * 256 + sym];
      nodes[step][1] = &t[1][c1 * 256 + sym];
      nodes[step][2] = &t[2][c2 * 256 + sym];
      nodes[step][3] = &t[3][c3 * 256 + sym];
      nodes[step][4] = (SZ_ORDER5 && !(gap_mode && SZ_NO5_B)) ? &t[4][c5 * 256 + sym] : nullptr;
      nodes[step][5] = &t[5][cw * 256 + sym];
      for (int j = 0; j < 6; ++j) saved_n[step][j] = nodes[step][j] ? *nodes[step][j] : 0;
      const uint32_t wk = wctx((uint32_t)(7 - k), h, st);
      wks[step] = wk;
      for (int j = 0; j < 6; ++j) saved_w[step][j] = w[wk][j];
      sym = (sym << 1) | ((b >> k) & 1u);
    }

    // shadow walk: identical to encode()'s updates, accumulating price
    sym = 1;
    uint32_t price = 0;
    for (int k = 7; k >= 0; --k) {
      const uint32_t bit = (b >> k) & 1;
      const uint32_t wk = wctx((uint32_t)(7 - k), h, st);
      Prob *n0 = nodes[7 - k][0], *n1 = nodes[7 - k][1], *n2 = nodes[7 - k][2];
      Prob *n3 = nodes[7 - k][3], *n4 = nodes[7 - k][4], *n5 = nodes[7 - k][5];
      const uint32_t p = mix(wk, n0, n1, n2, n3, n4, n5);
      price += kBitPrice[bit == 0 ? p : 2048 - p];
      train(wk, n0, n1, n2, n3, n4, n5, bit, p);
      sym = (sym << 1) | bit;
    }

    // restore
    for (int step = 0; step < 8; ++step) {
      for (int j = 0; j < 6; ++j)
        if (nodes[step][j]) *nodes[step][j] = saved_n[step][j];
      for (int j = 0; j < 6; ++j) w[wks[step]][j] = saved_w[step][j];
    }
    return price;
  }
};

} // namespace szip
