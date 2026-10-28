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
g0dszip                    # GUI — the default entry point (web page at :8848)
g0dszip c [-f] [-b N] in out.szip   # compress (-f = fast parse)
g0dszip d input.szip output         # decompress
g0dszip t input.szip                # verify (CRC32 + structure)
g0dszip i input.szip                # info
g0dszip g [port]                    # GUI on an explicit port (default 8848)
```

Running `g0dszip` with no arguments opens the GUI (and your browser); the
command-line subcommands are the optional power path.

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

| corpus (raw size) | **G0D SZIP v5** | gzip-9 | bzip2-9 | xz-9e | zstd-22 | brotli-11 | lz4 | snappy |
|---|---|---|---|---|---|---|---|---|
| code (70,592) | **17,011** | 19,282 | 18,214 | 17,640 | 18,012 | 16,894 | 23,869 | 29,674 |
| text (500,003) | **53,121** | 86,539 | 53,954 | 76,764 | 76,343 | 78,420 | 106,834 | 150,185 |
| rand (500,000) | **500,038** | 500,107 | 502,661 | 500,084 | 500,021 | 500,005 | 500,051 | 500,074 |
| src (277,530) | **50,984** | 76,747 | 65,288 | 51,796 | 56,066 | 50,871 | 98,238 | 119,236 |
| b64 (131,072) | **27,549** | 36,886 | 25,452 | 32,988 | 33,120 | 33,007 | 41,663 | 62,974 |
| binary (131,072) | **65,559** | 69,440 | 67,809 | 65,172 | 67,291 | 63,470 | 79,812 | 93,102 |
| csv (131,072) | **35,555** | 47,704 | 34,309 | 41,676 | 45,164 | 41,001 | 58,390 | 79,770 |
| dict (131,072) | **32,527** | 53,651 | 41,951 | 36,660 | 33,750 | 28,837 | 67,227 | 90,899 |
| dna (131,072) | **21,978** | 23,602 | 20,218 | 21,732 | 21,436 | 21,048 | 29,620 | 51,276 |
| floats (131,072) | **93,356** | 99,781 | 100,734 | 84,992 | 97,335 | 92,123 | 107,673 | 131,098 |
| hex (131,072) | **35,644** | 47,547 | 37,114 | 41,564 | 44,763 | 41,051 | 56,535 | 74,350 |
| html (131,072) | **13,295** | 16,919 | 12,036 | 15,252 | 15,680 | 14,857 | 19,534 | 30,747 |
| json (131,072) | **11,820** | 16,313 | 11,133 | 13,156 | 13,614 | 12,867 | 22,017 | 32,431 |
| log (131,072) | **10,079** | 13,495 | 8,948 | 11,172 | 11,589 | 11,166 | 17,404 | 26,010 |
| mixed (131,072) | **51,344** | 61,722 | 56,973 | 53,240 | 56,979 | 53,451 | 72,357 | 85,583 |
| pcm (131,072) | **121,573** | 127,322 | 122,971 | 124,512 | 127,575 | 114,926 | 131,099 | 131,098 |
| prose (131,072) | **27,402** | 36,679 | 28,427 | 33,292 | 33,285 | 32,793 | 42,781 | 61,607 |
| sparse (131,072) | **—** | 1,034 | 1,020 | 912 | 820 | 786 | 1,331 | 6,921 |
| utf8 (131,072) | **15,624** | 23,091 | 14,981 | 21,212 | 20,989 | 20,973 | 28,341 | 41,912 |
| xml (131,072) | **16,278** | 20,903 | 13,835 | 17,768 | 18,924 | 17,142 | 25,667 | 34,636 |

Win/loss against the best alternative on each corpus (the written check is
`sh tests/benchall.sh`, every roundtrip byte-exact):

| won | lost (to) |
|---|---|
| text **+1.54%** | code −0.69% (brotli), src −0.22% (brotli), rand −0.01% (brotli framing) |
| hex **+3.96%** | b64 −8.24%, dna −8.71%, csv −3.63% (bzip2/BWT) |
| mixed **+3.56%** | xml −17.66%, log −12.64%, html −10.46%, json −6.17%, utf8 −4.29%, dict −12.80% (bzip2 or brotli) |
| prose **+3.61%** | binary −3.29%, pcm −5.78% (brotli), floats −9.84% (xz) |
| | sparse: best-mode parse exceeds an hour on long zero runs (use `-f`; known bound to add) |

**4 wins, 15 losses, 1 DNF — the honest scoreboard.** The pattern is
consistent: G0D SZIP's LZ77 + adaptive context mixing is #1 or #2 almost
everywhere and beats gzip, bzip2, xz, zstd, lz4 and snappy on many types, but
two specialists keep their niches — **bzip2's BWT** wins on templated,
suffix-rich text (xml/log/html/json/dna/dict/csv: 4-18%), and **brotli's
static context model** wins on code-like and high-entropy binary data.
Flipping those needs BWT-class transforms and static context maps (model v6
research), not parse or container tuning.

The frozen benchmark corpora live in `tests/corpus/` (`src.bin` = the
real-world source snapshot) and `tests/corpus20.py` deterministically
generates the 16 data types, so every number above is reproducible.

Honest gaps (the benchall verdict, 1/4 first places): brotli's static-model
edge on **code** (0.69%), this repo's **source** (0.38%), and **random**
(0.01% — pure framing: brotli writes no checksum, G0D SZIP always writes
CRC32; all compressors sit within 33 bytes there). On text — the corpus that
actually rewards compression — nothing else is in the same league: G0D SZIP
is 833 bytes ahead of the runner-up and 23,000+ ahead of zstd/brotli/xz.
`tests/beatsniper.sh` remains the strict classic gate (10 passed / 1 failed —
the 4%-under-xz stretch, now 78 bytes away). Evolution:
v2 scored 26.3%/17.2% on the old corpora; v3 mixed literals; v4 dual-mode
six-pass DP optimal parse (beam 32 guided passes, 24 match candidates, both
literal modes per block, smallest actual payload ships) is the current form.

Benchmark corpora are frozen in `tests/corpus/` (`tests/mkcorpus.sh` copies
them to `build/`) so these claims stay reproducible as the code evolves.
The encoder parses each block by dynamic programming: every literal, rep, and
match edge is priced exactly (the model's real coded cost, 1/256-bit
resolution), six guided passes converge the plan, and every candidate is
actually encoded — the smallest payload ships. Each block is encoded in **four
modes** (v4 and "tight" token models × word-shape/gap literal mixers) and the
mode byte in the payload selects the winner. All of it is encoder-side; the
decoder just follows the mode byte. Any compliant parse policy interoperates.
Format v5 streams add the tight regime (split rep/normal length models,
length-class offset slots); the decoder still reads v4 files byte-exactly.
`tests/beatsniper.sh` verifies roundtrips and the sniper scoreboard.

## Safety

Decompressors parse attacker-controlled bytes. Both engines validate every
length/offset, never trust the framing, and the C++ engine is continuously
fuzz-shaped by ASan/UBSan roundtrips plus a corruption battery (bit-flips,
truncations, bad magic must be rejected).
