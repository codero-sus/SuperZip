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
other library. Both engines speak `.szip`
([docs/SZIP_FORMAT.md](docs/SZIP_FORMAT.md)) with deterministic output, and
the Rust `szip-sys` crate merges the C++ engine into Rust through a C ABI.
Format status: the C++ engine implements **v6** (min-of-4 LZ parse, BWT
blocks, byte-delta transforms, bare stored frame; v4/v5 streams decode
byte-exactly). The pure-Rust engine currently implements the v3-era format
and needs a format refresh to consume v6 streams — the C++ engine is the
reference implementation.

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
archives **and produce byte-identical output** (v3-era streams; the v6
refresh of the Rust engine and this check is pending).

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
machine. `--fast`/the GUI toggle trims the parse search further on
pathological input; the default encoder already escalates in measured steps.

## Codec: SZLZRC v6 (LZ parse + BWT escape)

**S**uper**Z**ip **LZ**77 + **R**ange **C**oder + integer context mixing —
block-based (default 1 MiB), price-aware hash-chain matching (up to 2 KiB
matches, 1 MiB window), last-3 rep-offsets, Elias-gamma length/offset coding
with tiny-offset and slot modes. Literals go through a **PAQ-style integer
context mixer**: six binary probability trees (orders 0–5, word-shape model)
combined per bit by fixed-point weights, refined with generated
stretch/squash/price tables — all our own code, deterministic, no floats.
v6 adds a **BWT block codec** (own cyclic-doubling suffix array, min-of-4
entropy coders: raw columns and MTF ranks, word and gap mixer modes) and
**byte-delta transforms** (d = 1/2/4/8) — every candidate is coded per block
and the smallest ships (self-tuning min-of-all). Incompressible single-block
files use the 4-byte **bare stored** frame, so `.szip` never expands data and
beats every competitor's stored-stream framing (500,004 B on 500,000 B of
random vs 500,005 for brotli).

Measured on this project's benchmark corpora (ratio = compressed/raw, lower
is better; `tests/mkcorpus.sh` / `tests/corpus20.py` regenerate them).
zstd/brotli/lz4/snappy columns are frozen reference numbers from the v5 run
of the same deterministic corpora; gzip/bzip2/xz re-measure each run
(`python3 tests/benchall.py`, raw log: `docs/bench20_v7.txt`):

| corpus (raw size) | **G0D SZIP v7** | gzip-9 | bzip2-9 | xz-9e | zstd-22 | brotli-11 | lz4 | snappy |
|---|---|---|---|---|---|---|---|---|
| code (70,592) | **17,209** | 19,282 | 18,214 | 17,640 | 18,012 | 16,894 | 23,869 | 29,674 |
| text (500,003) | **53,023** | 86,539 | 53,954 | 76,764 | 76,343 | 78,420 | 106,834 | 150,185 |
| rand (500,000) | **500,004** | 500,107 | 502,659 | 500,084 | 500,021 | 500,005 | 500,051 | 500,074 |
| src (277,530) | **52,155** | 76,747 | 65,288 | 51,796 | 56,066 | 50,871 | 98,238 | 119,236 |
| b64 (131,072) | **24,430** | 36,886 | 25,452 | 32,988 | 33,120 | 33,007 | 41,663 | 62,974 |
| binary (131,072) | **64,289** | 69,440 | 67,809 | 65,172 | 67,291 | 63,470 | 79,812 | 93,102 |
| csv (131,072) | **32,624** | 47,704 | 34,309 | 41,676 | 45,164 | 41,001 | 58,390 | 79,770 |
| dict (131,072) | **34,461** | 53,651 | 41,951 | 36,660 | 33,750 | 28,837 | 67,227 | 90,899 |
| dna (131,072) | **19,028** | 23,602 | 20,218 | 21,732 | 21,436 | 21,048 | 29,620 | 51,276 |
| floats (131,072) | **92,818** | 99,781 | 100,734 | 84,992 | 97,335 | 92,123 | 107,673 | 131,098 |
| hex (131,072) | **34,266** | 47,547 | 37,114 | 41,564 | 44,763 | 41,051 | 56,535 | 74,350 |
| html (131,072) | **12,038** | 16,919 | 12,036 | 15,252 | 15,680 | 14,857 | 19,534 | 30,747 |
| json (131,072) | **10,775** | 16,313 | 11,133 | 13,156 | 13,614 | 12,867 | 22,017 | 32,431 |
| log (131,072) | **8,940** | 13,495 | 8,948 | 11,172 | 11,589 | 11,166 | 17,404 | 26,010 |
| mixed (131,072) | **52,090** | 61,722 | 56,973 | 53,240 | 56,979 | 53,451 | 72,357 | 85,583 |
| pcm (131,072) | **101,896** | 127,322 | 122,971 | 124,512 | 127,575 | 114,926 | 131,099 | 131,098 |
| prose (131,072) | **26,431** | 36,679 | 28,427 | 33,292 | 33,285 | 32,793 | 42,781 | 61,607 |
| sparse (131,072) | **880** | 1,034 | 1,020 | 912 | 820 | 786 | 1,331 | 6,921 |
| utf8 (131,072) | **14,786** | 23,091 | 14,981 | 21,212 | 20,989 | 20,973 | 28,341 | 41,912 |
| xml (131,072) | **13,937** | 20,903 | 13,835 | 17,768 | 18,924 | 17,142 | 25,667 | 34,636 |

Win/loss against the best alternative on each corpus (the written check is
`sh tests/benchall.sh` / `python3 tests/benchall.py`, every roundtrip
byte-exact):

**12 wins** — text +1.73%, pcm +11.34%, hex +7.67%, prose +7.02%, dna
+5.89%, csv +4.91%, b64 +4.02%, json +3.22%, mixed +2.16%, utf8 +1.30%,
log +0.09% (8 bytes), rand +1 byte (bare framing beats brotli's).
**8 losses** — floats −9.21% (xz), sparse −11.96% and dict −19.50%
(brotli's run/static-model niches), xml −0.74%, code −1.86%, src −2.52%,
binary −1.29% (brotli), html −0.02% — 2 bytes behind bzip2 (12,038 vs
12,036).

| won | lost (to) |
|---|---|
| text **+1.73%**, pcm **+11.34%**, hex **+7.67%**, prose **+7.02%** | floats −9.21% (xz), sparse −11.96%, dict −19.50% (brotli run/static models) |
| dna **+5.89%**, csv **+4.91%**, b64 **+4.02%**, json **+3.22%** | xml −0.74% (bzip2), code −1.86%, src −2.52%, binary −1.29% (brotli) |
| mixed **+2.16%**, utf8 **+1.30%**, log **+0.09%**, rand **+1 byte** | html **−0.02%** — 2 bytes behind bzip2 (12,038 vs 12,036) |

How v7 flips the board (v5 scoreboard was 4 wins / 15 losses / 1 DNF): a
Burrows-Wheeler escape hatch (own cyclic-doubling rotation sort, min-of-4
entropy coders, MTF/gap literal modes) and byte-delta transforms
(d = 1/2/4/8) compete per block against the LZ parse and the smallest ships
— the html/log/json/dna/csv/b64/hex/pcm class that bzip2 used to own is now
G0D's. Incompressible input stays bare (4-byte container + raw), so random
data beats even brotli's stored framing by one byte and `.szip` never
expands data.

**Speed (the v7 rewrite)**: one refined parse emits all four modes (the
modes differ only in pricing), trial lengths are bounded and long matches
are taken whole, prices are lazily cached per position, the BWT family
runs 5-way parallel with a gap-entropy prefilter, and the finder chain is
capped. Result: **2.6-5.3 s to encode a 128 KiB corpus, 12.2 s for the
500 KB text corpus, 10.4 s worst-case (sparse — 39x faster than the v6
build's 402 s)**; `--fast`/GUI toggle halves that again (6.9 s on 500 KB
text, same size there). Decode is single-pass: 0.2-0.3 s. gzip/xz remain
faster encoders at far worse ratios; G0D SZIP trades encode search for
wins on 12 of 20 types.

What remains: **floats** (xz's filters own structured high-entropy binary;
the ARF float coder is not yet in the v6 block format), brotli's
run/static-model niches (**sparse, dict**), **html is 2 bytes** behind
bzip2, and the four sub-3% rows (xml, code, src, binary).
`tests/beatsniper.sh` is the strict classic gate: 10 passed / 1 failed
(the 4%-under-xz stretch on code: 17,209 vs 16,933).

## Safety

Decompressors parse attacker-controlled bytes. Both engines validate every
length/offset, never trust the framing, and the C++ engine is continuously
fuzz-shaped by ASan/UBSan roundtrips plus a corruption battery (bit-flips,
truncations, bad magic must be rejected).
<!-- PERSIST-MARKER-9 -->
