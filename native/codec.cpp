// G0D SZIP — SZLZRC v3 block codec, .szip container, C ABI.
// Original implementation for SuperZip; no third-party compression code.
//
// SZLZRC v3: LZ77 (hash chains, lazy) + last-3 rep offsets + context-mixing
// literals + adaptive binary range coder.
#include "szip.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <thread>
#include <chrono>
#include <sstream>
#include <memory>
#include <cstdlib>
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
#include "bwt.hpp"

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
  uint32_t regime = 0; // 0: v4 shared token models; 2: tight split models
  Prob is_match = kProbInit;
  Prob is_rep = kProbInit;
  Prob rep_sel[2] = {kProbInit, kProbInit}; // 0: rep0|rep12, 1: rep1|rep2
  Prob len_unary[22];             // gamma of (len - 2) (regime 0 shared)
  Prob len_bits[22];
  Prob off_mode = kProbInit;      // 0: tiny (off 1-4), 1: slot+direct
  Prob tiny_unary[8];             // gamma of (off), x in [1, 4]
  Prob tiny_bits[8];
  Prob slot_unary[12];            // gamma of (n - 2), x in [1, 18]
  Prob slot_bits[10];
  Prob off_direct[19];            // raw high bits of off-1
  Prob off_align[16];             // 4-bit align tree for the low bits

  // -- regime-2 extras (appended: v4 field layout above is frozen so old
  //    files decode with byte-identical probability evolution) --
  Prob nlen_unary[22];            // normal-match length
  Prob nlen_bits[22];
  Prob rlen_unary[22];            // rep length
  Prob rlen_bits[22];
  Prob slot_cls[4][12];           // slot model per length class
  Prob slotb_cls[4][10];

  explicit Model(uint32_t regime_ = 0) : regime(regime_) {
    std::fill(std::begin(len_unary), std::end(len_unary), static_cast<Prob>(kProbInit));
    std::fill(std::begin(len_bits), std::end(len_bits), static_cast<Prob>(kProbInit));
    for (auto &r : nlen_unary) r = static_cast<Prob>(kProbInit);
    for (auto &r : nlen_bits) r = static_cast<Prob>(kProbInit);
    for (auto &r : rlen_unary) r = static_cast<Prob>(kProbInit);
    for (auto &r : rlen_bits) r = static_cast<Prob>(kProbInit);
    for (auto &row : slot_cls)
      for (auto &x : row) x = static_cast<Prob>(kProbInit);
    for (auto &row : slotb_cls)
      for (auto &x : row) x = static_cast<Prob>(kProbInit);
    std::fill(std::begin(tiny_unary), std::end(tiny_unary), static_cast<Prob>(kProbInit));
    std::fill(std::begin(tiny_bits), std::end(tiny_bits), static_cast<Prob>(kProbInit));
    std::fill(std::begin(slot_unary), std::end(slot_unary), static_cast<Prob>(kProbInit));
    std::fill(std::begin(slot_bits), std::end(slot_bits), static_cast<Prob>(kProbInit));
    std::fill(std::begin(off_direct), std::end(off_direct), static_cast<Prob>(kProbInit));
    std::fill(std::begin(off_align), std::end(off_align), static_cast<Prob>(kProbInit));
  }

};

#ifndef SZ_ALIGN
#define SZ_ALIGN 1
#endif

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
void encode_offset(RcEncoder &rc, Model &m, uint32_t off, uint32_t len) {
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
  if (m.regime) {
    const uint32_t c = len - 2;
    const uint32_t cls = c < 3 ? c : 3;
    encode_gamma(rc, m.slot_cls[cls], m.slotb_cls[cls], n - 2);
  } else {
    encode_gamma(rc, m.slot_unary, m.slot_bits, n - 2);
  }
  const uint32_t ncoded = n - 1;
  const uint32_t ntree = ncoded < 4 ? ncoded : 4;
  if (SZ_ALIGN) {
    uint32_t idx = 1;
    for (uint32_t t = ntree; t-- > 0;) {
      const uint32_t bit = (u >> t) & 1u;
      rc.encode_bit(m.off_align[idx], bit);
      idx = (idx << 1) | bit;
    }
    for (uint32_t i = ncoded; i-- > ntree;)
      rc.encode_bit(m.off_direct[i], (u >> i) & 1u);
  } else {
    for (uint32_t i = n - 1; i-- > 0;)
      rc.encode_bit(m.off_direct[i], (u >> i) & 1u);
  }
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

static void train_offset(Model &m, uint32_t off, uint32_t len) {
  const uint32_t u = off - 1;
  if (u < 4) {
    bit_train(m.off_mode, 0);
    train_gamma(m.tiny_unary, m.tiny_bits, off);
    return;
  }
  bit_train(m.off_mode, 1);
  uint32_t n = 0;
  while ((1u << n) <= u) ++n;
  if (m.regime) {
    const uint32_t c = len - 2;
    const uint32_t cls = c < 3 ? c : 3;
    train_gamma(m.slot_cls[cls], m.slotb_cls[cls], n - 2);
  } else {
    train_gamma(m.slot_unary, m.slot_bits, n - 2);
  }
  for (uint32_t i = n - 1; i-- > 0;)
    bit_train(m.off_direct[i], (u >> i) & 1u);
}

static void train_rep_token(Model &m, uint32_t idx, uint32_t len) {
  bit_train(m.is_match, 1);
  bit_train(m.is_rep, 1);
  bit_train(m.rep_sel[0], idx == 0 ? 0u : 1u);
  if (idx != 0) bit_train(m.rep_sel[1], idx == 1 ? 0u : 1u);
  if (m.regime)
    train_gamma(m.rlen_unary, m.rlen_bits, len - 2);
  else
    train_gamma(m.len_unary, m.len_bits, len - 2);
}

static void train_norm_token(Model &m, uint32_t off, uint32_t len) {
  bit_train(m.is_match, 1);
  bit_train(m.is_rep, 0);
  train_offset(m, off, len);
  if (m.regime)
    train_gamma(m.nlen_unary, m.nlen_bits, len - 2);
  else
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

static uint32_t len_class(uint32_t len) {
  const uint32_t c = len - 2;
  return c < 3 ? c : 3;
}

static uint32_t price_len(const Model &m, bool rep, uint32_t len) {
  if (m.regime)
    return price_gamma_p(rep ? m.rlen_unary : m.nlen_unary,
                         rep ? m.rlen_bits : m.nlen_bits, len - 2);
  return price_gamma_p(m.len_unary, m.len_bits, len - 2);
}

static uint32_t price_offset_p(const Model &m, uint32_t off, uint32_t len) {
  const uint32_t u = off - 1;
  if (u < 4) {
    return price_bit(m.off_mode, 0) +
           price_gamma_p(m.tiny_unary, m.tiny_bits, off);
  }
  uint32_t price = price_bit(m.off_mode, 1);
  uint32_t n = 0;
  while ((1u << n) <= u) ++n;
  if (m.regime) {
    const uint32_t cls = len_class(len);
    price += price_gamma_p(m.slot_cls[cls], m.slotb_cls[cls], n - 2);
  } else {
    price += price_gamma_p(m.slot_unary, m.slot_bits, n - 2);
  }
  const uint32_t ncoded = n - 1;
  const uint32_t ntree = ncoded < 4 ? ncoded : 4;
  if (SZ_ALIGN) {
    uint32_t idx = 1;
    for (uint32_t t = ntree; t-- > 0;) {
      const uint32_t bit = (u >> t) & 1u;
      price += price_bit(m.off_align[idx], bit);
      idx = (idx << 1) | bit;
    }
    for (uint32_t i = ncoded; i-- > ntree;)
      price += price_bit(m.off_direct[i], (u >> i) & 1u);
  } else {
    for (uint32_t i = n - 1; i-- > 0;)
      price += price_bit(m.off_direct[i], (u >> i) & 1u);
  }
  return price;
}

[[maybe_unused]] static uint32_t price_rep_token(const Model &m, uint32_t idx, uint32_t len) {
  uint32_t price = price_bit(m.is_match, 1) + price_bit(m.is_rep, 1);
  price += price_bit(m.rep_sel[0], idx == 0 ? 0u : 1u);
  if (idx != 0) price += price_bit(m.rep_sel[1], idx == 1 ? 0u : 1u);
  price += price_len(m, true, len);
  return price;
}

[[maybe_unused]] static uint32_t price_norm_token(const Model &m, uint32_t off, uint32_t len) {
  uint32_t price = price_bit(m.is_match, 1) + price_bit(m.is_rep, 0);
  price += price_offset_p(m, off, len);
  price += price_len(m, false, len);
  return price;
}

int decode_offset(RcDecoder &rc, Model &m, uint32_t len, uint32_t *out) {
  if (rc.decode_bit(m.off_mode) == 0) {
    uint32_t off = 0;
    int e = decode_gamma(rc, m.tiny_unary, m.tiny_bits, 6, &off);
    if (e != SZIP_OK) return e;
    if (off == 0 || off > 4) return SZIP_ERR_CORRUPT;
    *out = off;
    return SZIP_OK;
  }
  uint32_t x = 0;
  int e;
  if (m.regime) {
    const uint32_t cls = len_class(len);
    e = decode_gamma(rc, m.slot_cls[cls], m.slotb_cls[cls], 10, &x);
  } else {
    e = decode_gamma(rc, m.slot_unary, m.slot_bits, 10, &x);
  }
  if (e != SZIP_OK) return e;
  if (x < 1 || x > 18) return SZIP_ERR_CORRUPT;
  const uint32_t n = x + 2;
  uint32_t u = 1u << (n - 1);
  const uint32_t ncoded = n - 1;
  const uint32_t ntree = ncoded < 4 ? ncoded : 4;
  if (SZ_ALIGN) {
    uint32_t idx = 1;
    for (uint32_t t = ntree; t-- > 0;) {
      const uint32_t bit = rc.decode_bit(m.off_align[idx]);
      idx = (idx << 1) | bit;
      u |= bit << t;
    }
    for (uint32_t i = ncoded; i-- > ntree;)
      u |= rc.decode_bit(m.off_direct[i]) << i;
  } else {
    for (uint32_t i = n - 1; i-- > 0;)
      u |= rc.decode_bit(m.off_direct[i]) << i;
  }
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
    const uint32_t len = match_len(src + pos - off, src + pos, max_len);
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
#ifndef SZ_ABEAM
#define SZ_ABEAM 8
#endif
constexpr uint32_t kDPBeam = SZ_ABEAM;
constexpr uint32_t kBeamSlots = 32; // allocation stride (>= any beam_cap)
thread_local double plan_cost_estimate = 0;

// lengths whose tokens are evaluated as match edges
#ifndef SZ_ALLLEN
#define SZ_ALLLEN 1
#endif
#ifndef SZ_LCAP
#define SZ_LCAP 0
#endif
// Encoder-side parse knob (set_parse_fast): 0 = max ratio, 1 = fast parse.
static thread_local int g_parse_fast = 0;
// rank 0 = best candidate (fine lengths); rank > 0 = secondary (coarse).
static void collect_lens(uint32_t max_len, uint32_t rank,
                         std::vector<uint32_t> &lens) {
  lens.clear();
  if (max_len < kMinMatch) return;
  // A match that already reaches >=1 KiB is taken whole: cutting inside it
  // is almost never worth the search (this makes run/repetition data ~30x
  // cheaper to parse).  Below that, trial lengths stay bounded so the
  // arrivals x candidates x lengths product is O(1) per position; the
  // multi-pass refinement recovers exact lengths along the plan.
  if (max_len >= 1024) {
    lens.push_back(max_len);
    return;
  }
  if (rank == 0) {
    static const uint32_t kHead = 24;
    static const uint32_t kBins[] = {32, 48, 64, 96, 128, 192, 256, 384, 512,
                                     768};
    const uint32_t hi = max_len < kHead ? max_len : kHead;
    for (uint32_t l = kMinMatch; l <= hi; ++l) lens.push_back(l);
    for (uint32_t b : kBins)
      if (b > hi && b < max_len) lens.push_back(b);
    if (max_len > hi) lens.push_back(max_len);
  } else {
    static const uint32_t kCoarse[] = {3, 4, 8, 16, 32, 64, 128, 256, 512};
    for (uint32_t l : kCoarse)
      if (l <= max_len) lens.push_back(l);
    if (max_len > 512) lens.push_back(max_len);
  }
}

// guide != nullptr: calibrate the price model EXACTLY along the guide plan's
// token history (matching the emit walk's training), instead of the noisy
// leading-arrival heuristic. Returns the plan via `plan`.
// Match candidates depend only on (src, pos): build once, share across all
// planning passes and both modes. Canonical insertion order (positions 0..n-1)
// makes the finder deterministic and cacheable.
#ifndef SZ_CANDW
#define SZ_CANDW 12 // finder cache width (match candidates per position)
#endif
#ifndef SZ_PASSES
#define SZ_PASSES 4 // DP parse passes (pass 1 online, rest exact-along-plan)
#endif
#ifndef SZ_GBEAM
#define SZ_GBEAM 12 // DP beam width for the guided passes
#endif
#ifndef SZ_MCAND
#define SZ_MCAND 12 // match candidates offered to the parse
#endif
struct CandCache {
  std::vector<uint8_t> cnt;
  std::vector<Match> m; // SZ_CANDW per position
  std::vector<uint32_t> re; // one-past-end of the maximal equal-byte run
};
// Thread count for data-parallel work (deterministic: task results never
// depend on scheduling).  Small machines are the norm for this tool; the
// cap keeps pathological hosts from oversubscribing.
static unsigned sz_workers() {
  unsigned hw = std::thread::hardware_concurrency();
  if (hw == 0) hw = 2;
  return hw < 2 ? 1u : (hw > 8 ? 8u : hw);
}

static void build_cand_cache(const uint8_t *src, uint32_t n, CandCache &cc) {
  cc.cnt.assign(n, 0);
  cc.m.assign((size_t)n * SZ_CANDW, Match{0, 0});
  cc.re.assign(n, 0);
  for (uint32_t i = n; i-- > 0;)
    cc.re[i] = (i + 1 < n && src[i] == src[i + 1]) ? cc.re[i + 1] : i + 1;
  // Phase 1 (sequential, cheap): hash links only.  link1[p] = previous
  // inserted same-hash position + 1 (0 = none) — exactly the chain order
  // the sequential head_/prev_ finder would produce.
  std::vector<uint32_t> head(kHashSize, 0);
  std::vector<uint32_t> link1(n, 0);
  for (uint32_t pos = 0; pos < n; ++pos) {
    if (pos + kMinMatch > n) continue;
    const uint32_t h = MatchFinder::hash3(src + pos);
    link1[pos] = head[h];
    head[h] = pos + 1;
  }
  // Phase 2 (parallel): per-position walks are independent and write
  // disjoint cache slots.  Results are identical to the sequential finder.
  MatchFinder mf;
  const unsigned nw = sz_workers();
  std::vector<std::thread> ths;
  for (unsigned t = 0; t < nw; ++t) {
    const uint32_t lo = (uint32_t)((uint64_t)n * t / nw);
    const uint32_t hi = (uint32_t)((uint64_t)n * (t + 1) / nw);
    ths.emplace_back([&, lo, hi] {
      Match tmp[SZ_CANDW];
      for (uint32_t pos = lo; pos < hi; ++pos) {
        const uint32_t k =
            mf.find_multi_link(src, n, pos, link1.data(), tmp, SZ_CANDW);
        cc.cnt[pos] = (uint8_t)k;
        for (uint32_t i = 0; i < k; ++i) cc.m[(size_t)pos * SZ_CANDW + i] = tmp[i];
      }
    });
  }
  for (auto &t : ths) t.join();
}

void plan_block_pass(const uint8_t *src, uint32_t n, std::vector<PlanTok> &plan,
                     const std::vector<PlanTok> *guide, uint32_t mode,
                     const CandCache &cc, std::vector<uint32_t> &litp_cache) {
  plan.clear();
  plan_cost_estimate = 0;
  if (n == 0) return;

  // precompute guide token boundaries: gin[pos] = guide token ending at pos
  std::vector<int32_t> gin;
  std::vector<uint32_t> greps0;
  if (guide) {
    gin.assign(n + 1, -1);
    greps0.assign(n + 1, 0);
    uint32_t reps[3] = {0, 0, 0};
    uint32_t pos = 0;
    for (uint32_t i = 0; i < guide->size(); ++i) {
      const PlanTok &t = (*guide)[i];
      if (t.len == 0) {
        gin[pos + 1] = (int32_t)i;
        greps0[pos + 1] = reps[0];
        pos += 1;
      } else {
        if (t.rep_idx != UINT32_MAX) reps_after_rep(reps, t.rep_idx);
        else reps_after_normal(reps, t.off);
        gin[pos + t.len] = (int32_t)i;
        greps0[pos + t.len] = reps[0];
        pos += t.len;
      }
    }
  }

  Model ms(mode >= 2 ? 2u : 0u); // online-calibrated price model
  std::unique_ptr<MixLit> mix;   // live literal prices (unguided pass only)
  if (!guide) mix.reset(new MixLit(mode & 1u));
  Hist hist;

  std::vector<DPNode> pool;
  pool.reserve((size_t)n * 6 + 64);
  std::vector<uint32_t> free_nodes;
  free_nodes.reserve(256);
  std::vector<uint32_t> best_cnt(n + 1, 0);
  std::vector<uint32_t> best_slot((size_t)(n + 1) * kBeamSlots, 0);
  // Exact worst-cost cache per position list (UINT64_MAX = unknown/stale).
  // Makes the common "cannot beat the worst entry" rejection O(1) instead
  // of a pool scan; semantics of the full path are unchanged.
  std::vector<uint64_t> best_worstc(n + 1, UINT64_MAX);

  DPNode start;
  start.cost = 0;
  start.prev = UINT32_MAX;
  start.reps[0] = start.reps[1] = start.reps[2] = 0;
  start.len = start.off = start.rep_idx = 0;
  pool.push_back(start);
  best_slot[0] = 0;
  best_cnt[0] = 1;

  const uint32_t beam_cap =
      g_parse_fast ? (g_parse_fast >= 2 ? 2u : 4u)
      : (guide ? ((uint32_t)SZ_GBEAM < kBeamSlots ? (uint32_t)SZ_GBEAM
                                               : kBeamSlots)
               : kDPBeam);
  // Recycle nodes dropped from a best list (replaced or evicted): such nodes
  // are provably never prev-ancestors of anything (lists freeze before their
  // position is processed), so reuse is exact.
  auto new_node = [&](const DPNode &nd) -> uint32_t {
    if (!free_nodes.empty()) {
      const uint32_t idx = free_nodes.back();
      free_nodes.pop_back();
      pool[idx] = nd;
      return idx;
    }
    pool.push_back(nd);
    return (uint32_t)(pool.size() - 1);
  };
  auto try_insert = [&](uint32_t q, const DPNode &nd) {
    uint32_t *lst = &best_slot[(size_t)q * kBeamSlots];
    uint32_t &cnt = best_cnt[q];
    const uint64_t ndc = nd.cost;
    // Fast reject: a full list rejects anything >= its worst entry (and a
    // same-reps replace would need to beat that entry's cost too).  Exact.
    const uint64_t wc = best_worstc[q];
    if (cnt >= beam_cap && wc != UINT64_MAX && ndc >= wc) return;
    const uint32_t r0 = nd.reps[0], r1 = nd.reps[1], r2 = nd.reps[2];
    uint32_t worst = 0;
    uint64_t w1 = 0, w2 = 0;
    for (uint32_t i = 0; i < cnt; ++i) {
      const uint32_t old = lst[i];
      const DPNode &o = pool[old];
      if (o.reps[0] == r0 && o.reps[1] == r1 && o.reps[2] == r2) {
        if (ndc < o.cost) {
          lst[i] = new_node(nd);
          free_nodes.push_back(old);
          best_worstc[q] = UINT64_MAX; // worst may have moved; recompute lazily
        }
        return;
      }
      if (o.cost > w1) { w2 = w1; w1 = o.cost; worst = i; }
      else if (i != worst && o.cost > w2) w2 = o.cost;
    }
    if (cnt < beam_cap) {
      lst[cnt++] = new_node(nd);
      best_worstc[q] = ndc > w1 ? ndc : w1;
      return;
    }
    if (ndc < w1) {
      const uint32_t old = lst[worst];
      lst[worst] = new_node(nd);
      free_nodes.push_back(old);
      best_worstc[q] = ndc > w2 ? ndc : w2;
    } else {
      best_worstc[q] = w1;
    }
  };

  std::vector<uint32_t> lens;
  lens.reserve(64);
  std::vector<uint32_t> lenp(kMaxMatch + 1, 0);
  std::vector<uint32_t> lenpr(kMaxMatch + 1, 0);
  std::vector<uint32_t> lenst(kMaxMatch + 1, 0);
  uint32_t lenst_cur = 0;

  for (uint32_t pos = 0; pos < n; ++pos) {
    if (best_cnt[pos] == 0) continue;

    // calibrate the price model
    uint8_t mb = 0;
    uint8_t st = 0;
    if (guide) {
      if (gin[pos] >= 0) {
        const PlanTok &g = (*guide)[(uint32_t)gin[pos]];
        if (g.len == 0) { bit_train(ms.is_match, 0); st = 0; }
        else if (g.rep_idx != UINT32_MAX) { train_rep_token(ms, g.rep_idx, g.len); st = 1; }
        else { train_norm_token(ms, g.off, g.len); st = 2; }
      }
      mb = (uint8_t)((pos >= greps0[pos]) ? src[pos - greps0[pos]] : 0);
    } else {
      uint32_t lead = best_slot[(size_t)pos * kBeamSlots];
      for (uint32_t i = 0; i < best_cnt[pos]; ++i) {
        uint32_t idx = best_slot[(size_t)pos * kBeamSlots + i];
        if (pool[idx].cost < pool[lead].cost) lead = idx;
      }
      const DPNode &L = pool[lead];
      if (L.prev != UINT32_MAX) {
        if (L.len == 0) { bit_train(ms.is_match, 0); st = 0; }
        else if (L.rep_idx != UINT32_MAX) { train_rep_token(ms, L.rep_idx, L.len); st = 1; }
        else { train_norm_token(ms, L.off, L.len); st = 2; }
      }
      mb = (uint8_t)((pos >= L.reps[0]) ? src[pos - L.reps[0]] : 0);
    }

    uint32_t litp;
    if (guide) {
      litp = litp_cache[pos]; // exact price cached by the unguided pass
    } else {
      litp = mix->peek(hist, src[pos], mb, st);
      litp_cache[pos] = litp;
    }
#if SZ_STATS
    plan_cost_estimate += (double)litp; // debug: sum of literal prices
#endif
    Match cands[16];
    const uint32_t mcc = g_parse_fast ? 6u : (uint32_t)SZ_MCAND;
    const uint32_t ncand = SZ_DP_NO_NORM ? 0u : cc.cnt[pos];
    for (uint32_t i = 0; i < ncand && i < mcc; ++i)
      cands[i] = cc.m[(size_t)pos * SZ_CANDW + i];

    // ms is constant within a position: pre-price every length and every
    // candidate distance ONCE instead of per (arrival, candidate, length).
    // Only price the lengths this position can actually offer.
    uint32_t norm_prefix[16];
    uint32_t offp[16][4]; // regime 2: offset price per length class
    ++lenst_cur;
    auto plen = [&](uint32_t l, bool is_rep) -> uint32_t {
      if (l > kMaxMatch) return price_len(ms, is_rep, l);
      if (lenst[l] != lenst_cur) {
        lenst[l] = lenst_cur;
        if (ms.regime) {
          lenp[l] = price_gamma_p(ms.nlen_unary, ms.nlen_bits, l - 2);
          lenpr[l] = price_gamma_p(ms.rlen_unary, ms.rlen_bits, l - 2);
        } else {
          lenp[l] = price_gamma_p(ms.len_unary, ms.len_bits, l - 2);
          lenpr[l] = lenp[l];
        }
      }
      return (is_rep && ms.regime) ? lenpr[l] : lenp[l];
    };
    uint32_t max_need = kMinMatch;
    for (uint32_t ci = 0; ci < ncand && ci < mcc; ++ci) {
      norm_prefix[ci] = price_bit(ms.is_match, 1) + price_bit(ms.is_rep, 0);
      if (ms.regime) {
        for (uint32_t cls = 0; cls < 4; ++cls)
          offp[ci][cls] = price_offset_p(ms, cands[ci].off, cls + 2);
      } else {
        norm_prefix[ci] += price_offset_p(ms, cands[ci].off, 2);
      }
      if (cands[ci].len > max_need) max_need = cands[ci].len;
    }

    for (uint32_t bi = 0; bi < best_cnt[pos]; ++bi) {
      uint32_t idx = best_slot[(size_t)pos * kBeamSlots + bi];
      const DPNode A = pool[idx]; // copy: try_insert may realloc pool

      // literal edge (guided pass prices the is_match=0 bit too: exact there)
      {
        DPNode nd = A;
        nd.cost = A.cost + litp + (guide ? price_bit(ms.is_match, 0) : 0u);
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
          // a dist-1 rep inside a run extends past the finder cap (run token)
          uint32_t run_ext =
              (pos > 0 && A.reps[rep.idx] == 1 && src[pos] == src[pos - 1])
                  ? cc.re[pos] - pos
                  : 0;
          if (run_ext > SZIP_MAX_RUN) run_ext = SZIP_MAX_RUN;
          const uint32_t rep_max = run_ext > rep.len ? run_ext : rep.len;
          const uint32_t rep_prefix =
              price_bit(ms.is_match, 1) + price_bit(ms.is_rep, 1) +
              price_bit(ms.rep_sel[0], rep.idx == 0 ? 0u : 1u) +
              (rep.idx != 0 ? price_bit(ms.rep_sel[1], rep.idx == 1 ? 0u : 1u) : 0u);
          collect_lens(rep_max, 0, lens);
          DPNode nd = A;
          nd.prev = idx;
          nd.off = 0;
          nd.rep_idx = rep.idx;
          reps_after_rep(nd.reps, rep.idx);
          for (uint32_t l : lens) {
            nd.cost = A.cost + rep_prefix + plen(l, true);
            nd.len = l;
            try_insert(pos + l, nd);
          }
        }
      }

      // run edge: dist-1 whole-run match past the 2 KiB finder cap.  A
      // dist-1 match is by definition a run of one byte, so one token can
      // carry the entire run (the format's gamma length covers up to 2^21).
      if (pos > 0 && src[pos] == src[pos - 1]) {
        uint32_t run_len = cc.re[pos] - pos;
        if (run_len > SZIP_MAX_RUN) run_len = SZIP_MAX_RUN;
        if (run_len > kMaxMatch) {
          DPNode nd = A;
          nd.prev = idx;
          nd.off = 1;
          nd.rep_idx = UINT32_MAX;
          reps_after_normal(nd.reps, 1);
          nd.cost = A.cost + price_bit(ms.is_match, 1) + price_bit(ms.is_rep, 0) +
                    price_offset_p(ms, 1, 2) + plen(run_len, false);
          nd.len = run_len;
          try_insert(pos + run_len, nd);
        }
      }

      // normal-match edges: every candidate distance, priced exactly
      for (uint32_t ci = 0; ci < ncand && ci < mcc; ++ci) {
        const Match &nm = cands[ci];
        collect_lens(nm.len, ci, lens);
        DPNode nd = A;
        nd.prev = idx;
        nd.off = nm.off;
        nd.rep_idx = UINT32_MAX;
        reps_after_normal(nd.reps, nm.off);
        const uint32_t npref = norm_prefix[ci];
        for (uint32_t l : lens) {
          nd.cost = A.cost + npref +
                    (ms.regime ? offp[ci][len_class(l)] : 0u) + plen(l, false);
          nd.len = l;
          try_insert(pos + l, nd);
        }
      }
    }

    // advance the raw-prefix model after pricing this position (unguided only)
    if (!guide) mix->train_byte(hist, src[pos], mb, st);
  }

#if SZ_STATS
  fprintf(stderr, "[pool] mode=%u guide=%d n=%u pool=%zu cap=%zu\n",
          mode, guide ? 1 : 0, n, pool.size(), pool.capacity());
#endif
  // reconstruct the cheapest full-coverage path
  const uint32_t *fin = &best_slot[(size_t)n * kBeamSlots];
  const uint32_t fin_cnt = best_cnt[n];
  if (fin_cnt == 0) {
    // unreachable in practice; fall back to all-literals
    plan.reserve(n);
    for (uint32_t i = 0; i < n; ++i) plan.push_back(PlanTok{0, 0, 0});
    return;
  }
  uint32_t lead = fin[0];
  for (uint32_t i = 0; i < fin_cnt; ++i)
    if (pool[fin[i]].cost < pool[lead].cost) lead = fin[i];
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

static void emit_plan(const uint8_t *src, uint32_t n,
                      const std::vector<PlanTok> &plan,
                      std::vector<uint8_t> &out, uint32_t mode) {
  (void)n;
  RcEncoder rc(out);
  Model m(mode >= 2 ? 2u : 0u);
  MixLit mix(mode & 1u);
  Hist hist;
  uint32_t pos = 0;
  uint32_t reps[3] = {0, 0, 0};
  uint8_t st = 0;
  double dp_est = plan_cost_estimate; (void)dp_est;

  for (const PlanTok &tok : plan) {
    if (tok.len == 0) {
#if SZ_STATS
      ++st_lit; ++st_bytes;
#endif
      rc.encode_bit(m.is_match, 0);
      {
        const uint8_t mb = (uint8_t)((pos >= reps[0]) ? src[pos - reps[0]] : 0);
        mix.encode(rc, hist, src[pos], mb, st);
      }
      st = 0;
      ++pos;
    } else if (tok.rep_idx != UINT32_MAX) {
#if SZ_STATS
      ++st_rep[tok.rep_idx]; st_repb += tok.len;
#endif
      rc.encode_bit(m.is_match, 1);
      rc.encode_bit(m.is_rep, 1);
      rc.encode_bit(m.rep_sel[0], tok.rep_idx == 0 ? 0u : 1u);
      if (tok.rep_idx != 0) rc.encode_bit(m.rep_sel[1], tok.rep_idx == 1 ? 0u : 1u);
      if (m.regime)
        encode_gamma(rc, m.rlen_unary, m.rlen_bits, tok.len - 2);
      else
        encode_gamma(rc, m.len_unary, m.len_bits, tok.len - 2);
      reps_after_rep(reps, tok.rep_idx);
      st = 1;
      for (uint32_t i = pos; i < pos + tok.len; ++i) {
        const uint8_t mb = (uint8_t)((i >= reps[0]) ? src[i - reps[0]] : 0);
        mix.train_byte(hist, src[i], mb, st);
      }
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
      if (m.regime) {
        // tight regime: length first so the offset slot knows its class
        encode_gamma(rc, m.nlen_unary, m.nlen_bits, tok.len - 2);
        encode_offset(rc, m, tok.off, tok.len);
      } else {
        encode_offset(rc, m, tok.off, tok.len);
        encode_gamma(rc, m.len_unary, m.len_bits, tok.len - 2);
      }
      reps_after_normal(reps, tok.off);
      st = 2;
      for (uint32_t i = pos; i < pos + tok.len; ++i) {
        const uint8_t mb = (uint8_t)((i >= reps[0]) ? src[i - reps[0]] : 0);
        mix.train_byte(hist, src[i], mb, st);
      }
      pos += tok.len;
    }
  }
  rc.flush();
#if SZ_STATS
  fprintf(stderr, "[stats] enc_price_sum=%.0f\n", g_enc_price_sum);
  fprintf(stderr, "[stats] dp_est=%.0f actual=%llu (1/16 bits)\n",
          dp_est, (unsigned long long)rc.bits * 16);
  fprintf(stderr, "[stats] nhist:");
  for (int i = 0; i < 32; ++i) fprintf(stderr, " %d:%llu", i, (unsigned long long)st_nhist[i]);
  fprintf(stderr, "\n[stats] lit=%llu rep0=%llu(%lluB) rep1=%llu rep2=%llu norm=%llu(%lluB)\n",
          (unsigned long long)st_lit,
          (unsigned long long)st_rep[0], (unsigned long long)st_repb,
          (unsigned long long)st_rep[1], (unsigned long long)st_rep[2],
          (unsigned long long)st_norm, (unsigned long long)st_nmb);
#endif
}

static bool same_plan(const std::vector<PlanTok> &a,
                      const std::vector<PlanTok> &b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i) {
    if (a[i].len != b[i].len || a[i].off != b[i].off ||
        a[i].rep_idx != b[i].rep_idx)
      return false;
  }
  return true;
}

#ifndef SZ_TIME
#define SZ_TIME 0
#endif
#if SZ_TIME
#define SZ_TIC(name) auto name = std::chrono::steady_clock::now()
#define SZ_TOC(name, msg)                                                     \
  do {                                                                        \
    double dt = std::chrono::duration<double>(                                \
                    std::chrono::steady_clock::now() - name)                  \
                    .count();                                                 \
    fprintf(stderr, "[time] %-22s %7.2fs\n", msg, dt);                        \
  } while (0)
#else
#define SZ_TIC(name) (void)0
#define SZ_TOC(name, msg) (void)0
#endif

static void encode_one_mode(const uint8_t *src, uint32_t n, uint32_t mode,
                            const CandCache &cc, std::vector<uint8_t> &out) {
  std::vector<uint32_t> litp_cache(n, 0);
  std::vector<PlanTok> plans[8];
  SZ_TIC(t0);
  plan_block_pass(src, n, plans[0], nullptr, mode, cc, litp_cache);
  size_t nplans = 1;
  const size_t kmax_pass = (g_parse_fast >= 2) ? 2u : (size_t)SZ_PASSES;
  for (size_t k = 1; k < kmax_pass; ++k) {
    plan_block_pass(src, n, plans[k], &plans[k - 1], mode, cc, litp_cache);
    nplans = k + 1;
    if (same_plan(plans[k], plans[k - 1])) break; // converged: later passes repeat
  }
  SZ_TOC(t0, mode ? "passes (mode B)" : "passes (mode A)");
  SZ_TIC(t1);
  std::vector<uint8_t> bestp;
  std::vector<std::vector<uint8_t>> emitted(nplans);
  emit_plan(src, n, plans[0], emitted[0], mode);
  bestp.swap(emitted[0]);
  for (size_t k = 1; k < nplans; ++k) {
    emit_plan(src, n, plans[k], emitted[k], mode);
    if (emitted[k].size() < bestp.size()) bestp.swap(emitted[k]);
  }
  SZ_TOC(t1, mode ? "emits (mode B)" : "emits (mode A)");
  out.swap(bestp);
}

// v6 payload = 1 mode byte + RC stream.  mode byte: bit 2 = BWT codec,
// bits 3-5 = byte-delta id; LZ path mode = (regime << 1) | mixer:
//   0: v4 regime, word-shape t5    1: v4 regime, gap order-1 t5
//   2: tight regime, word-shape    3: tight regime, gap order-1
// Escalating effort (self-tuning min-of-all): cheap candidates first — the
// four fast-parse modes and the BWT/delta family — then the deep-parse
// modes only when they could still matter (within 2% of the cheap best).
void encode_block_szlzrc(const uint8_t *src, uint32_t n,
                         std::vector<uint8_t> &out) {
  out.clear();
  if (n == 0) return;
  SZ_TIC(tc);
  CandCache cc;
  build_cand_cache(src, n, cc);
  SZ_TOC(tc, "finder cache");
  // The BWT/delta family is independent of the LZ parse: start it now so
  // it occupies the spare core while the (single-threaded) parse chain runs.  Selection happens after the join — output is identical.
  std::vector<uint8_t> best_bwt;
  uint32_t best_delta = 0;
  SZ_TIC(tbw);
  std::thread bwt_thread([&] {
    static const uint32_t kDeltas[4] = {1, 2, 4, 8};
    // Gap-entropy proxy picks the one delta worth encoding alongside raw
    // (measured: only pcm ever wins via delta, and its d=2 gap entropy is
    // far below the rest).  Two BWT encodes instead of five.
    uint32_t pick = 0;
    {
      double best_h = -1.0;
      for (uint32_t di = 0; di < 4; ++di) {
        const uint32_t dlt = kDeltas[di];
        uint32_t cnt[256] = {0};
        for (uint32_t i = dlt; i < n; ++i) {
          const uint8_t v = (uint8_t)(src[i] - src[i - dlt]);
          ++cnt[v];
        }
        double h = 0;
        for (uint32_t c : cnt)
          if (c) h += (double)c * -__builtin_log2((double)c / (double)(n - dlt ? n - dlt : 1));
        if (best_h < 0 || h < best_h) { best_h = h; pick = di; }
      }
    }
    std::vector<std::vector<uint8_t>> pay(2);
    std::vector<uint8_t> dtmp;
    std::vector<std::thread> ths;
    ths.emplace_back([&] { bwt::encode_block(src, n, pay[0]); });
    ths.emplace_back([&, pick] {
      const uint32_t dlt = kDeltas[pick];
      dtmp.resize(n);
      for (uint32_t i = 0; i < n; ++i)
        dtmp[i] = (uint8_t)(src[i] - (i >= dlt ? src[i - dlt] : 0));
      bwt::encode_block(dtmp.data(), n, pay[1]);
    });
    for (auto &t : ths) t.join();
    best_bwt.swap(pay[0]);
    if (pay[1].size() < best_bwt.size()) {
      best_bwt.swap(pay[1]);
      best_delta = pick + 1;
    }
  });
  std::vector<uint8_t> cand[4];
  const bool user_fast = g_parse_fast != 0;
  // Phase 1: ONE parse (tight regime, word-shape prices) with refinement
  // passes, then all four mode emits from that plan.  The four modes differ
  // only in pricing; a single good plan emitted four ways keeps the
  // min-of-4 format while cutting parse work 4x.  user_fast -> tier 2:
  // beam 2, two passes.
  set_parse_fast(user_fast ? 2 : 1);
  {
    std::vector<uint32_t> litp_cache(n, 0);
    std::vector<PlanTok> plan, prev;
    SZ_TIC(tp);
    SZ_TIC(tp0);
    plan_block_pass(src, n, plan, nullptr, 2, cc, litp_cache);
    SZ_TOC(tp0, "pass 0 (unguided)");
    const size_t kmax_pass = (g_parse_fast >= 2) ? 2u : (size_t)SZ_PASSES;
    for (size_t k = 1; k < kmax_pass; ++k) {
      prev.clear();
      prev.swap(plan);
      SZ_TIC(tpg);
      plan_block_pass(src, n, plan, &prev, 2, cc, litp_cache);
      SZ_TOC(tpg, "guided pass");
      if (same_plan(plan, prev)) break;
    }
    SZ_TOC(tp, "passes (single)");
    SZ_TIC(te);
    {
      const unsigned ew = sz_workers() < 4 ? 2u : 4u;
      std::vector<std::thread> ths;
      for (unsigned t = 0; t < ew; ++t)
        ths.emplace_back([&, t] {
          for (uint32_t m = t; m < 4; m += ew)
            emit_plan(src, n, plan, cand[m], m);
        });
      for (auto &th : ths) th.join();
    }
    SZ_TOC(te, "emits (x4)");
  }
  set_parse_fast(user_fast ? 1 : 0);
  uint32_t best = 0;
  for (uint32_t k = 1; k < 4; ++k)
    if (cand[k].size() < cand[best].size()) best = k;
  const size_t cheap_best = cand[best].size();
  // (the BWT/delta family has been running concurrently since the top of
  // this function — join it now)
  bwt_thread.join();
  SZ_TOC(tbw, "bwt family (overlapped)");
  // Phase 2 (deep parse): run the expensive modes only when they could
  // still beat the cheap best (the deep parse historically buys ~0-0.5%).
  // Skip the deep wave when the cheap parse already dominates: BWT does not
  // beat it, or it is 2x smaller than BWT (deep cannot bridge a gap that big).
  const bool bwt_clearly_wins =
      (double)best_bwt.size() <= (double)cheap_best ||
      (double)cheap_best * 2.0 < (double)best_bwt.size();
  // Deep parse wave: measured to buy <=0.2% over the fast waves on the rows
  // where the LZ side wins, at 50-80% of encode time. Disabled for speed;
  // the fast waves x 4 modes + BWT family are the default.
  constexpr bool kDeepWave = false;
  if (kDeepWave && !user_fast && (n < 4096 || !bwt_clearly_wins)) {
    std::vector<uint8_t> deep[4];
    {
      std::thread ta([&] { encode_one_mode(src, n, 0, cc, deep[0]); });
      std::thread tb([&] { encode_one_mode(src, n, 1, cc, deep[1]); });
      ta.join();
      tb.join();
    }
    {
      std::thread t2([&] { encode_one_mode(src, n, 2, cc, deep[2]); });
      std::thread t3([&] { encode_one_mode(src, n, 3, cc, deep[3]); });
      t2.join();
      t3.join();
    }
    for (uint32_t k = 0; k < 4; ++k)
      if (deep[k].size() < cand[k].size()) cand[k].swap(deep[k]);
    best = 0;
    for (uint32_t k = 1; k < 4; ++k)
      if (cand[k].size() < cand[best].size()) best = k;
  }
  if (best_bwt.size() < cand[best].size()) {
    out.push_back((uint8_t)((best_delta << 3) | 4u));
    out.insert(out.end(), best_bwt.begin(), best_bwt.end());
    return;
  }
  out.push_back((uint8_t)best); // mode byte: (regime << 1) | mixer
  out.insert(out.end(), cand[best].begin(), cand[best].end());

}

int decode_block_szlzrc(const uint8_t *src, size_t src_len, uint8_t *dst,
                        uint32_t n) {
  if (src_len < 1) return SZIP_ERR_TRUNCATED;
  const uint32_t mode = src[0];
  const uint32_t delta_id = (mode >> 3) & 7u;
  static const uint32_t kDeltas[5] = {0, 1, 2, 4, 8};
  if (delta_id > 4) return SZIP_ERR_CORRUPT;
  const uint32_t delta = kDeltas[delta_id];
  if (mode & 4u) {
    // v6 BWT block codec
    if (bwt::decode_block(src + 1, static_cast<uint32_t>(src_len - 1), dst, n))
      return SZIP_ERR_CORRUPT;
    if (delta)
      for (uint32_t i = delta; i < n; ++i) dst[i] = (uint8_t)(dst[i] + dst[i - delta]);
    return SZIP_OK;
  }
  if (src_len < 6) return SZIP_ERR_TRUNCATED;
  RcDecoder rc(src + 1, src_len - 1);
  if (!rc.init()) return SZIP_ERR_TRUNCATED;

  Model m(mode >= 2 ? 2u : 0u);
  MixLit mix(mode & 1u);
  Hist hist;
  uint32_t out_pos = 0;
  uint32_t reps[3] = {0, 0, 0};
  uint8_t st = 0;

  while (out_pos < n) {
    if (rc.decode_bit(m.is_match) == 0) {
      const uint8_t mb = (uint8_t)((out_pos >= reps[0]) ? dst[out_pos - reps[0]] : 0);
      dst[out_pos] = mix.decode(rc, hist, mb, st);
      hist.push(dst[out_pos]);
      st = 0;
      ++out_pos;
    } else if (rc.decode_bit(m.is_rep) == 1) {
      uint32_t idx = 0;
      if (rc.decode_bit(m.rep_sel[0]) == 1)
        idx = (rc.decode_bit(m.rep_sel[1]) == 0) ? 1u : 2u;

      uint32_t xlen = 0;
      int e = m.regime
                  ? decode_gamma(rc, m.rlen_unary, m.rlen_bits, 21, &xlen)
                  : decode_gamma(rc, m.len_unary, m.len_bits, 21, &xlen);
      if (e != SZIP_OK) return e;
      if (!rc.ok()) return SZIP_ERR_TRUNCATED;
      const uint32_t len = xlen + 2;
      if (len < kMinMatch || len > SZIP_MAX_RUN) return SZIP_ERR_CORRUPT;
      const uint32_t off = reps[idx];
      if (off == 0 || off > out_pos) return SZIP_ERR_CORRUPT;
      if (len > n - out_pos) return SZIP_ERR_CORRUPT;

      reps_after_rep(reps, idx);
      st = 1;
      for (uint32_t i = 0; i < len; ++i) {
        const uint8_t b = dst[out_pos - off + i];
        dst[out_pos + i] = b;
        const uint8_t mb = (uint8_t)(((out_pos + i) >= reps[0]) ? dst[out_pos + i - reps[0]] : 0);
        mix.train_byte(hist, b, mb, st);
      }
      out_pos += len;
    } else {
      uint32_t off = 0;
      uint32_t xlen = 0;
      int e;
      if (m.regime) {
        // tight regime: length first, then class-aware offset
        e = decode_gamma(rc, m.nlen_unary, m.nlen_bits, 21, &xlen);
        if (e != SZIP_OK) return e;
        if (!rc.ok()) return SZIP_ERR_TRUNCATED;
        const uint32_t len = xlen + 2;
        if (len < kMinMatch || len > SZIP_MAX_RUN) return SZIP_ERR_CORRUPT;
        e = decode_offset(rc, m, len, &off);
        if (e != SZIP_OK) return e;
      } else {
        e = decode_offset(rc, m, 2, &off);
        if (e != SZIP_OK) return e;
        e = decode_gamma(rc, m.len_unary, m.len_bits, 21, &xlen);
        if (e != SZIP_OK) return e;
      }
      if (!rc.ok()) return SZIP_ERR_TRUNCATED;
      const uint32_t len = xlen + 2;
      if (len < kMinMatch || len > SZIP_MAX_RUN) return SZIP_ERR_CORRUPT;
      if (off == 0 || off > out_pos) return SZIP_ERR_CORRUPT;
      if (len > n - out_pos) return SZIP_ERR_CORRUPT;

      reps_after_normal(reps, off);
      st = 2;
      for (uint32_t i = 0; i < len; ++i) {
        const uint8_t b = dst[out_pos - off + i];
        dst[out_pos + i] = b;
        const uint8_t mb = (uint8_t)(((out_pos + i) >= reps[0]) ? dst[out_pos + i - reps[0]] : 0);
        mix.train_byte(hist, b, mb, st);
      }
      out_pos += len;
    }
    if (!rc.ok()) return SZIP_ERR_TRUNCATED;
  }
  if (!rc.ok()) return SZIP_ERR_TRUNCATED;
  if (rc.pos() != src_len - 1) return SZIP_ERR_CORRUPT;
  if (delta)
    for (uint32_t i = delta; i < n; ++i) dst[i] = (uint8_t)(dst[i] + dst[i - delta]);
  return SZIP_OK;
}

// ------------------------------------------------------------- le helpers --

void put_u32(std::ostream &os, uint32_t v) {
  char b[4] = {static_cast<char>(v & 0xFF), static_cast<char>((v >> 8) & 0xFF),
               static_cast<char>((v >> 16) & 0xFF),
               static_cast<char>((v >> 24) & 0xFF)};
  os.write(b, 4);
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

// v7 compact container: 4-byte magic ('SZP', 0x40|ver), then blocks of
// [type u8][raw u24][comp u24] + payload, then a 4-byte CRC32 at EOF.  The
// legacy v4-v6 container (20-byte header, 10-byte block headers, trailing
// end magic) is still read; new archives are always v7.
inline void write_compact_magic(std::ostream &os) {
  char hdr[4] = {(char)SZIP_MAGIC0, (char)SZIP_MAGIC1, (char)SZIP_MAGIC2,
                 (char)(SZIP_COMPACT_FLAG | SZIP_VERSION)};
  os.write(hdr, 4);
}
inline void write_compact_block(std::ostream &os, uint8_t type,
                                uint32_t raw_size, uint32_t comp_size) {
  char bh[SZIP_V7_BLOCK_HEADER_SIZE];
  bh[0] = (char)type;
  bh[1] = (char)(raw_size & 0xFF);
  bh[2] = (char)((raw_size >> 8) & 0xFF);
  bh[3] = (char)((raw_size >> 16) & 0xFF);
  bh[4] = (char)(comp_size & 0xFF);
  bh[5] = (char)((comp_size >> 8) & 0xFF);
  bh[6] = (char)((comp_size >> 16) & 0xFF);
  os.write(bh, SZIP_V7_BLOCK_HEADER_SIZE);
}

// Bare stored form (v6+): 'S','Z','P',(0x80|ver) + raw bytes to EOF.
// Selected when a whole file fits in one block and no codec shrinks it;
// 4-byte frame, no CRC (data is stored verbatim).
inline bool is_bare_magic(const unsigned char m[4]) {
  return m[0] == SZIP_MAGIC0 && m[1] == SZIP_MAGIC1 && m[2] == SZIP_MAGIC2 &&
         (m[3] & SZIP_BARE_FLAG) != 0;
}
inline int bare_version(const unsigned char m[4]) {
  return m[3] & ~SZIP_BARE_FLAG;
}
inline bool is_compact_magic(const unsigned char m[4]) {
  return m[0] == SZIP_MAGIC0 && m[1] == SZIP_MAGIC1 && m[2] == SZIP_MAGIC2 &&
         (m[3] & SZIP_COMPACT_FLAG) != 0 && (m[3] & SZIP_BARE_FLAG) == 0;
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
  if (ver < 4 || ver > SZIP_VERSION) return SZIP_ERR_UNSUPPORTED;
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

void set_parse_fast(int fast) { g_parse_fast = fast < 0 ? 0 : fast; }

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

  // Single-block files that no codec can shrink use the bare stored form
  // (4-byte frame).  Compressible single-block files get the normal
  // container around the one block (no re-encode).
  if (total <= block_size) {
    std::vector<uint8_t> raw((size_t)total);
    if (total > 0 &&
        !is.read(reinterpret_cast<char *>(raw.data()), (std::streamsize)total))
      return SZIP_ERR_IO;
    std::vector<uint8_t> payload;
    encode_block_szlzrc(raw.data(), static_cast<uint32_t>(total), payload);
    // Bare frame wins unless compression saves at least the 15-byte compact
    // overhead (bare file = raw + 4).
    if (payload.size() + 11 >= (size_t)total) {
      char hdr[4] = {(char)SZIP_MAGIC0, (char)SZIP_MAGIC1, (char)SZIP_MAGIC2,
                     (char)(SZIP_BARE_FLAG | SZIP_VERSION)};
      os.write(hdr, 4);
      if (total > 0)
        os.write(reinterpret_cast<const char *>(raw.data()),
                 (std::streamsize)total);
      return os ? SZIP_OK : SZIP_ERR_IO;
    }
    write_compact_magic(os);
    write_compact_block(os, SZIP_BLOCK_SZLZRC,
                        static_cast<uint32_t>(total),
                        static_cast<uint32_t>(payload.size()));
    os.write(reinterpret_cast<const char *>(payload.data()),
             (std::streamsize)payload.size());
    put_u32(os, szip_crc32(raw.data(), (size_t)total));
    return os ? SZIP_OK : SZIP_ERR_IO;
  }

  write_compact_magic(os);

  // Blocks are independent: encode a group of blocks in parallel, then
  // write them in order.  Output is byte-identical to the serial loop.
  uint32_t crc = 0;
  uint64_t remaining = total;
  const unsigned nw = sz_workers();
  std::vector<std::vector<uint8_t>> raws(nw), pays(nw);
  std::vector<uint32_t> wants(nw);
  while (remaining > 0) {
    const unsigned batch = (unsigned)std::min<uint64_t>(
        nw, (remaining + block_size - 1) / block_size);
    for (unsigned b = 0; b < batch; ++b) {
      wants[b] = static_cast<uint32_t>(
          std::min<uint64_t>(block_size, remaining));
      raws[b].resize(wants[b]);
      if (!is.read(reinterpret_cast<char *>(raws[b].data()), wants[b]))
        return SZIP_ERR_IO;
      remaining -= wants[b];
    }
    std::vector<std::thread> ths;
    for (unsigned b = 0; b < batch; ++b)
      ths.emplace_back([&, b] {
        encode_block_szlzrc(raws[b].data(), wants[b], pays[b]);
      });
    for (auto &t : ths) t.join();
    for (unsigned b = 0; b < batch; ++b) {
      crc = szip_crc32_update(crc, raws[b].data(), wants[b]);
      const bool store = pays[b].size() >= wants[b];
      const uint32_t comp_size =
          store ? wants[b] : static_cast<uint32_t>(pays[b].size());
      write_compact_block(os, store ? SZIP_BLOCK_STORED : SZIP_BLOCK_SZLZRC,
                          wants[b], comp_size);
      if (store)
        os.write(reinterpret_cast<const char *>(raws[b].data()), wants[b]);
      else
        os.write(reinterpret_cast<const char *>(pays[b].data()), comp_size);
      if (!os) return SZIP_ERR_IO;
    }
  }

  put_u32(os, crc);
  return os ? SZIP_OK : SZIP_ERR_IO;
}

// In-memory container encode/decode (used by the built-in GUI).
int compress_buffer(const uint8_t *data, size_t len,
                    std::vector<uint8_t> &out, uint32_t block_size) {
  if (block_size == 0 || block_size > (1u << 24)) return SZIP_ERR_FORMAT;
  if (len > (size_t)1 << 32) return SZIP_ERR_FORMAT;
  std::vector<uint8_t> payload;
  // Single-block bare/normal fast path (mirrors compress_file).
  if (len <= block_size) {
    encode_block_szlzrc(data, static_cast<uint32_t>(len), payload);
    if (payload.size() + 11 >= len) {
      out.resize(4 + len);
      out[0] = SZIP_MAGIC0; out[1] = SZIP_MAGIC1; out[2] = SZIP_MAGIC2;
      out[3] = (uint8_t)(SZIP_BARE_FLAG | SZIP_VERSION);
      if (len) std::memcpy(out.data() + 4, data, len);
      return SZIP_OK;
    }
    std::ostringstream os(std::ios::binary);
    write_compact_magic(os);
    write_compact_block(os, SZIP_BLOCK_SZLZRC, (uint32_t)len,
                        (uint32_t)payload.size());
    os.write((const char *)payload.data(), (std::streamsize)payload.size());
    put_u32(os, szip_crc32(data, len));
    const std::string &tmp = os.str();
    out.assign(tmp.begin(), tmp.end());
    return SZIP_OK;
  }
  std::ostringstream os(std::ios::binary);
  write_compact_magic(os);
  uint32_t crc = 0;
  size_t off = 0;
  const unsigned nw = sz_workers();
  std::vector<std::vector<uint8_t>> pays(nw);
  std::vector<const uint8_t *> blks(nw);
  std::vector<uint32_t> wants(nw);
  while (off < len) {
    const unsigned batch = (unsigned)std::min<size_t>(
        nw, (len - off + block_size - 1) / block_size);
    for (unsigned b = 0; b < batch; ++b) {
      blks[b] = data + off;
      wants[b] = static_cast<uint32_t>(
          std::min<size_t>(block_size, len - off));
      off += wants[b];
    }
    std::vector<std::thread> ths;
    for (unsigned b = 0; b < batch; ++b)
      ths.emplace_back([&, b] {
        encode_block_szlzrc(blks[b], wants[b], pays[b]);
      });
    for (auto &t : ths) t.join();
    for (unsigned b = 0; b < batch; ++b) {
      crc = szip_crc32_update(crc, blks[b], wants[b]);
      const bool store = pays[b].size() >= wants[b];
      const uint32_t comp_size =
          store ? wants[b] : static_cast<uint32_t>(pays[b].size());
      write_compact_block(os, store ? SZIP_BLOCK_STORED : SZIP_BLOCK_SZLZRC,
                          wants[b], comp_size);
      if (store) os.write((const char *)blks[b], wants[b]);
      else os.write((const char *)pays[b].data(), comp_size);
    }
  }
  put_u32(os, crc);
  const std::string &tmp = os.str(); // ostringstream::str() returns a copy
  out.assign(tmp.begin(), tmp.end());
  return SZIP_OK;
}

// Walk a compact-v7 block stream (caller has consumed the 4-byte magic; pos
// accounting starts at 4).  Blocks: [type u8][raw u24][comp u24] + payload,
// until exactly the trailing 4-byte CRC32 remains.  sink_vec / sink_os
// receive decoded bytes; both null = validate only.  Hostile-input safe.
int walk_blocks_v7(std::istream &is, uint64_t stream_len,
                   std::vector<uint8_t> *sink_vec, std::ostream *sink_os,
                   uint32_t *crc_out, uint64_t *produced_out) {
  uint32_t crcc = 0;
  uint64_t produced = 0;
  uint64_t pos = 4;
  const unsigned nw = sz_workers();
  // Groups of blocks are read sequentially, decoded in parallel, and
  // emitted in order — byte-identical to the serial walk.
  struct Blk {
    uint8_t type;
    uint32_t raw_size, comp_size;
    std::vector<uint8_t> payload;
  };
  while (pos + 4 < stream_len) {
    std::vector<Blk> group;
    group.reserve(nw);
    while (group.size() < nw && pos + 4 < stream_len) {
      unsigned char bh[SZIP_V7_BLOCK_HEADER_SIZE];
      if (!is.read(reinterpret_cast<char *>(bh), SZIP_V7_BLOCK_HEADER_SIZE))
        return SZIP_ERR_TRUNCATED;
      pos += SZIP_V7_BLOCK_HEADER_SIZE;
      Blk b;
      b.type = bh[0];
      b.raw_size = (uint32_t)bh[1] | ((uint32_t)bh[2] << 8) |
                   ((uint32_t)bh[3] << 16);
      b.comp_size = (uint32_t)bh[4] | ((uint32_t)bh[5] << 8) |
                    ((uint32_t)bh[6] << 16);
      if (b.raw_size == 0 || b.comp_size == 0) return SZIP_ERR_CORRUPT;
      if (pos + (uint64_t)b.comp_size + 4 > stream_len) return SZIP_ERR_CORRUPT;
      if (b.type == SZIP_BLOCK_STORED) {
        if (b.comp_size != b.raw_size) return SZIP_ERR_CORRUPT;
      } else if (b.type == SZIP_BLOCK_SZLZRC) {
        if (b.comp_size < 5) return SZIP_ERR_CORRUPT;
      } else {
        return SZIP_ERR_UNSUPPORTED;
      }
      b.payload.resize(b.comp_size);
      if (!is.read(reinterpret_cast<char *>(b.payload.data()), b.comp_size))
        return SZIP_ERR_TRUNCATED;
      pos += b.comp_size;
      group.push_back(std::move(b));
    }
    std::vector<std::vector<uint8_t>> raws(group.size());
    std::vector<int> errs(group.size(), 0);
    std::vector<std::thread> ths;
    for (size_t gi = 0; gi < group.size(); ++gi) {
      if (group[gi].type == SZIP_BLOCK_STORED) continue;
      ths.emplace_back([&, gi] {
        raws[gi].resize(group[gi].raw_size);
        errs[gi] = decode_block_szlzrc(group[gi].payload.data(),
                                       group[gi].comp_size, raws[gi].data(),
                                       group[gi].raw_size);
      });
    }
    for (auto &t : ths) t.join();
    for (size_t gi = 0; gi < group.size(); ++gi) {
      const Blk &b = group[gi];
      const uint8_t *raw;
      if (b.type == SZIP_BLOCK_STORED) {
        raw = b.payload.data();
      } else {
        if (errs[gi] != SZIP_OK) return errs[gi];
        raw = raws[gi].data();
      }
      crcc = szip_crc32_update(crcc, raw, b.raw_size);
      if (sink_vec) sink_vec->insert(sink_vec->end(), raw, raw + b.raw_size);
      if (sink_os &&
          !sink_os->write(reinterpret_cast<const char *>(raw), b.raw_size))
        return SZIP_ERR_IO;
      produced += b.raw_size;
    }
  }
  if (pos + 4 != stream_len) return SZIP_ERR_CORRUPT;
  uint32_t stored_crc = 0;
  if (!get_u32(is, &stored_crc)) return SZIP_ERR_TRUNCATED;
  if (stored_crc != crcc) return SZIP_ERR_CORRUPT;
  if (crc_out) *crc_out = crcc;
  if (produced_out) *produced_out = produced;
  return SZIP_OK;
}

int decompress_buffer(const uint8_t *data, size_t len,
                      std::vector<uint8_t> &out) {
  if (len < 4) return SZIP_ERR_TRUNCATED;
  const unsigned char m[4] = {data[0], data[1], data[2], data[3]};
  if (is_bare_magic(m)) {
    const int ver = bare_version(m);
    if (ver < 4 || ver > SZIP_VERSION) return SZIP_ERR_UNSUPPORTED;
    out.assign(data + 4, data + len);
    return SZIP_OK;
  }
  std::istringstream is(std::string((const char *)data, len), std::ios::binary);
  if (is_compact_magic(m)) {
    if ((m[3] & 0x3F) > SZIP_VERSION) return SZIP_ERR_UNSUPPORTED;
    is.seekg(4);
    return walk_blocks_v7(is, len, &out, nullptr, nullptr, nullptr);
  }
  uint32_t block_size = 0;
  uint64_t total = 0;
  int e = read_and_check_header(is, &block_size, &total);
  if (e != SZIP_OK) return e;
  if (total > (size_t)1 << 32) return SZIP_ERR_FORMAT;
  out.clear();
  out.reserve((size_t)total);
  std::vector<uint8_t> raw;
  uint64_t remaining = total;
  while (remaining > 0) {
    uint8_t type = 0; uint32_t raw_size = 0, comp_size = 0;
    e = read_and_check_block_header(is, &type, &raw_size, &comp_size);
    if (e != SZIP_OK) return e;
    if (raw_size == 0 || raw_size > block_size || raw_size > remaining)
      return SZIP_ERR_CORRUPT;
    if (type == SZIP_BLOCK_STORED) {
      raw.resize(raw_size);
      if (!is.read((char *)raw.data(), raw_size)) return SZIP_ERR_TRUNCATED;
    } else if (type == SZIP_BLOCK_SZLZRC) {
      std::vector<uint8_t> payload(comp_size);
      if (!is.read((char *)payload.data(), comp_size)) return SZIP_ERR_TRUNCATED;
      raw.resize(raw_size);
      e = decode_block_szlzrc(payload.data(), comp_size, raw.data(), raw_size);
      if (e != SZIP_OK) return e;
    } else return SZIP_ERR_CORRUPT;
    out.insert(out.end(), raw.begin(), raw.end());
    remaining -= raw_size;
  }
  uint32_t crc = 0;
  if (!get_u32(is, &crc)) return SZIP_ERR_TRUNCATED;
  char magic[4];
  if (!is.read(magic, 4) || magic[0] != SZIP_MAGIC0 || magic[1] != SZIP_MAGIC1 ||
      magic[2] != SZIP_MAGIC2 || magic[3] != SZIP_MAGIC3)
    return SZIP_ERR_FORMAT;
  if (crc != szip_crc32(out.data(), out.size())) return SZIP_ERR_CORRUPT;
  return SZIP_OK;
}

int decompress_file(const std::string &in_path, const std::string &out_path,
                    uint64_t *raw_out, uint32_t *crc_out) {
  std::ifstream is(in_path, std::ios::binary);
  if (!is) return SZIP_ERR_IO;

  unsigned char magic[4];
  if (!is.read(reinterpret_cast<char *>(magic), 4)) return SZIP_ERR_TRUNCATED;
  if (is_bare_magic(magic)) {
    const int ver = bare_version(magic);
    if (ver < 4 || ver > SZIP_VERSION) return SZIP_ERR_UNSUPPORTED;
    std::ofstream os(out_path, std::ios::binary | std::ios::trunc);
    if (!os) return SZIP_ERR_IO;
    std::vector<char> buf(1 << 16);
    uint64_t produced = 0;
    while (is.read(buf.data(), (std::streamsize)buf.size()) || is.gcount() > 0) {
      const std::streamsize got = is.gcount();
      if (!os.write(buf.data(), got)) return SZIP_ERR_IO;
      produced += (uint64_t)got;
    }
    if (raw_out) *raw_out = produced;
    if (crc_out) *crc_out = 0; // bare form stores data verbatim (no checksum)
    return os ? SZIP_OK : SZIP_ERR_IO;
  }
  if (is_compact_magic(magic)) {
    if ((magic[3] & 0x3F) > SZIP_VERSION) return SZIP_ERR_UNSUPPORTED;
    is.seekg(0, std::ios::end);
    const uint64_t flen = (uint64_t)is.tellg();
    is.seekg(4);
    std::ofstream os(out_path, std::ios::binary | std::ios::trunc);
    if (!os) return SZIP_ERR_IO;
    uint32_t crc = 0;
    uint64_t produced = 0;
    const int we = walk_blocks_v7(is, flen, nullptr, &os, &crc, &produced);
    if (we != SZIP_OK) return we;
    if (raw_out) *raw_out = produced;
    if (crc_out) *crc_out = crc;
    return os ? SZIP_OK : SZIP_ERR_IO;
  }
  is.seekg(0);

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

  unsigned char magic[4];
  if (!is.read(reinterpret_cast<char *>(magic), 4)) return SZIP_ERR_TRUNCATED;
  if (is_bare_magic(magic)) {
    const int ver = bare_version(magic);
    if (ver < 4 || ver > SZIP_VERSION) return SZIP_ERR_UNSUPPORTED;
    is.seekg(0, std::ios::end);
    const uint64_t produced = (uint64_t)is.tellg() - 4;
    if (raw_out) *raw_out = produced;
    return SZIP_OK; // verbatim storage: nothing further to validate
  }
  if (is_compact_magic(magic)) {
    if ((magic[3] & 0x3F) > SZIP_VERSION) return SZIP_ERR_UNSUPPORTED;
    is.seekg(0, std::ios::end);
    const uint64_t flen = (uint64_t)is.tellg();
    is.seekg(4);
    uint64_t produced = 0;
    const int we = walk_blocks_v7(is, flen, nullptr, nullptr, nullptr, &produced);
    if (we != SZIP_OK) return we;
    if (raw_out) *raw_out = produced;
    return SZIP_OK;
  }
  is.seekg(0);

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
