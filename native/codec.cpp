// G0D SZIP — SZLZRC v3 block codec, .szip container, C ABI.
// Original implementation for SuperZip; no third-party compression code.
//
// SZLZRC v3: LZ77 (hash chains, lazy) + last-3 rep offsets + context-mixing
// literals + adaptive binary range coder.
#include "szip.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <vector>

#include "crc32.hpp"
#include "lz77.hpp"
#include "mix.hpp"
#ifndef SZ_MATCHBIAS
#define SZ_MATCHBIAS 16
#endif
#ifndef SZ_NOMATCH
#define SZ_NOMATCH 0
#endif
#include "rc.hpp"

#ifndef SZ_STATS
#define SZ_STATS 0
#endif
#if SZ_STATS
static uint64_t st_lit, st_rep[3], st_norm, st_repb, st_nmb, st_bytes;
static uint64_t st_nhist[24];
static uint64_t st_offbits, st_lenbits, st_litbits, st_tokbits;
#endif

namespace szip {
namespace {

// ---------------------------------------------------------------- models --

struct Model {
  Prob is_match = kProbInit;
  Prob is_rep = kProbInit;
  Prob rep_sel[2] = {kProbInit, kProbInit}; // 0: rep0|rep12, 1: rep1|rep2
  Prob len_unary[12];   // gamma of (len - 2), x in [1, 271]
  Prob len_bits[10];
  Prob off_mode = kProbInit;      // 0: tiny (off 1-4), 1: slot+direct
  Prob tiny_unary[8];             // gamma of (off), x in [1, 4]
  Prob tiny_bits[8];
  Prob slot_unary[12];            // gamma of (n - 2), x in [1, 18]
  Prob slot_bits[10];
  Prob off_direct[19];            // low n-1 bits of off-1

  Model() {
    std::fill(std::begin(len_unary), std::end(len_unary), static_cast<Prob>(kProbInit));
    std::fill(std::begin(len_bits), std::end(len_bits), static_cast<Prob>(kProbInit));
    std::fill(std::begin(tiny_unary), std::end(tiny_unary), static_cast<Prob>(kProbInit));
    std::fill(std::begin(tiny_bits), std::end(tiny_bits), static_cast<Prob>(kProbInit));
    std::fill(std::begin(slot_unary), std::end(slot_unary), static_cast<Prob>(kProbInit));
    std::fill(std::begin(slot_bits), std::end(slot_bits), static_cast<Prob>(kProbInit));
    std::fill(std::begin(off_direct), std::end(off_direct), static_cast<Prob>(kProbInit));
  }

};

// ------------------------------------------------------------- primitives --

// Elias gamma for x >= 1: n one-bits, one zero-bit, then n low bits MSB first.
void encode_gamma(RcEncoder &rc, Prob *unary, Prob *bits, uint32_t x) {
  uint32_t n = 0;
  while ((2u << n) <= x) ++n;
  for (uint32_t i = 0; i < n; ++i) rc.encode_bit(unary[i], 1);
  rc.encode_bit(unary[n], 0);
  for (uint32_t i = n; i-- > 0;) rc.encode_bit(bits[i], (x >> i) & 1u);
}

int decode_gamma(RcDecoder &rc, Prob *unary, Prob *bits, uint32_t n_max,
                 uint32_t *out) {
  uint32_t n = 0;
  while (rc.decode_bit(unary[n]) == 1) {
    ++n;
    if (n > n_max) return SZIP_ERR_CORRUPT;
  }
  uint32_t x = 1;
  for (uint32_t i = n; i-- > 0;) x = (x << 1) | rc.decode_bit(bits[i]);
  *out = x;
  return SZIP_OK;
}

// Offset: u = off - 1, n = bitlen(u).
//   tiny mode  (off 1-4):      gamma(off)
//   slot mode  (off >= 5):     gamma(n - 2), then n-1 direct low bits of u
void encode_offset(RcEncoder &rc, Model &m, uint32_t off) {
  const uint32_t u = off - 1;
#if SZ_STATS
  { uint32_t nn = 0; while ((1u << nn) <= u) ++nn; ++st_nhist[nn]; }
#endif
  if (u < 4) {
    rc.encode_bit(m.off_mode, 0);
    encode_gamma(rc, m.tiny_unary, m.tiny_bits, off);
    return;
  }
  rc.encode_bit(m.off_mode, 1);
  uint32_t n = 0;
  while ((1u << n) <= u) ++n;
  encode_gamma(rc, m.slot_unary, m.slot_bits, n - 2);
  for (uint32_t i = n - 1; i-- > 0;)
    rc.encode_bit(m.off_direct[i], (u >> i) & 1u);
}

// ---- model-only training twins (no RC) for parse planning -----------------
static inline void bit_train(Prob &p, uint32_t bit) {
  if (bit == 0) p = (Prob)(p + ((2048 - p) >> 5));
  else p = (Prob)(p - (p >> 5));
}

static void train_gamma(Prob *unary, Prob *bits, uint32_t x) {
  uint32_t n = 0;
  while ((2u << n) <= x) ++n;
  for (uint32_t i = 0; i < n; ++i) bit_train(unary[i], 1);
  bit_train(unary[n], 0);
  for (uint32_t i = n; i-- > 0;) bit_train(bits[i], (x >> i) & 1u);
}

static void train_offset(Model &m, uint32_t off) {
  const uint32_t u = off - 1;
  if (u < 4) {
    bit_train(m.off_mode, 0);
    train_gamma(m.tiny_unary, m.tiny_bits, off);
    return;
  }
  bit_train(m.off_mode, 1);
  uint32_t n = 0;
  while ((1u << n) <= u) ++n;
  train_gamma(m.slot_unary, m.slot_bits, n - 2);
  for (uint32_t i = n - 1; i-- > 0;)
    bit_train(m.off_direct[i], (u >> i) & 1u);
}

static void train_rep_token(Model &m, uint32_t idx, uint32_t len) {
  bit_train(m.is_match, 1);
  bit_train(m.is_rep, 1);
  bit_train(m.rep_sel[0], idx == 0 ? 0u : 1u);
  if (idx != 0) bit_train(m.rep_sel[1], idx == 1 ? 0u : 1u);
  train_gamma(m.len_unary, m.len_bits, len - 2);
}

static void train_norm_token(Model &m, uint32_t off, uint32_t len) {
  bit_train(m.is_match, 1);
  bit_train(m.is_rep, 0);
  train_offset(m, off);
  train_gamma(m.len_unary, m.len_bits, len - 2);
}

// ---- read-only token prices (1/16-bit units) for parse decisions ----------
static inline uint32_t price_bit(Prob p, uint32_t bit) {
  return kBitPrice[bit == 0 ? p : 2048u - (uint32_t)p];
}

static uint32_t price_gamma_p(const Prob *unary, const Prob *bits, uint32_t x) {
  uint32_t n = 0;
  while ((2u << n) <= x) ++n;
  uint32_t price = 0;
  for (uint32_t i = 0; i < n; ++i) price += price_bit(unary[i], 1);
  price += price_bit(unary[n], 0);
  for (uint32_t i = n; i-- > 0;) price += price_bit(bits[i], (x >> i) & 1u);
  return price;
}

static uint32_t price_offset_p(const Model &m, uint32_t off) {
  const uint32_t u = off - 1;
  if (u < 4) {
    return price_bit(m.off_mode, 0) +
           price_gamma_p(m.tiny_unary, m.tiny_bits, off);
  }
  uint32_t price = price_bit(m.off_mode, 1);
  uint32_t n = 0;
  while ((1u << n) <= u) ++n;
  price += price_gamma_p(m.slot_unary, m.slot_bits, n - 2);
  for (uint32_t i = n - 1; i-- > 0;)
    price += price_bit(m.off_direct[i], (u >> i) & 1u);
  return price;
}

static uint32_t price_rep_token(const Model &m, uint32_t idx, uint32_t len) {
  uint32_t price = price_bit(m.is_match, 1) + price_bit(m.is_rep, 1);
  price += price_bit(m.rep_sel[0], idx == 0 ? 0u : 1u);
  if (idx != 0) price += price_bit(m.rep_sel[1], idx == 1 ? 0u : 1u);
  price += price_gamma_p(m.len_unary, m.len_bits, len - 2);
  return price;
}

static uint32_t price_norm_token(const Model &m, uint32_t off, uint32_t len) {
  uint32_t price = price_bit(m.is_match, 1) + price_bit(m.is_rep, 0);
  price += price_offset_p(m, off);
  price += price_gamma_p(m.len_unary, m.len_bits, len - 2);
  return price;
}

int decode_offset(RcDecoder &rc, Model &m, uint32_t *out) {
  if (rc.decode_bit(m.off_mode) == 0) {
    uint32_t off = 0;
    int e = decode_gamma(rc, m.tiny_unary, m.tiny_bits, 6, &off);
    if (e != SZIP_OK) return e;
    if (off == 0 || off > 4) return SZIP_ERR_CORRUPT;
    *out = off;
    return SZIP_OK;
  }
  uint32_t x = 0;
  int e = decode_gamma(rc, m.slot_unary, m.slot_bits, 10, &x);
  if (e != SZIP_OK) return e;
  if (x < 1 || x > 18) return SZIP_ERR_CORRUPT;
  const uint32_t n = x + 2;
  uint32_t u = 1u << (n - 1);
  for (uint32_t i = n - 1; i-- > 0;)
    u |= rc.decode_bit(m.off_direct[i]) << i;
  if (u + 1 > SZIP_MAX_OFFSET) return SZIP_ERR_CORRUPT;
  *out = u + 1;
  return SZIP_OK;
}

// ------------------------------------------------------------ block codec --

struct RepPick {
  uint32_t len = 0;
  uint32_t idx = 0;
};

RepPick rep_match(const uint8_t *src, uint32_t n, uint32_t pos,
                  const uint32_t reps[3]) {
  RepPick best;
  const uint32_t max_len = std::min<uint32_t>(kMaxMatch, n - pos);
  for (uint32_t i = 0; i < 3; ++i) {
    const uint32_t off = reps[i];
    if (off == 0 || off > pos) continue;
    uint32_t len = 0;
    while (len < max_len && src[pos + len] == src[pos - off + len]) ++len;
    if (len >= kMinMatch && len > best.len) {
      best.len = len;
      best.idx = i;
    }
  }
  return best;
}

void reps_after_normal(uint32_t reps[3], uint32_t off) {
  reps[2] = reps[1];
  reps[1] = reps[0];
  reps[0] = off;
}

void reps_after_rep(uint32_t reps[3], uint32_t idx) {
  if (idx > 0) {
    const uint32_t tmp = reps[idx];
    for (uint32_t j = idx; j > 0; --j) reps[j] = reps[j - 1];
    reps[0] = tmp;
  }
}

// ---------------------------------------------------------------------------
// DP optimal parse: shortest path over byte positions.
//
// Nodes are "arrivals" at a position carrying their rep-offset state.
// Edges: literal (exact mixed-model price), rep match, normal match (token
// prices from an online-calibrated token model). The mixed literal model is a
// pure function of the raw prefix, so its per-byte peek prices are exact
// regardless of parse. The decoder is agnostic to all of this.
// ---------------------------------------------------------------------------

struct PlanTok {
  uint32_t len;      // 0 = literal
  uint32_t off;      // normal-match offset (unused for literal/rep)
  uint32_t rep_idx;  // 0..2 for rep tokens, UINT32_MAX for normal/literal
};

struct DPNode {
  uint64_t cost;     // 1/16-bit units
  uint32_t prev;     // pool index, UINT32_MAX = start
  uint32_t reps[3];
  uint32_t len;      // transition INTO this node: 0 = literal
  uint32_t off;
  uint32_t rep_idx;
};

#ifndef SZ_DP_NO_REP
#define SZ_DP_NO_REP 0
#endif
#ifndef SZ_DP_NO_NORM
#define SZ_DP_NO_NORM 0
#endif
constexpr uint32_t kDPBeam = 4;
double plan_cost_estimate = 0;

// lengths whose tokens are evaluated as match edges
static void collect_lens(uint32_t max_len, std::vector<uint32_t> &lens) {
  lens.clear();
  if (max_len <= 40) {
    for (uint32_t l = kMinMatch; l <= max_len; ++l) lens.push_back(l);
    return;
  }
  for (uint32_t l = kMinMatch; l <= 40; ++l) lens.push_back(l);
  for (uint32_t b = 4; b <= 15; ++b) {
    const uint32_t l = (1u << b) + 2;
    if (l > 40 && l < max_len) lens.push_back(l);
  }
  lens.push_back(max_len);
}

void plan_block(const uint8_t *src, uint32_t n, std::vector<PlanTok> &plan) {
  plan.clear();
  plan_cost_estimate = 0;
  if (n == 0) return;

  Model ms;                 // online-calibrated price model
  MatchFinder mf;
  mf.prepare(n);
  std::vector<uint8_t> inserted(n, 0);
  MixLit mix;               // live literal prices (raw-prefix function)
  Hist hist;

  std::vector<DPNode> pool;
  pool.reserve((size_t)n + 16);
  std::vector<std::vector<uint32_t>> best(n + 1);
  for (auto &v : best) v.reserve(kDPBeam + 2);

  DPNode start;
  start.cost = 0;
  start.prev = UINT32_MAX;
  start.reps[0] = start.reps[1] = start.reps[2] = 0;
  start.len = start.off = start.rep_idx = 0;
  pool.push_back(start);
  best[0].push_back(0);

  auto ensure = [&](uint32_t q) {
    if (q < n && !inserted[q]) {
      inserted[q] = 1;
      mf.insert(src, n, q);
    }
  };

  auto try_insert = [&](uint32_t q, DPNode nd) {
    auto &lst = best[q];
    for (uint32_t idx : lst) {
      const DPNode o = pool[idx]; // copy: push_back may realloc pool
      if (o.reps[0] == nd.reps[0] && o.reps[1] == nd.reps[1] &&
          o.reps[2] == nd.reps[2]) {
        if (nd.cost < o.cost) {
          nd.prev = nd.prev; // unchanged
          pool.push_back(nd);
          // replace the equal-state node in the list
          for (uint32_t &slot : lst)
            if (slot == idx) slot = (uint32_t)(pool.size() - 1);
        }
        return;
      }
    }
    if (lst.size() < kDPBeam) {
      pool.push_back(nd);
      lst.push_back((uint32_t)(pool.size() - 1));
      return;
    }
    size_t worst = 0;
    for (size_t i = 1; i < lst.size(); ++i)
      if (pool[lst[i]].cost > pool[lst[worst]].cost) worst = i;
    if (nd.cost < pool[lst[worst]].cost) {
      pool.push_back(nd);
      lst[worst] = (uint32_t)(pool.size() - 1);
    }
  };

  std::vector<uint32_t> lens;
  lens.reserve(64);

  for (uint32_t pos = 0; pos < n; ++pos) {
    if (best[pos].empty()) continue;
    ensure(pos);

    // calibrate the price model on the token of the leading arrival at pos
    {
      uint32_t lead = best[pos][0];
      for (uint32_t idx : best[pos])
        if (pool[idx].cost < pool[lead].cost) lead = idx;
      const DPNode &L = pool[lead];
      if (L.prev != UINT32_MAX) {
        if (L.len == 0) bit_train(ms.is_match, 0);
        else if (L.rep_idx != UINT32_MAX) train_rep_token(ms, L.rep_idx, L.len);
        else train_norm_token(ms, L.off, L.len);
      }
    }

    const uint32_t litp = mix.peek(hist, src[pos]); // exact, 1/16-bit units
#if SZ_STATS
    plan_cost_estimate += (double)litp; // debug: sum of literal prices
#endif
    const Match nm = mf.find(src, n, pos);

    for (uint32_t idx : best[pos]) {
      const DPNode A = pool[idx]; // copy: try_insert may realloc pool

      // literal edge
      {
        DPNode nd = A;
        nd.cost = A.cost + litp;
        nd.prev = idx;
        nd.len = 0;
        nd.off = 0;
        nd.rep_idx = 0;
        try_insert(pos + 1, nd);
      }

      // rep edges from this arrival's offsets
      if (!SZ_DP_NO_REP) {
        const RepPick rep = rep_match(src, n, pos, A.reps);
        if (rep.len >= kMinMatch) {
          collect_lens(rep.len, lens);
          for (uint32_t l : lens) {
            DPNode nd = A;
            nd.cost = A.cost + price_rep_token(ms, rep.idx, l);
            nd.prev = idx;
            nd.len = l;
            nd.off = 0;
            nd.rep_idx = rep.idx;
            reps_after_rep(nd.reps, rep.idx);
            try_insert(pos + l, nd);
          }
        }
      }

      // normal-match edges (same candidate for every arrival here)
      if (!SZ_DP_NO_NORM && nm.len >= kMinMatch) {
        collect_lens(nm.len, lens);
        for (uint32_t l : lens) {
          DPNode nd = A;
          nd.cost = A.cost + price_norm_token(ms, nm.off, l);
#if SZ_STATS
          { static int printed = 0; if (printed < 12 && l == nm.len) {
            fprintf(stderr, "[dbg] dp pos=%u off=%u len=%u est=%u is_match_p=%u litp=%u\n",
                    pos, nm.off, l, price_norm_token(ms, nm.off, l),
                    (uint32_t)ms.is_match, litp);
            ++printed; } }
#endif
          nd.prev = idx;
          nd.len = l;
          nd.off = nm.off;
          nd.rep_idx = UINT32_MAX;
          reps_after_normal(nd.reps, nm.off);
          try_insert(pos + l, nd);
        }
      }
    }

    // advance the raw-prefix model after pricing this position
    mix.train_byte(hist, src[pos]);
  }

  // reconstruct the cheapest full-coverage path
  const auto &fin = best[n];
  if (fin.empty()) {
    // unreachable in practice; fall back to all-literals
    plan.reserve(n);
    for (uint32_t i = 0; i < n; ++i) plan.push_back(PlanTok{0, 0, 0});
    return;
  }
  uint32_t lead = fin[0];
  for (uint32_t idx : fin)
    if (pool[idx].cost < pool[lead].cost) lead = idx;
  std::vector<PlanTok> rev;
  for (uint32_t i = lead; pool[i].prev != UINT32_MAX; i = pool[i].prev) {
    PlanTok t;
    t.len = pool[i].len;
    t.off = pool[i].off;
    t.rep_idx = pool[i].len == 0 ? 0 : pool[i].rep_idx;
    if (pool[i].len == 0) t.rep_idx = UINT32_MAX;
    rev.push_back(t);
  }
  plan.assign(rev.rbegin(), rev.rend());
#if SZ_STATS
  fprintf(stderr, "[stats] sum_litp=%.0f chosen_cost=%.0f\n",
          plan_cost_estimate, (double)pool[lead].cost);
#endif
  plan_cost_estimate = (double)pool[lead].cost;
}

void encode_block_szlzrc(const uint8_t *src, uint32_t n,
                         std::vector<uint8_t> &out) {
  out.clear();
  std::vector<PlanTok> plan;
  plan_block(src, n, plan);

  RcEncoder rc(out);
  Model m;
  MixLit mix;
  Hist hist;
  uint32_t pos = 0;
  uint32_t reps[3] = {0, 0, 0};
  double dp_est = plan_cost_estimate; (void)dp_est;

  for (const PlanTok &tok : plan) {
    if (tok.len == 0) {
#if SZ_STATS
      ++st_lit; ++st_bytes;
#endif
      rc.encode_bit(m.is_match, 0);
      mix.encode(rc, hist, src[pos]);
      ++pos;
    } else if (tok.rep_idx != UINT32_MAX) {
#if SZ_STATS
      ++st_rep[tok.rep_idx]; st_repb += tok.len;
#endif
      rc.encode_bit(m.is_match, 1);
      rc.encode_bit(m.is_rep, 1);
      rc.encode_bit(m.rep_sel[0], tok.rep_idx == 0 ? 0u : 1u);
      if (tok.rep_idx != 0) rc.encode_bit(m.rep_sel[1], tok.rep_idx == 1 ? 0u : 1u);
      encode_gamma(rc, m.len_unary, m.len_bits, tok.len - 2);
      reps_after_rep(reps, tok.rep_idx);
      for (uint32_t i = pos; i < pos + tok.len; ++i) mix.train_byte(hist, src[i]);
      pos += tok.len;
    } else {
#if SZ_STATS
      ++st_norm; st_nmb += tok.len;
      if (st_norm <= 12) {
        fprintf(stderr, "[dbg] norm tok off=%u len=%u live_price=%u\n",
                tok.off, tok.len, price_norm_token(m, tok.off, tok.len));
      }
#endif
      rc.encode_bit(m.is_match, 1);
      rc.encode_bit(m.is_rep, 0);
      encode_offset(rc, m, tok.off);
      encode_gamma(rc, m.len_unary, m.len_bits, tok.len - 2);
      reps_after_normal(reps, tok.off);
      for (uint32_t i = pos; i < pos + tok.len; ++i) mix.train_byte(hist, src[i]);
      pos += tok.len;
    }
  }
  rc.flush();
#if SZ_STATS
  fprintf(stderr, "[stats] enc_price_sum=%.0f\n", g_enc_price_sum);
  fprintf(stderr, "[stats] dp_est=%.0f actual=%llu (1/16 bits)\n",
          dp_est, (unsigned long long)rc.bits * 16);
  fprintf(stderr, "[stats] lit=%llu rep0=%llu(%lluB) rep1=%llu rep2=%llu norm=%llu(%lluB)\n",
          (unsigned long long)st_lit,
          (unsigned long long)st_rep[0], (unsigned long long)st_repb,
          (unsigned long long)st_rep[1], (unsigned long long)st_rep[2],
          (unsigned long long)st_norm, (unsigned long long)st_nmb);
#endif
}

int decode_block_szlzrc(const uint8_t *src, size_t src_len, uint8_t *dst,
                        uint32_t n) {
  if (src_len < 5) return SZIP_ERR_TRUNCATED;
  RcDecoder rc(src, src_len);
  if (!rc.init()) return SZIP_ERR_TRUNCATED;

  Model m;
  MixLit mix;
  Hist hist;
  uint32_t out_pos = 0;
  uint32_t reps[3] = {0, 0, 0};

  while (out_pos < n) {
    if (rc.decode_bit(m.is_match) == 0) {
      dst[out_pos] = mix.decode(rc, hist);
      hist.push(dst[out_pos]);
      ++out_pos;
    } else if (rc.decode_bit(m.is_rep) == 1) {
      uint32_t idx = 0;
      if (rc.decode_bit(m.rep_sel[0]) == 1)
        idx = (rc.decode_bit(m.rep_sel[1]) == 0) ? 1u : 2u;

      uint32_t xlen = 0;
      int e = decode_gamma(rc, m.len_unary, m.len_bits, 10, &xlen);
      if (e != SZIP_OK) return e;
      if (!rc.ok()) return SZIP_ERR_TRUNCATED;
      const uint32_t len = xlen + 2;
      if (len < kMinMatch || len > kMaxMatch) return SZIP_ERR_CORRUPT;
      const uint32_t off = reps[idx];
      if (off == 0 || off > out_pos) return SZIP_ERR_CORRUPT;
      if (len > n - out_pos) return SZIP_ERR_CORRUPT;

      for (uint32_t i = 0; i < len; ++i)
        dst[out_pos + i] = dst[out_pos - off + i];
      reps_after_rep(reps, idx);
      for (uint32_t i = 0; i < len; ++i)
        mix.train_byte(hist, dst[out_pos + i]);
      out_pos += len;
    } else {
      uint32_t off = 0;
      int e = decode_offset(rc, m, &off);
      if (e != SZIP_OK) return e;
      uint32_t xlen = 0;
      e = decode_gamma(rc, m.len_unary, m.len_bits, 10, &xlen);
      if (e != SZIP_OK) return e;
      if (!rc.ok()) return SZIP_ERR_TRUNCATED;
      const uint32_t len = xlen + 2;
      if (len < kMinMatch || len > kMaxMatch) return SZIP_ERR_CORRUPT;
      if (off == 0 || off > out_pos) return SZIP_ERR_CORRUPT;
      if (len > n - out_pos) return SZIP_ERR_CORRUPT;

      for (uint32_t i = 0; i < len; ++i)
        dst[out_pos + i] = dst[out_pos - off + i];
      reps_after_normal(reps, off);
      for (uint32_t i = 0; i < len; ++i)
        mix.train_byte(hist, dst[out_pos + i]);
      out_pos += len;
    }
    if (!rc.ok()) return SZIP_ERR_TRUNCATED;
  }
  if (!rc.ok()) return SZIP_ERR_TRUNCATED;
  if (rc.pos() != src_len) return SZIP_ERR_CORRUPT;
  return SZIP_OK;
}

// ------------------------------------------------------------- le helpers --

void put_u16(std::ostream &os, uint16_t v) {
  char b[2] = {static_cast<char>(v & 0xFF), static_cast<char>((v >> 8) & 0xFF)};
  os.write(b, 2);
}
void put_u32(std::ostream &os, uint32_t v) {
  char b[4] = {static_cast<char>(v & 0xFF), static_cast<char>((v >> 8) & 0xFF),
               static_cast<char>((v >> 16) & 0xFF),
               static_cast<char>((v >> 24) & 0xFF)};
  os.write(b, 4);
}
void put_u64(std::ostream &os, uint64_t v) {
  put_u32(os, static_cast<uint32_t>(v & 0xFFFFFFFFu));
  put_u32(os, static_cast<uint32_t>(v >> 32));
}
bool get_u16(std::istream &is, uint16_t *v) {
  unsigned char b[2];
  if (!is.read(reinterpret_cast<char *>(b), 2)) return false;
  *v = static_cast<uint16_t>(b[0] | (b[1] << 8));
  return true;
}
bool get_u32(std::istream &is, uint32_t *v) {
  unsigned char b[4];
  if (!is.read(reinterpret_cast<char *>(b), 4)) return false;
  *v = static_cast<uint32_t>(b[0]) | (static_cast<uint32_t>(b[1]) << 8) |
       (static_cast<uint32_t>(b[2]) << 16) |
       (static_cast<uint32_t>(b[3]) << 24);
  return true;
}
bool get_u64(std::istream &is, uint64_t *v) {
  uint32_t lo = 0, hi = 0;
  if (!get_u32(is, &lo) || !get_u32(is, &hi)) return false;
  *v = static_cast<uint64_t>(lo) | (static_cast<uint64_t>(hi) << 32);
  return true;
}

bool write_header(std::ostream &os, uint32_t block_size, uint64_t total_raw) {
  char magic[4] = {SZIP_MAGIC0, SZIP_MAGIC1, SZIP_MAGIC2, SZIP_MAGIC3};
  os.write(magic, 4);
  char ver = SZIP_VERSION, flags = 0;
  os.write(&ver, 1);
  os.write(&flags, 1);
  put_u16(os, 0);
  put_u32(os, block_size);
  put_u64(os, total_raw);
  return static_cast<bool>(os);
}

int read_and_check_header(std::istream &is, uint32_t *block_size,
                          uint64_t *total_raw) {
  unsigned char magic[4];
  if (!is.read(reinterpret_cast<char *>(magic), 4)) return SZIP_ERR_TRUNCATED;
  if (magic[0] != SZIP_MAGIC0 || magic[1] != SZIP_MAGIC1 ||
      magic[2] != SZIP_MAGIC2 || magic[3] != SZIP_MAGIC3)
    return SZIP_ERR_FORMAT;
  unsigned char ver = 0, flags = 0;
  if (!is.read(reinterpret_cast<char *>(&ver), 1) ||
      !is.read(reinterpret_cast<char *>(&flags), 1))
    return SZIP_ERR_TRUNCATED;
  if (ver != SZIP_VERSION) return SZIP_ERR_UNSUPPORTED;
  uint16_t reserved = 0;
  if (!get_u16(is, &reserved)) return SZIP_ERR_TRUNCATED;
  if (!get_u32(is, block_size)) return SZIP_ERR_TRUNCATED;
  if (!get_u64(is, total_raw)) return SZIP_ERR_TRUNCATED;
  if (*block_size == 0 || *block_size > (1u << 24)) return SZIP_ERR_FORMAT;
  return SZIP_OK;
}

int read_and_check_block_header(std::istream &is, uint8_t *type,
                                uint32_t *raw_size, uint32_t *comp_size) {
  unsigned char bh[SZIP_BLOCK_HEADER_SIZE];
  if (!is.read(reinterpret_cast<char *>(bh), SZIP_BLOCK_HEADER_SIZE))
    return SZIP_ERR_TRUNCATED;
  *type = bh[0];
  *raw_size = static_cast<uint32_t>(bh[2]) | (static_cast<uint32_t>(bh[3]) << 8) |
              (static_cast<uint32_t>(bh[4]) << 16) |
              (static_cast<uint32_t>(bh[5]) << 24);
  *comp_size = static_cast<uint32_t>(bh[6]) | (static_cast<uint32_t>(bh[7]) << 8) |
               (static_cast<uint32_t>(bh[8]) << 16) |
               (static_cast<uint32_t>(bh[9]) << 24);
  return SZIP_OK;
}

} // namespace

// ------------------------------------------------------------- container --

int compress_file(const std::string &in_path, const std::string &out_path,
                  uint32_t block_size) {
  if (block_size == 0 || block_size > (1u << 24)) return SZIP_ERR_FORMAT;

  std::ifstream is(in_path, std::ios::binary);
  if (!is) return SZIP_ERR_IO;
  is.seekg(0, std::ios::end);
  const uint64_t total = static_cast<uint64_t>(is.tellg());
  is.seekg(0, std::ios::beg);

  std::ofstream os(out_path, std::ios::binary | std::ios::trunc);
  if (!os) return SZIP_ERR_IO;

  if (!write_header(os, block_size, total)) return SZIP_ERR_IO;

  std::vector<uint8_t> block(block_size);
  std::vector<uint8_t> payload;
  uint32_t crc = 0;
  uint64_t remaining = total;

  while (remaining > 0) {
    const uint32_t want =
        static_cast<uint32_t>(std::min<uint64_t>(block_size, remaining));
    if (!is.read(reinterpret_cast<char *>(block.data()), want)) return SZIP_ERR_IO;
    crc = szip_crc32_update(crc, block.data(), want);

    // DP optimal parse is parameter-free; encoder-side only.
    encode_block_szlzrc(block.data(), want, payload);
    const bool store = payload.size() >= want;
    const uint32_t comp_size =
        store ? want : static_cast<uint32_t>(payload.size());

    // Block header, explicitly little-endian.
    char hdr[SZIP_BLOCK_HEADER_SIZE];
    hdr[0] = store ? SZIP_BLOCK_STORED : SZIP_BLOCK_SZLZRC;
    hdr[1] = 0;
    hdr[2] = static_cast<char>(want & 0xFF);
    hdr[3] = static_cast<char>((want >> 8) & 0xFF);
    hdr[4] = static_cast<char>((want >> 16) & 0xFF);
    hdr[5] = static_cast<char>((want >> 24) & 0xFF);
    hdr[6] = static_cast<char>(comp_size & 0xFF);
    hdr[7] = static_cast<char>((comp_size >> 8) & 0xFF);
    hdr[8] = static_cast<char>((comp_size >> 16) & 0xFF);
    hdr[9] = static_cast<char>((comp_size >> 24) & 0xFF);
    os.write(hdr, SZIP_BLOCK_HEADER_SIZE);

    if (store)
      os.write(reinterpret_cast<const char *>(block.data()), want);
    else
      os.write(reinterpret_cast<const char *>(payload.data()), comp_size);
    if (!os) return SZIP_ERR_IO;

    remaining -= want;
  }

  put_u32(os, crc);
  char magic[4] = {SZIP_MAGIC0, SZIP_MAGIC1, SZIP_MAGIC2, SZIP_MAGIC3};
  os.write(magic, 4);
  return os ? SZIP_OK : SZIP_ERR_IO;
}

int decompress_file(const std::string &in_path, const std::string &out_path,
                    uint64_t *raw_out, uint32_t *crc_out) {
  std::ifstream is(in_path, std::ios::binary);
  if (!is) return SZIP_ERR_IO;

  uint32_t block_size = 0;
  uint64_t total_raw = 0;
  int e = read_and_check_header(is, &block_size, &total_raw);
  if (e != SZIP_OK) return e;

  std::ofstream os(out_path, std::ios::binary | std::ios::trunc);
  if (!os) return SZIP_ERR_IO;

  std::vector<uint8_t> payload, raw;
  uint32_t crc = 0;
  uint64_t produced = 0;

  while (produced < total_raw) {
    uint8_t type = 0;
    uint32_t raw_size = 0, comp_size = 0;
    e = read_and_check_block_header(is, &type, &raw_size, &comp_size);
    if (e != SZIP_OK) return e;

    if (raw_size == 0 || raw_size > block_size) return SZIP_ERR_CORRUPT;
    if (raw_size > total_raw - produced) return SZIP_ERR_CORRUPT;
    if (type == SZIP_BLOCK_STORED) {
      if (comp_size != raw_size) return SZIP_ERR_CORRUPT;
    } else if (type == SZIP_BLOCK_SZLZRC) {
      if (comp_size < 5 || comp_size > szip_compress_bound(raw_size))
        return SZIP_ERR_CORRUPT;
    } else {
      return SZIP_ERR_UNSUPPORTED;
    }

    payload.resize(comp_size);
    if (!is.read(reinterpret_cast<char *>(payload.data()), comp_size))
      return SZIP_ERR_TRUNCATED;

    raw.resize(raw_size);
    if (type == SZIP_BLOCK_STORED) {
      std::memcpy(raw.data(), payload.data(), raw_size);
    } else {
      e = decode_block_szlzrc(payload.data(), comp_size, raw.data(), raw_size);
      if (e != SZIP_OK) return e;
    }

    crc = szip_crc32_update(crc, raw.data(), raw_size);
    if (!os.write(reinterpret_cast<const char *>(raw.data()), raw_size))
      return SZIP_ERR_IO;
    produced += raw_size;
  }

  uint32_t stored_crc = 0;
  if (!get_u32(is, &stored_crc)) return SZIP_ERR_TRUNCATED;
  unsigned char end_magic[4];
  if (!is.read(reinterpret_cast<char *>(end_magic), 4)) return SZIP_ERR_TRUNCATED;
  if (end_magic[0] != SZIP_MAGIC0 || end_magic[1] != SZIP_MAGIC1 ||
      end_magic[2] != SZIP_MAGIC2 || end_magic[3] != SZIP_MAGIC3)
    return SZIP_ERR_FORMAT;
  if (stored_crc != crc) return SZIP_ERR_CORRUPT;

  if (raw_out) *raw_out = produced;
  if (crc_out) *crc_out = crc;
  return SZIP_OK;
}

int test_file(const std::string &in_path, uint64_t *raw_out) {
  std::ifstream is(in_path, std::ios::binary);
  if (!is) return SZIP_ERR_IO;

  uint32_t block_size = 0;
  uint64_t total_raw = 0;
  int e = read_and_check_header(is, &block_size, &total_raw);
  if (e != SZIP_OK) return e;

  std::vector<uint8_t> payload, raw;
  uint32_t crc = 0;
  uint64_t produced = 0;

  while (produced < total_raw) {
    uint8_t type = 0;
    uint32_t raw_size = 0, comp_size = 0;
    e = read_and_check_block_header(is, &type, &raw_size, &comp_size);
    if (e != SZIP_OK) return e;

    if (raw_size == 0 || raw_size > block_size) return SZIP_ERR_CORRUPT;
    if (raw_size > total_raw - produced) return SZIP_ERR_CORRUPT;
    if (type == SZIP_BLOCK_STORED) {
      if (comp_size != raw_size) return SZIP_ERR_CORRUPT;
    } else if (type == SZIP_BLOCK_SZLZRC) {
      if (comp_size < 5 || comp_size > szip_compress_bound(raw_size))
        return SZIP_ERR_CORRUPT;
    } else {
      return SZIP_ERR_UNSUPPORTED;
    }
    payload.resize(comp_size);
    if (!is.read(reinterpret_cast<char *>(payload.data()), comp_size))
      return SZIP_ERR_TRUNCATED;
    raw.resize(raw_size);
    if (type == SZIP_BLOCK_STORED) {
      std::memcpy(raw.data(), payload.data(), raw_size);
    } else {
      e = decode_block_szlzrc(payload.data(), comp_size, raw.data(), raw_size);
      if (e != SZIP_OK) return e;
    }
    crc = szip_crc32_update(crc, raw.data(), raw_size);
    produced += raw_size;
  }

  uint32_t stored_crc = 0;
  if (!get_u32(is, &stored_crc)) return SZIP_ERR_TRUNCATED;
  unsigned char end_magic[4];
  if (!is.read(reinterpret_cast<char *>(end_magic), 4)) return SZIP_ERR_TRUNCATED;
  if (end_magic[0] != SZIP_MAGIC0 || end_magic[1] != SZIP_MAGIC1 ||
      end_magic[2] != SZIP_MAGIC2 || end_magic[3] != SZIP_MAGIC3)
    return SZIP_ERR_FORMAT;
  if (stored_crc != crc) return SZIP_ERR_CORRUPT;
  if (raw_out) *raw_out = produced;
  return SZIP_OK;
}

} // namespace szip

// ---------------------------------------------------------------- C ABI ----

extern "C" {

size_t szip_compress_bound(size_t raw_len) {
  return raw_len + raw_len / 2 + 64;
}

int szip_compress_block(const uint8_t *src, size_t src_len, uint8_t *dst,
                        size_t dst_cap, size_t *dst_len) {
  if (!dst_len) return SZIP_ERR_FORMAT;
  if (src_len == 0) {
    *dst_len = 0;
    return SZIP_OK;
  }
  if (src_len > (1u << 24)) return SZIP_ERR_FORMAT;
  try {
    std::vector<uint8_t> payload;
    szip::encode_block_szlzrc(src, static_cast<uint32_t>(src_len), payload);
    if (payload.size() > dst_cap) return SZIP_ERR_BUFFER;
    std::memcpy(dst, payload.data(), payload.size());
    *dst_len = payload.size();
    return SZIP_OK;
  } catch (const std::bad_alloc &) {
    return SZIP_ERR_NOMEM;
  } catch (...) {
    return SZIP_ERR_NOMEM;
  }
}

int szip_decompress_block(const uint8_t *src, size_t src_len, uint8_t *dst,
                          size_t raw_len) {
  if (raw_len == 0) return SZIP_OK;
  if (raw_len > (1u << 24)) return SZIP_ERR_FORMAT;
  return szip::decode_block_szlzrc(src, src_len, dst,
                                   static_cast<uint32_t>(raw_len));
}

uint32_t szip_crc32_update(uint32_t crc, const uint8_t *data, size_t len) {
  return szip::crc32_table().update(crc, data, len);
}

uint32_t szip_crc32(const uint8_t *data, size_t len) {
  return szip::crc32_table().update(0, data, len);
}

} /* extern "C" */
