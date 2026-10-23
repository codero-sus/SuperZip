# G0D SZIP — `.szip` Container Format (v3)

Reference project: **SuperZip / G0D SZIP**. This document is normative for the
byte layout of `.szip` archives and the SZLZRC v3 bitstream.

Design goals:

1. **Own everything** — encoder, decoder, entropy coder, match finder and CRC
   are original implementations (C++ engine `native/` and Rust engine
   `rust/szip-core/`). No third-party compression code.
2. **Deterministic** — same input + same settings ⇒ byte-identical output.
3. **Hostile-input safe** — every length is validated; the decoder never reads
   or writes out of bounds.
4. **Extensible** — codec IDs and block framing allow future "GOD stages"
   without breaking v3 files.

All multi-byte integers are **little-endian**. All arithmetic below is exact;
where machine integer wraparound is intended it is written `mod 2^32`.

---

## 1. File layout

```
header (20 bytes) | block* | footer (8 bytes)
```

### 1.1 Header (20 bytes)

| off | size | field |
|-----|------|-------|
| 0   | 4    | magic `"SZP1"` |
| 4   | 1    | `version` = `3` |
| 5   | 1    | flags (bit0 = stream CRC present; always 1) |
| 6   | 2    | reserved, zero |
| 8   | 8    | `raw_size` (total uncompressed bytes, LE u64) |

### 1.2 Block framing

Each block: `type` (1 byte), `pad` (1 byte, zero), `comp_size` (LE u24,
bytes after the 4-byte prefix), payload (`comp_size` bytes).

| type | codec |
|------|-------|
| 0    | stored (raw copy) |
| 1    | SZLZRC v3 payload |
| other| reserved |

Sum of block `raw_size` fields must equal the header `raw_size`.

### 1.3 Footer (8 bytes)

`stream_crc` (LE u32, CRC-32/ISO-HDLC of the raw stream) and `reserved`
(LE u32, zero).

---

## 2. Block codec: SZLZRC v3 (`type = 1`)

**S**uper**Z**ip **LZ**77 + **R**ange **C**oder + context mixing, v3.

Pipeline: LZ77 factorization (hash chains, price-aware parse, 1 MiB window,
last-3 rep offsets) → token stream → adaptive binary range coder (carryless,
11-bit probabilities). **Literals** are bit-coded through the integer
context-mixing model of §2.4 ("GOD stage"); token syntax and match/rep coding
are as in SZLZRC v2.

### 2.1 Range coder

State: `low` (u64), `range` (u32, init `0xFFFFFFFF`), `code` (u32, decoder).

Bit coding with probability `p` (u16, init `1024`, updated with shift 5):

```
bound = (range >> 11) * p
bit 0:  range = bound;         p += (2048 - p) >> 5
bit 1:  low += bound; range -= bound; p -= p >> 5   (decoder: code -= bound)
normalize while range < 2^24:  range <<= 8; emit/consume one byte
```

Encoder writes a 5-byte flush tail (`shiftLow` x5). The decoder pre-reads 5
bytes (first must be `0`) and must finish having consumed the payload exactly.

**Fixed-probability bits** (used by the literal mixer): identical bound
arithmetic, but `p` is supplied externally and is *not* updated by the coder.
`p` is the probability that the bit is `0`, in `[1, 2047]`.

### 2.2 Token models (all u16, init `1024`, self-updating as in §2.1)

- `is_match` — literal vs match flag
- `is_rep` — rep match vs normal match
- `rep_sel[2]` — rep index: bit0 `0`→rep0, `1`→rep1/2; bit1 `0`→rep1, `1`→rep2
- `len_unary[12]`, `len_bits[10]` — Elias gamma of `len - 2`
- `off_mode` — offset mode flag
- `tiny_unary[8]`, `tiny_bits[8]` — Elias gamma of `off` (tiny mode)
- `slot_unary[12]`, `slot_bits[10]` — Elias gamma of `n - 2` (slot mode)
- `off_direct[19]` — mantissa bits of `off - 1`

Elias gamma value `x >= 1`, `n = floor(log2 x)`: `n` one-bits, one zero-bit,
then the `n` low bits of `x` MSB-first. Each position `i` uses its own
adaptive probability (`unary[i]`, `bits[i]`).

### 2.3 Token stream

Decoder state: history `hist` (last 5 raw bytes, all `0` initially, plus
`wpos` = bytes since the last space byte `0x20`, clamped to `7`, init `0`),
`rep0,rep1,rep2` (1-based offsets, `0` = none), all zero initially.

```
repeat until raw_size bytes are produced:
  is_match?
  0 -> literal:
       byte = MIXDECODE(hist)                      (section 2.4)
       push(hist, byte)
  1 -> is_rep?
       1 -> rep match:
            rep_sel bits select idx in {0,1,2}
            len  = gamma(len_unary/len_bits) + 2      in [3, 2048]
            offset = rep[idx]                          (must be != 0, <= out_pos)
            rep update: if idx > 0 { tmp=rep[idx]; shift; rep0=tmp }
       0 -> normal match:
            offset code (below)
            len  = gamma(len_unary/len_bits) + 2      in [3, 2048]
            rep update: rep2=rep1; rep1=rep0; rep0=offset
       copy len bytes from out_pos - offset (overlap allowed)
       for each copied byte b in order: MIXTRAIN(hist, b); push(hist, b)
```

`push(hist, b)`: `hist.b[4..1] = hist.b[3..0]; hist.b[0] = b;` then
`wpos = (b == 32) ? 0 : min(wpos + 1, 7)`.

**Offset code** (`u = off - 1`, `n = bitlen(u)`):

- `u < 4` (off 1..4): `off_mode = 0`, then `gamma(off)`
- else: `off_mode = 1`, then `gamma(n - 2)`, then `n - 1` mantissa bits of `u`
  (bit `i` coded with `off_direct[i]`, `i` from `n-2` down to `0`; the top bit
  of `u` is implicitly `1`)

`off` must decode to `[1, 2^20]`.

### 2.4 Literal mixer ("GOD stage", normative)

All probabilities below are integers in `[1, 2047]` (11-bit, `1024` = 50%).
All hashing multiplies are `uint32` (`mod 2^32`); `>>` on negative values is
an arithmetic shift.

**Fixed-point tables** (generated; reference implementation
`native/tables.hpp`, `rust/szip-core/src/tables.rs`):

- `STRETCH[2048]` (i32): `STRETCH[p] = clamp(round(log2(p/(2048-p)) * 4096),
  -45056, 45056)` for `p` in 1..2047, `STRETCH[0] = -45056`.
- `SQUASH[5633]` (i16): `SQUASH[i] = clamp(round(2048 / (1 + 2^(-x/4096))),
  1, 2047)` with `x = i*16 - 45056`.
- `BIT_PRICE[2049]` (u16): `BIT_PRICE[p] = clamp(round(-log2(p/2048) * 16),
  1, 2048)` (1/16-bit units; encoder-only).

**Model state**: six binary trees `t0..t5`, each `num_ctx × 256` u16 nodes
indexed `[ctx * 256 + sym]`, `sym` in 1..255 during the walk (leaf `sym = 0`
is never indexed). All nodes init `1024`.

Contexts at literal time (`b0..b4 = hist.b[0..4]`, `wp = wpos`):

```
c0 = b0 >> 3                                             (32 ctx)
c1 = (b0*2654435761 ^ b1*40503 ^ wp*2246822519) >> 19     (8192 ctx)
c2 = (b0*2654435761 ^ b1*40503 ^ b2*805459861 ^ wp*2246822519) >> 15
c3 = (b0*2654435761 ^ b1*40503 ^ b2*805459861 ^ b3*1013904223 ^ wp*2246822519) >> 15
c4 = (b0*2654435761 ^ b1*40503 ^ b2*805459861 ^ b3*1013904223
      ^ b4*2246822519 ^ wp*3266489917) >> 15               (131072 ctx)
c5 = (wp << 13) | (b0 << 5) | (b1 >> 3)                   (65536 ctx)
```

Tree sizes: `t0` 32, `t1` 8192, `t2` 131072, `t3` 131072, `t4` 131072,
`t5` 65536 contexts.

**Mixer**: weights `w[64][6]` (i32), all init `1365` (= `2^13 / 6`, equal
mix). Weight context `wk = bitpos + 8 * (b0 >> 5)` where `bitpos` counts the
coded bits of the byte MSB-first: the first bit (bit 7) uses `bitpos = 0`,
…, the last (bit 0) uses `bitpos = 7`.

Per bit of a literal byte, `sym` starting at 1 (MSB first, `bit` is the bit):

```
p_i   = t_i[ctx_i * 256 + sym]                 (p_4 valid only if tree exists)
x     = Σ_i w[wk][i] * STRETCH[p_i]            >> 13  (arithmetic)
x     = clamp(x, -45056, 45056)
p_mix = SQUASH[(x + 45056) >> 4]
RC fixed-prob bit with p = p_mix               (MIXDECODE reads the bit here)
// updates, in this exact order:
if bit == 0:  p_i += (2048 - p_i) >> 5         for every tree i
else:         p_i -= p_i >> 5
err = bit == 0 ? 2048 - p_mix : -p_mix
for i in 0..5:
    w[wk][i] = clamp(w[wk][i] + ((err * STRETCH[p_i_after_update]) >> 17),
                     -2^20, 2^20)
sym = (sym << 1) | bit
```

`MIXTRAIN(hist, b)` performs the same per-bit walk and updates (trees and
weights) with `bit` taken from `b`, without touching the range coder; it does
not push `hist` (the caller does). Encoder and decoder must call
`MIXTRAIN` for every match-covered byte so both stay in lock-step.

### 2.5 Encoder parse (advice; decoder-agnostic)

The parse policy is **not** part of the bitstream contract — any factorization
respecting the grammar decodes correctly. For deterministic output and
reference ratios, the shipped encoders use:

- Match finder: 3-byte hash (`v * 2654435761 >> 14`), 18-bit table, hash
  chains, window `2^20`, chain depth `400`, max match `2048`, nice length
  `128`. Candidate selection is price-aware: score `= len*8 - bitlen(off)*3`,
  highest wins.
- At each position the encoder also measures rep matches against the current
  `rep0..2` (longest wins; lowest index on ties).
- Rep vs normal: rep wins if longer; otherwise by price
  `3 + gamma_price(len_r - 2) + 5*(len_n - len_r) < gamma_price(off) +
  gamma_price(len_n - 2)` where `gamma_price(x) = 2*floor(log2 x) + 1`.
- A match is rejected (encoded as literals) when
  `est * bias > lit_peek` where `est` is the estimated token price in bits,
  `lit_peek` is the mixer's price in 1/16 bits of coding the same span as
  literals (§2.4 `BIT_PRICE`, evaluated read-only), and `bias` is a tuning
  constant.
- Multi-parse: the encoder tries `bias ∈ {13, 16, 20}` × lazy-depth
  `{1, 2, 4}`, keeping the smallest payload. The decoder cannot observe
  which parse produced a stream.
- One-step lazy: if the best match at `pos+1` is longer than the one at `pos`
  (by at least 1), emit a literal at `pos`.

### 2.6 Stored blocks (`type = 0`)

Raw bytes verbatim. The encoder picks stored whenever the compressed payload
would not be smaller than `raw_size` (incompressible data never expands).

---

## 3. Integrity

The decoder rejects: bad magic/version, truncation, `raw_size`/`comp_size`
violations, range-coder over/under-consumption, invalid offsets/lengths, and a
footer CRC mismatch. A corrupted archive must never cause out-of-bounds access.

## 4. Versioning

`version = 1` (2026-10-23, pre-release) used SZLZRC v1 (no rep offsets, plain
gamma offsets). `version = 2` used SZLZRC v2 (rep offsets + price-aware
parse + order-1 literal trees). Both are rejected by the version field.
`version = 3` replaces literal coding with the §2.4 context mixer; token
syntax is unchanged from v2. Future GOD stages get new block `type` IDs or a
`version = 4` bump.
