# G0D SZIP — `.szip` Container Format (v7)

Reference project: **SuperZip / G0D SZIP**. This document is normative for the
byte layout of `.szip` archives and the SZLZRC v7 bitstream.

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

A v7 archive is one of three forms, chosen by the 4th magic byte:

| byte 3 | form | written when |
|--------|------|--------------|
| `0x40 \| ver` | **compact v7 container** (this section) | compression saved ≥ 12 bytes |
| `0x80 \| ver` | **bare stored form** (§1.2) | incompressible / savings < 12 bytes |
| `0x31` (ver 4–6) | **legacy container** (§1.3) | never written; read-only support |

Decoders route on the flags: bit 7 set ⇒ bare; bit 6 set ⇒ compact; exact
`0x31` ⇒ legacy. Bit 7 and bit 6 are mutually exclusive.

### 1.1 Compact v7 container (default)

```
magic (4 bytes) | block* | stream CRC32 (4 bytes, LE) at EOF
```

Magic: `'S','Z','P', (0x40 | 7)`.  Each block header is **7 bytes**:
`type` (1), `raw_size` (LE u24), `comp_size` (LE u24), then `comp_size`
payload bytes.  `raw_size` must be in `[1, 2^24-1]`, `comp_size >= 1` (and
`== raw_size` for stored blocks).  Blocks continue while more than 4 bytes
remain in the file; the last 4 bytes are the stream CRC32 (ISO-HDLC of the
raw stream).  There is no end magic, no `total_raw` and no `block_size` —
the reader walks blocks to EOF-minus-4.  A single-block archive therefore
costs **15 bytes** of overhead (4 magic + 7 block header + 4 CRC), 23 bytes
less than the legacy container.

| type | codec |
|------|-------|
| 0    | stored (raw copy, `comp_size == raw_size`) |
| 1    | SZLZRC payload: 1 mode byte then the RC stream (see §2) |
| other| rejected |

Block boundaries are a pure encoder choice (bounded by 16 MiB raw per block);
the decoder is independent of them.

### 1.2 Bare stored form (v6+)

When compression cannot save at least 12 bytes (`payload + 11 >= total_raw`),
the encoder writes the **bare stored form** instead of the container above:

```
magic "SZP" (3 bytes) | flags+version (1 byte) | raw bytes to EOF
```

The 4th byte is `0x80 | version` (`SZIP_BARE_FLAG | SZIP_VERSION`).  There is
no block framing, no length field and no CRC — the payload is the file,
verbatim.  Decoders detect the form by `"SZP"` and the high bit of byte 3;
v4/v5 decoders reject it on the magic.  Rationale: incompressible input is
stored exactly as-is with a 4-byte frame, the smallest honest overhead in the
field (a brotli/zstd stored stream needs 5+ bytes), and the whole-file
expansion of G0D SZIP is therefore bounded at +4 bytes.

### 1.3 Legacy container (v4–v6, read-only)

v4–v6 archives are still decoded byte-exactly:

```
header (20 bytes) | block* | footer (8 bytes)
```

Header: magic `"SZP1"` (4), `version` (1, 4–6), flags (1, zero), reserved
(2), `block_size` (LE u32, `1..2^24`), `total_raw` (LE u64).  Block headers
are 10 bytes LE: `type` (1), `pad` (1, zero), `raw_size` (LE u32),
`comp_size` (LE u32), then `comp_size` payload bytes; the sum of `raw_size`
must equal `total_raw`.  Footer: stream CRC32 (LE u32) and the end magic
`"SZP1"` (4 bytes).  G0D SZIP no longer writes this form.

---

## 2. Block codec: SZLZRC v7 (`type = 1`)

**S**uper**Z**ip **LZ**77 + **R**ange **C**oder + context mixing.

The payload begins with one **mode byte**:

```
bit 0    mixer        (LZ path)
bit 1    regime       (LZ path)
bit 2    codec: 0 = SZLZRC (LZ), 1 = BWT (see §2.7)
bits 3-5 delta id    (0 = none, 1 = d1, 2 = d2, 3 = d4, 4 = d8; see below)
         7 = **stored sub-mode** (bit 2 must be 0): payload is exactly
         1 + raw_size bytes = mode byte + the raw block.  Emitted by the
         incompressible pre-reject; containers with a raw/stored block type
         strip the mode byte and store the raw block instead.  ids 5-6
         are reserved and decoders must reject them.
bit 6    **column transform flag** (v6.1, see §2.8): the rest of the mode
         byte is ignored; the payload is a column-stream container
bit 7    reserved, zero
```

LZ path (bit 2 = 0), `mode = (regime << 1) | mixer`:

| mode | regime | mixer | availability |
|------|--------|-------|--------------|
| 0 | v4 token models | word-shape t5 | v4 + v5 decoders |
| 1 | v4 token models | gap order-1 t5 | v4 + v5 decoders |
| 2 | tight token models | word-shape t5 | v5 decoders |
| 3 | tight token models | gap order-1 t5 | v5 decoders |

**Byte-delta transform (v6):** when the delta id is non-zero the decoded
block bytes `p` satisfy `p[i] = raw[i] - raw[i-d] (mod 256)` with `d` the
delta distance (1, 2, 4 or 8); the decoder restores `raw` in place with
`raw[i] = p[i] + raw[i-d]`.  First `d` bytes are stored undifferenced.

A v5 encoder codes all four LZ modes per block and stores the smallest. v4
files (header version 4, mode 0/1) decode byte-exactly with the v5 and v6
decoders.  A v6 encoder additionally codes BWT candidates (raw block and
each of the four delta transforms) and ships the smallest of all; v4/v5
decoders reject v6 streams on the version field.

Pipeline: LZ77 factorization (hash chains, price-aware parse, 1 MiB window,
last-3 rep offsets) → token stream → adaptive binary range coder (carryless,
11-bit probabilities). **Literals** are bit-coded through the integer
context-mixing model of §2.4 ("GOD stage"); token syntax and match/rep coding
are as in SZLZRC v2.

### 2.8 Column transform (v6.1)

Delimiter-aligned columnar text (every complete line carries exactly K
delimiter bytes, 1 ≤ K ≤ 8; delimiter is `TAB`, `,` or `"`) is split into
K+1 per-column streams, each an independent SZLZRC block.  Word columns
collapse; numeric columns stay near entropy.  The encoder stores the
columnar form only when it is smaller than the plain form.

Payload after the mode byte:

```
u8   delimiter byte
u8   ncols (K+1)
u32  tail_len   (raw trailing bytes after the last '\n', stored verbatim at the end)
repeat ncols times:
    u32 raw_len   (column length, fields joined by '\n')
    u32 comp_len  (compressed column payload)
    comp_len bytes: an SZLZRC block payload (its own mode byte)
tail_len bytes: the raw tail
```

Decoder: decode each column, split each at `'\n'` into `lines` fields,
re-interleave `field0 + delim + ... + fieldK + '\n'` per line, then append
the tail.  `lines` = the newline count of column 0.  Reconstruction must
consume every column byte and reproduce the block size exactly.

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
- `len_unary[22]`, `len_bits[22]` — Elias gamma of `len - 2` (regime 0, shared)
- regime 2 only: `nlen_unary[22]`/`nlen_bits[22]` (normal-match length) and
  `rlen_unary[22]`/`rlen_bits[22]` (rep length) replace the shared model
- regime 2 only: `slot_cls[4][12]`/`slotb_cls[4][10]` — slot model per
  length class `min(len - 2, 3)` (replaces the shared slot model)
- `off_mode` — offset mode flag
- `tiny_unary[8]`, `tiny_bits[8]` — Elias gamma of `off` (tiny mode)
- `slot_unary[12]`, `slot_bits[10]` — Elias gamma of `n - 2` (slot mode)
- `off_direct[19]` — mantissa bits of `off - 1`

Elias gamma value `x >= 1`, `n = floor(log2 x)`: `n` one-bits, one zero-bit,
then the `n` low bits of `x` MSB-first. Each position `i` uses its own
adaptive probability (`unary[i]`, `bits[i]`).

### 2.3 Token stream

Decoder state: history `hist` (last 5 raw bytes, all `0` initially, plus
`wpos` = bytes since the last whitespace byte (`0x20/0x09/0x0A/0x0D`),
clamped to `7`, init `0`; `bns` = last non-whitespace byte, init `0`),
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
            len  = gamma(len_unary/len_bits) + 2      in [3, 2^21+2]
            offset = rep[idx]                          (must be != 0, <= out_pos)
            rep update: if idx > 0 { tmp=rep[idx]; shift; rep0=tmp }
       0 -> normal match:
            offset code (below)          [regime 0]
            len  = gamma(len_unary/len_bits) + 2      in [3, 2^21+2]
            rep update: rep2=rep1; rep1=rep0; rep0=offset
            -- regime 2 (modes 2/3) reverses the order: len first
               (gamma(nlen_unary/nlen_bits)), then the offset code whose
               slot model is selected by class = min(len-2, 3)
       copy len bytes from out_pos - offset (overlap allowed)
       for each copied byte b in order: MIXTRAIN(hist, b); push(hist, b)
```

`push(hist, b)`: `hist.b[4..1] = hist.b[3..0]; hist.b[0] = b;` then
`wpos = ws(b) ? 0 : min(wpos + 1, 7)` where `ws(b)` is `b` in
`{0x20, 0x09, 0x0A, 0x0D}`; `if !ws(b): bns = b`.

**Offset code** (`u = off - 1`, `n = bitlen(u)`):

- `u < 4` (off 1..4): `off_mode = 0`, then `gamma(off)`
- else: `off_mode = 1`, then `gamma(n - 2)` (model `slot_unary/slot_bits` in
  regime 0; `slot_cls[cls]/slotb_cls[cls]`, `cls = min(len-2, 3)`, in
  regime 2), then the `n - 1` mantissa bits
  of `u` (top bit of `u` implicitly `1`): the low `min(n-1, 4)` bits are
  coded through the 4-bit **align tree** `off_align` (binary tree, node `idx`
  starts at `1`, `idx = 2*idx + bit`), the remaining high bits with
  `off_direct[i]` (bit `i`, `i` from `n-2` down to `ntree`)

`off` must decode to `[1, 2^20]`.

**Run tokens (v8).**  Match and rep lengths are Elias-gamma coded values
`len - 2` with up to 21 bit-planes (`SZIP_MAX_RUN = 2^21`), so a single
token can carry an arbitrarily long run of one repeated byte (e.g. one
distance-1 match for an entire zero page).  Such runs used to cost one
2 KiB token per 2048 bytes; they are now a single token.  Decoders must
accept lengths up to `2^21 + 2` and simply bound the copy by the remaining
output (`len <= raw_size - out_pos`); the encoder clamps every emitted
length at `2^21`.

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
- `BIT_PRICE[2049]` (u16): `BIT_PRICE[p] = clamp(round(-log2(p/2048) * 256),
  1, 5120)` (1/256-bit units; encoder-only).

**Model state**: six binary trees `t0..t5`, each `num_ctx × 256` u16 nodes
indexed `[ctx * 256 + sym]`, `sym` in 1..255 during the walk (leaf `sym = 0`
is never indexed). All nodes init `1024`. Mixer weights `w[64][6]` init
`(1<<13)/6`, clamp `±2^20`.

Contexts at literal time (`b0..b4 = hist.b[0..4]`, `wp = wpos`, `bn = bns`;
`mode` is the block mode byte):

```
c0 = b0 >> 3                                             (32 ctx)
c1 = (b0 << 8) | b1                                      (65536 exact)
c2 = (b0*2654435761 ^ b1*40503 ^ b2*805459861 ^ wp*2246822519) >> 14
c3 = (b0*2654435761 ^ b1*40503 ^ b2*805459861 ^ b3*1013904223 ^ wp*2246822519) >> 14
c4 = (b0*2654435761 ^ b1*40503 ^ b2*805459861 ^ b3*1013904223
      ^ b4*2246822519 ^ wp*3266489917) >> 14               (262144 ctx)
c5 = mode ? ((b0 << 8) | bn) : ((wp << 13) | (b0 << 5) | (b1 >> 3))   (65536)
```

Tree sizes: `t0` 32, `t1` 65536, `t2` 262144, `t3` 262144, `t4` 262144,
`t5` 65536 contexts.

**Per-mode rates**: tree nodes update `p += (2048-p)>>5` / `p -= p>>5` in
mode `0`, `>>4` in mode `1`. Mixer weight error shift: `>>18` in mode `0`,
`>>20` in mode `1`. Weight scale `>>13` both. (The encoder encodes each
block in both modes and stores the smaller; the mode byte selects which the
decoder applies.)

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
respecting the grammar decodes correctly. The shipped encoder uses
**dynamic-programming optimal parse** per block and mode:

- Match finder: 3-byte hash (`v * 2654435761 >> 14`), 18-bit table, hash
  chains, window `2^20`, chain depth `400`, max match `2048`; up to 8
  distinct-distance candidates per position (top by score, plus longest and
  nearest).
- Per position, three edge kinds: a literal; any rep0-2 family (length
  3..rep_maxlen); normal matches (full length sets 3..max_len at each
  candidate distance).
- Edge prices are exact (the models' live coded costs, `BIT_PRICE` at
  1/256-bit units; literal edges use the mixer's shadow price, trained bit by
  bit while pricing and restored afterwards).
- Planning passes run per mode (default 6, beam 8 for pass 1 and 32 for the
  guided passes; up to 24 match candidates per position); each pass after
  the first calibrates the price model along the previous plan's exact token
  history. Every candidate plan is encoded and the ACTUAL smaller payload
  ships. Both modes (mode byte `0`/`1`) are encoded per block and the smaller
  payload ships there too. The decoder cannot observe any of this.

### 2.5 SZPRINT v2 sprint blocks (`type = 2`)

The speed tier (`--sprint`) writes `type = 2` blocks: an element stream of
literals and copies (snappy-class tags):

- `tok & 3 == 0` LITERAL: `len-1 = tok >> 2` (1..60 direct; code 60 reads an
  extension value E and `len = 61 + E`), then `len` raw bytes.  The stream may
  end after a literal element.
- `tok & 3 == 1` COPY short: `mlen = ((tok >> 2) & 7) + 4` (4..11),
  `offset = ((tok >> 5) << 8) | next u8` (1..2047).
- `tok & 3 == 2` COPY wide: `mlen = (tok >> 2) + 4` (4..66; code 63 reads an
  extension E and `mlen = 67 + E`), `offset = u16 LE` (1..65535).
- `tok & 3 == 3` reserved (corrupt).

Extension value E: one byte if `E < 255`, else `255` sentinel + u16 LE `E-255`.
Copies use memmove semantics (`dst[i] = dst[i-offset]`).  The stream must not
end mid-element and must decompress to exactly `raw_size`.

### 2.6 Stored blocks (`type = 0`)

Raw bytes verbatim. The encoder picks stored whenever the compressed payload
would not be smaller than `raw_size` (incompressible data never expands).

### 2.7 BWT block codec (v6, `type = 1`, mode bit 2 set)

Payload after the mode byte:

| field | size | meaning |
|-------|------|---------|
| `primary` | 4 | BWT row of the unrotated input (LE u32) |
| `coder` | 1 | 0..3, selects the entropy coder below |
| stream | rest | the range-coder stream |

Transform: build the suffix array of the block's **rotations** (cyclic
doubling, ties left in radix-sort order), `bw[i] = raw[(sa[i] - 1) mod n]`.
Entropy coders over `bw` (each an ordinary `RcEncoder` stream):

| coder | model |
|-------|-------|
| 0 | raw `bw` columns through the §2.4 mixer (word-shape mode) |
| 1 | MTF ranks of `bw` through the mixer (word-shape mode) |
| 2 | raw `bw` columns, mixer gap mode |
| 3 | MTF ranks, mixer gap mode |
| 4 | MTF ranks + zero-run Elias-gamma (RUNA/RUNB-style runs of rank 0) |

Decoders invert MTF (if used) and then `bw` via the standard last/first
walk from `primary`, finally applying the byte-delta inverse if the mode
byte requested it.  Blocks of raw size 1 use a degenerate 2-byte payload
(`coder = 0`, one literal byte) and skip `primary`/MTF.

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
syntax is unchanged from v2. `version = 4` (2026-10-24) changes the mixer
(exact order-1, gap/word dual mode with per-mode rates, whitespace `wpos`),
the offset code (align tree), and prefixes each coded payload with its mode
byte. v1-v3 streams are rejected by the version field.

`version = 5` (2026-10-28) extends the mode byte to `(regime << 1) | mixer`
(modes 0-3). Mode 2/3 ("tight" regime) splits the length model by token
class (rep vs normal) and selects the offset-slot model by length class
`min(len - 2, 3)`, emitting `len` before the offset code. v5 encoders code
all four modes per block and store the smallest. v4 streams (version 4
header, mode 0/1) decode byte-exactly with a v5 decoder; v5 mode-2/3
streams are rejected by v4 decoders.

`version = 6` (2026-11-07) makes the mode byte extensible: bit 2 selects the
BWT block codec (§2.7, min-of-4 entropy coders) and bits 3-5 select the
byte-delta transform.  v6 encoders code all four LZ modes, the BWT codec on
the raw block and on each of the four delta transforms, and store the
smallest (self-tuning min-of-all).  The bare stored form (§1.2) adds a
4-byte frame for incompressible single-block files.  v4 and v5 streams
(header version 4/5, delta id 0) decode byte-exactly with a v6 decoder;
v6 streams are rejected by older decoders on the version field.

`version = 7` (2026-11-17) changes the **container only**; every payload
format above is bit-identical.  The compact v7 container (§1.1) replaces
the 20-byte header / 10-byte block headers / end-magic layout with a 4-byte
magic, 7-byte block headers and a 4-byte trailing CRC — 23 bytes less
overhead per archive.  The bare rule tightens to "compression saves at
least 12 bytes" so the worst-case expansion stays +4 bytes.  The SZLZRC
payload itself (called "SZLZRC v7") extends match/rep lengths to
`len - 2` gamma codes with 21 bit-planes (§2.3, run tokens), and the
regime-2 length tables grow to 22 positions; indices 0..9 evolve exactly
as in v4-v6, so all legacy payload bytes are reproduced identically.
Decoders route by the 4th magic byte (§1) and accept containers v4-v7;
v4-v6 archives decode byte-exactly.  v7 streams are rejected by older
decoders on the version/flag bytes.
