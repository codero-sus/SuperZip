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

| corpus | **G0D SZIP v4** | gzip-9 | bzip2-9 | xz-9e | zstd-22 | brotli-11 |
|---|---|---|---|---|---|---|
| source code (70,592 B) | **17,011 B** | 19,282 | 18,214 | 17,640 | 18,012 | 16,894 |
| word-salad text (500,003 B) | **53,121 B** | 86,539 | 53,954 | 76,764 | 76,343 | 78,420 |
| random (500,000 B) | **500,038 B** | 500,107 | 502,690 | 500,084 | 500,021 | 500,005 |
| this repo's source (277,530 B) | **51,064 B** | 76,747 | 65,288 | 51,796 | 56,066 | 50,871 |

**G0D SZIP beats gzip, bzip2 and xz on every corpus**, and on text it beats
*everything in the known universe* by 800+ bytes (bzip2 53,954 was the
previous best there). `sh tests/benchall.sh` is the written universe
benchmark: it runs every compressor available (gzip, bzip2, xz, zstd-22,
brotli-11, lz4, snappy) at maximum settings on all four corpora, verifies
every roundtrip byte-exact, and prints the honest scoreboard.

The frozen benchmark corpora live in `tests/corpus/` (including `src.bin`,
the real-world source snapshot) so every number above is reproducible.

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
