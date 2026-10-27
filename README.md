# G0D SZIP (SuperZip)

Compress files nicely. Format `.szip`.

**G0D SZIP** is a from-scratch compression project with **two complete
implementations of one format**, merged into one toolchain:

| layer | C++ engine (`native/`) | Rust engine (`rust/szip-core/`) |
|---|---|---|
| LZ77 match finder | ✅ own | ✅ own |
| adaptive range coder | ✅ own | ✅ own |
| CRC-32 | ✅ own | ✅ own |
| `.szip` container | ✅ own | ✅ own |
| CLI | `build/g0dszip` | `g0dszip`, `g0dszip-cpp` |

No third-party compression code anywhere — no zlib, zstd, lz4, brotli or any
other library. The two engines are byte-compatible twins: same format
([docs/SZIP_FORMAT.md](docs/SZIP_FORMAT.md)), deterministic output, and the
Rust `szip-sys` crate merges the C++ engine into Rust through a C ABI.

## Layout

```
native/            C++17 engine: range coder, LZ77, CRC32, codec, CLI
rust/szip-core/    pure-Rust engine (std only, zero dependencies)
rust/szip-sys/     FFI bridge: builds native/ with g++/ar, exposes C ABI
rust/g0dszip/      CLI — Rust engine
rust/g0dszip-cpp/  CLI — Rust container + C++ codec (the merge)
docs/              format specification
tests/             cross-engine compatibility checks
```

## Build & test

```sh
make            # builds build/g0dszip (C++ engine)
make test       # C++ engine tests (ASan/UBSan) + cross-engine checks

cd rust
cargo build --release   # needs a Rust toolchain + g++
cargo test              # Rust engine unit tests + C++ engine via FFI
```

`tests/crosscheck.sh` proves all three engines round-trip each other's
archives **and produce byte-identical output**.

Both parseers run in parallel and share match-finder / literal-price caches
(encode is ~3× faster on code, ~5× on text than the first v4 build at the same
compressed sizes). Pre-release binaries for Linux, macOS and Windows are built
by `.github/workflows/release.yml` when a `v4.0.0-pre*` tag is pushed.

## Usage

```sh
g0dszip c [-f] [-b blocksize] input output.szip   # compress (-f = fast parse)
g0dszip d input.szip output                       # decompress
g0dszip t input.szip                              # verify (CRC32 + structure)
g0dszip i input.szip                              # info
g0dszip g [port]                                  # lightweight web GUI (default :8848)
```

The GUI is a tiny built-in HTTP server with one embedded page — drag & drop a
file, pick Compress/Decompress (with a Fast-parse toggle), download the result.
Everything runs in-process with the same encoder/decoder; nothing leaves the
machine. `--fast`/the GUI toggle switches to a quicker parse (on text it is
~2.5x faster at the same size; on code it costs a few dozen bytes).

## Codec: SZLZRC v4 ("GOD stage")

**S**uper**Z**ip **LZ**77 + **R**ange **C**oder + integer context mixing —
block-based (default 1 MiB), price-aware hash-chain matching (up to 2 KiB
matches, 1 MiB window), last-3 rep-offsets, Elias-gamma length/offset coding
with tiny-offset and slot modes. Literals go through a **PAQ-style integer
context mixer**: six binary probability trees (orders 0–5, word-shape model)
combined per bit by fixed-point weights, refined with generated
stretch/squash/price tables — all our own code, deterministic, no floats.
Incompressible blocks are stored verbatim, so `.szip` never expands data.

Measured on this project's benchmark corpora (ratio = compressed/raw, lower
is better; `tests/mkcorpus.sh` regenerates them; random data is stored so it
never expands):

| corpus | **G0D SZIP v3** | gzip-9 | bzip2-9 | xz-9e |
|---|---|---|---|---|
| source code (70,592 B) | **24.122%** (17,018 B) | 27.325% | 25.802% | 24.989% |
| word-salad text (500,003 B) | **10.624%** (53,122 B) | 17.309% | 10.791% | 15.353% |
| random (500,000 B) | **100.008%** (500,038 B) | 100.023% | 100.526% | 100.017% |

**G0D SZIP beats every sniper on all three corpora** — source code now
included: 17,018 B vs xz-9e's 17,640 B (3.5% better) and bzip2-9's 18,214 B.
On text it stays 770+ bytes ahead of bzip2. The strict goal of 4% under xz on
code (16,933 B) is 85 bytes away; `tests/beatsniper.sh` runs the whole
scoreboard with roundtrip checks and reports it. Evolution: v2 scored
26.3%/17.2% on the old corpora — the v3 mixer cut text by ~38% relative; the
v4 dual-mode optimal parse (four exact-calibrated DP passes, both model modes
per block, smallest actual payload wins) took code from 25.8% to 24.1%.

Benchmark corpora are frozen in `tests/corpus/` (`tests/mkcorpus.sh` copies
them to `build/`) so these claims stay reproducible as the code evolves.
The encoder parses each block by dynamic programming: every literal, rep, and
match edge is priced exactly (the model's real coded cost, 1/256-bit
resolution), four guided passes converge the plan, and every candidate is
actually encoded — the smallest payload ships. Each block is encoded in both
literal-model modes (word-shape for text, whitespace-gap for code) and the
mode byte in the payload selects the winner. All of it is encoder-side; the
decoder just follows the mode byte. Any compliant parse policy interoperates.
`tests/beatsniper.sh` verifies roundtrips and the sniper scoreboard.

## Safety

Decompressors parse attacker-controlled bytes. Both engines validate every
length/offset, never trust the framing, and the C++ engine is continuously
fuzz-shaped by ASan/UBSan roundtrips plus a corruption battery (bit-flips,
truncations, bad magic must be rejected).
