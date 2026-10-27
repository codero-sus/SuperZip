import re
p = 'native/codec.cpp'
s = open(p).read()

# all candidates get full length sets again (restore the 17,018 config)
s = s.replace("  const uint32_t fine = rank == 0 ? (SZ_LCAP ? 64u : 4096u) : 40u;",
              "  const uint32_t fine = 4096u; // full sets for every candidate (rank kept)")
s = s.replace('static void collect_lens(uint32_t max_len, uint32_t rank,\n                         std::vector<uint32_t> &lens) {',
              'static void collect_lens(uint32_t max_len, uint32_t rank,\n                         std::vector<uint32_t> &lens) {\n  (void)rank;')

old = '''  auto try_insert = [&](uint32_t q, DPNode nd) {
    auto &lst = best[q];
    // scan by reference: pool is not mutated during the scan
    for (uint32_t idx : lst) {
      const DPNode &o = pool[idx];
      if (o.reps[0] == nd.reps[0] && o.reps[1] == nd.reps[1] &&
          o.reps[2] == nd.reps[2]) {
        if (nd.cost < o.cost) {
          pool.push_back(nd);
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
  };'''
new = '''  auto try_insert = [&](uint32_t q, const DPNode &nd) {
    auto &lst = best[q];
    // scan by reference: pool is not mutated during the scan
    for (uint32_t idx : lst) {
      const DPNode &o = pool[idx];
      if (o.reps[0] == nd.reps[0] && o.reps[1] == nd.reps[1] &&
          o.reps[2] == nd.reps[2]) {
        if (nd.cost < o.cost) {
          pool.push_back(nd); // single copy, only on the winning path
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
  };'''
assert old in s, 'try_insert'
s = s.replace(old, new)

s = s.replace('  pool.reserve((size_t)n + 16);', '  pool.reserve((size_t)n * 6 + 64);')

old = '''          const uint32_t rep_prefix =
              price_bit(ms.is_match, 1) + price_bit(ms.is_rep, 1) +
              price_bit(ms.rep_sel[0], rep.idx == 0 ? 0u : 1u) +
              (rep.idx != 0 ? price_bit(ms.rep_sel[1], rep.idx == 1 ? 0u : 1u) : 0u);
          collect_lens(rep.len, 0, lens);
          for (uint32_t l : lens) {
            DPNode nd = A;
            nd.cost = A.cost + rep_prefix +
                      price_gamma_p(ms.len_unary, ms.len_bits, l - 2);
            nd.len = l;
            nd.off = 0;
            nd.rep_idx = rep.idx;
            reps_after_rep(nd.reps, rep.idx);
            try_insert(pos + l, nd);
          }'''
new = '''          const uint32_t rep_prefix =
              price_bit(ms.is_match, 1) + price_bit(ms.is_rep, 1) +
              price_bit(ms.rep_sel[0], rep.idx == 0 ? 0u : 1u) +
              (rep.idx != 0 ? price_bit(ms.rep_sel[1], rep.idx == 1 ? 0u : 1u) : 0u);
          collect_lens(rep.len, 0, lens);
          DPNode nd = A;
          nd.off = 0;
          nd.rep_idx = rep.idx;
          reps_after_rep(nd.reps, rep.idx);
          for (uint32_t l : lens) {
            nd.cost = A.cost + rep_prefix +
                      price_gamma_p(ms.len_unary, ms.len_bits, l - 2);
            nd.len = l;
            try_insert(pos + l, nd);
          }'''
assert old in s, 'rep loop'
s = s.replace(old, new)

old = '''        const uint32_t norm_prefix = price_bit(ms.is_match, 1) +
                                     price_bit(ms.is_rep, 0) +
                                     price_offset_p(ms, nm.off);
        collect_lens(nm.len, ci, lens);
        for (uint32_t l : lens) {
          DPNode nd = A;
          nd.cost = A.cost + norm_prefix +
                    price_gamma_p(ms.len_unary, ms.len_bits, l - 2);
          nd.len = l;
          nd.off = nm.off;
          nd.rep_idx = UINT32_MAX;
          reps_after_normal(nd.reps, nm.off);
          try_insert(pos + l, nd);
        }'''
new = '''        const uint32_t norm_prefix = price_bit(ms.is_match, 1) +
                                     price_bit(ms.is_rep, 0) +
                                     price_offset_p(ms, nm.off);
        collect_lens(nm.len, ci, lens);
        DPNode nd = A;
        nd.off = nm.off;
        nd.rep_idx = UINT32_MAX;
        reps_after_normal(nd.reps, nm.off);
        for (uint32_t l : lens) {
          nd.cost = A.cost + norm_prefix +
                    price_gamma_p(ms.len_unary, ms.len_bits, l - 2);
          nd.len = l;
          try_insert(pos + l, nd);
        }'''
assert old in s, 'norm loop'
s = s.replace(old, new)

open(p, 'w').write(s)
print('storm optimizations in')
