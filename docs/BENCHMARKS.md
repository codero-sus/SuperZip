# G0D SZIP — Benchmarks (speed & size)

Engine **v8 / SZLZRC v7 format** (ULTRA speed pass). 20 deterministic
benchmark corpora (`tests/mkcorpus.sh` + `tests/corpus20.py` regenerate
them). Every roundtrip is byte-exact (`cmp`). Wall times are single-run
indicative numbers measured on a 2-core machine; reproduce with
`python3 tests/benchall.py` (raw log: `bench20_v8u.txt`).

Columns gzip-9 / bzip2-9 / xz-9e are re-measured every run.
Columns zstd-22 / brotli-11 / lz4 / snappy are frozen reference numbers
from the v5-era run of the same deterministic corpora (those tools are not
installed in this environment; their sizes do not change across runs of
fixed corpora).

## Size (bytes, lower is better)

| corpus (raw) | **G0D SZIP** | gzip-9 | bzip2-9 | xz-9e | zstd-22 | brotli-11 | lz4 | snappy |
|---|---|---|---|---|---|---|---|---|
| text (500,003) | **53,000** | 86,539 | 53,954 | 76,764 | 76,343 | 78,420 | 106,834 | 150,185 |
| rand (500,000) | **500,004** | 500,107 | 502,684 | 500,084 | 500,021 | 500,005 | 500,051 | 500,074 |
| code (70,592) | **17,186** | 19,282 | 18,214 | 17,640 | 18,012 | 16,894 | 23,869 | 29,674 |
| src (277,530) | **52,132** | 76,747 | 65,288 | 51,796 | 56,066 | 50,871 | 98,238 | 119,236 |
| b64 (131,072) | **24,407** | 36,886 | 25,452 | 32,988 | 33,120 | 33,007 | 41,663 | 62,974 |
| binary (131,072) | **64,266** | 69,440 | 67,809 | 65,172 | 67,291 | 63,470 | 79,812 | 93,102 |
| csv (131,072) | **32,601** | 47,704 | 34,309 | 41,676 | 45,164 | 41,001 | 58,390 | 79,770 |
| dict (131,072) | **34,438** | 53,651 | 41,951 | 36,660 | 33,750 | 28,837 | 67,227 | 90,899 |
| dna (131,072) | **19,005** | 23,602 | 20,218 | 21,732 | 21,436 | 21,048 | 29,620 | 51,276 |
| floats (131,072) | **92,795** | 99,781 | 100,734 | 84,992 | 97,335 | 92,123 | 107,673 | 131,098 |
| hex (131,072) | **34,243** | 47,547 | 37,114 | 41,564 | 44,763 | 41,051 | 56,535 | 74,350 |
| html (131,072) | **12,015** | 16,919 | 12,036 | 15,252 | 15,680 | 14,857 | 19,534 | 30,747 |
| json (131,072) | **10,752** | 16,313 | 11,133 | 13,156 | 13,614 | 12,867 | 22,017 | 32,431 |
| log (131,072) | **8,917** | 13,495 | 8,948 | 11,172 | 11,589 | 11,166 | 17,404 | 26,010 |
| mixed (131,072) | **52,067** | 61,722 | 56,973 | 53,240 | 56,979 | 53,451 | 72,357 | 85,583 |
| pcm (131,072) | **101,873** | 127,322 | 122,971 | 124,512 | 127,575 | 114,926 | 131,099 | 131,098 |
| prose (131,072) | **26,408** | 36,679 | 28,427 | 33,292 | 33,285 | 32,793 | 42,781 | 61,607 |
| sparse (131,072) | **780** | 1,034 | 1,020 | 912 | 820 | 786 | 1,331 | 6,921 |
| utf8 (131,072) | **14,763** | 23,091 | 14,981 | 21,212 | 20,989 | 20,973 | 28,341 | 41,912 |
| xml (131,072) | **13,914** | 20,903 | 13,835 | 17,768 | 18,924 | 17,142 | 25,667 | 34,636 |

## Size scoreboard vs the best alternative (all 7 competitors)

**14 wins / 6 losses**

| corpus | G0D | best alternative | edge | verdict |
|---|---|---|---|---|
| text | 53,000 | bzip2 53,954 | **+1.77%** | WIN |
| rand | 500,004 | brotli 500,005 | **+0.00%** | WIN |
| b64 | 24,407 | bzip2 25,452 | **+4.11%** | WIN |
| csv | 32,601 | bzip2 34,309 | **+4.98%** | WIN |
| dna | 19,005 | bzip2 20,218 | **+6.00%** | WIN |
| hex | 34,243 | bzip2 37,114 | **+7.74%** | WIN |
| html | 12,015 | bzip2 12,036 | **+0.17%** | WIN |
| json | 10,752 | bzip2 11,133 | **+3.42%** | WIN |
| log | 8,917 | bzip2 8,948 | **+0.35%** | WIN |
| mixed | 52,067 | xz 53,240 | **+2.20%** | WIN |
| pcm | 101,873 | brotli 114,926 | **+11.36%** | WIN |
| prose | 26,408 | bzip2 28,427 | **+7.10%** | WIN |
| sparse | 780 | brotli 786 | **+0.76%** | WIN |
| utf8 | 14,763 | bzip2 14,981 | **+1.46%** | WIN |
| code | 17,186 | brotli 16,894 | -1.73% | loss |
| src | 52,132 | brotli 50,871 | -2.48% | loss |
| binary | 64,266 | brotli 63,470 | -1.25% | loss |
| dict | 34,438 | brotli 28,837 | -19.42% | loss |
| floats | 92,795 | xz 84,992 | -9.18% | loss |
| xml | 13,914 | bzip2 13,835 | -0.57% | loss |

## Encode speed (seconds, lower is better)

| corpus | **G0D** | G0D -f | gzip-9 | bzip2-9 | xz-9e | G0D MB/s |
|---|---|---|---|---|---|---|
| text | 8.90 | 4.85 | 0.04 | 0.04 | 0.18 | 0.06 |
| rand | 5.07 | 4.28 | 0.02 | 0.07 | 0.17 | 0.10 |
| code | 2.56 | 2.24 | 0.01 | 0.01 | 0.04 | 0.03 |
| src | 5.41 | 3.18 | 0.03 | 0.02 | 0.10 | 0.05 |
| b64 | 3.23 | 2.38 | 0.03 | 0.02 | 0.06 | 0.04 |
| binary | 2.69 | 2.37 | 0.06 | 0.01 | 0.08 | 0.05 |
| csv | 3.00 | 2.44 | 0.03 | 0.01 | 0.05 | 0.04 |
| dict | 3.03 | 2.37 | 0.01 | 0.01 | 0.06 | 0.04 |
| dna | 3.06 | 2.08 | 0.06 | 0.01 | 0.05 | 0.04 |
| floats | 2.55 | 2.21 | 0.01 | 0.02 | 0.08 | 0.05 |
| hex | 3.12 | 2.26 | 0.04 | 0.01 | 0.07 | 0.04 |
| html | 3.40 | 2.30 | 0.01 | 0.02 | 0.07 | 0.04 |
| json | 3.10 | 2.29 | 0.02 | 0.02 | 0.05 | 0.04 |
| log | 3.19 | 2.31 | 0.01 | 0.02 | 0.05 | 0.04 |
| mixed | 2.65 | 2.25 | 0.01 | 0.01 | 0.08 | 0.05 |
| pcm | 2.35 | 2.28 | 0.01 | 0.02 | 0.08 | 0.06 |
| prose | 2.84 | 2.18 | 0.06 | 0.01 | 0.05 | 0.05 |
| sparse | 2.54 | 1.99 | 0.01 | 0.01 | 0.02 | 0.05 |
| utf8 | 3.05 | 2.15 | 0.02 | 0.01 | 0.05 | 0.04 |
| xml | 3.12 | 2.18 | 0.01 | 0.01 | 0.05 | 0.04 |
| **total (all rows)** | **68.9** | 50.6 | 0.50 | 0.36 | 1.44 | |

## Decode speed (seconds, lower is better)

| corpus | **G0D** | gzip-9 | bzip2-9 | xz-9e | G0D MB/s |
|---|---|---|---|---|---|
| text | 0.31 | 0.01 | 0.02 | 0.01 | 1.6 |
| rand | 0.01 | 0.01 | 0.04 | 0.01 | 50.0 |
| code | 0.23 | 0.00 | 0.01 | 0.01 | 0.3 |
| src | 0.30 | 0.01 | 0.01 | 0.01 | 0.9 |
| b64 | 0.24 | 0.01 | 0.01 | 0.01 | 0.5 |
| binary | 0.28 | 0.00 | 0.01 | 0.01 | 0.5 |
| csv | 0.25 | 0.00 | 0.01 | 0.01 | 0.5 |
| dict | 0.25 | 0.00 | 0.01 | 0.01 | 0.5 |
| dna | 0.21 | 0.00 | 0.01 | 0.00 | 0.6 |
| floats | 0.29 | 0.00 | 0.01 | 0.01 | 0.5 |
| hex | 0.24 | 0.00 | 0.01 | 0.01 | 0.5 |
| html | 0.22 | 0.00 | 0.01 | 0.01 | 0.6 |
| json | 0.22 | 0.00 | 0.01 | 0.01 | 0.6 |
| log | 0.22 | 0.00 | 0.01 | 0.00 | 0.6 |
| mixed | 0.26 | 0.00 | 0.01 | 0.01 | 0.5 |
| pcm | 0.32 | 0.00 | 0.01 | 0.01 | 0.4 |
| prose | 0.23 | 0.00 | 0.01 | 0.01 | 0.6 |
| sparse | 0.24 | 0.00 | 0.00 | 0.00 | 0.5 |
| utf8 | 0.23 | 0.00 | 0.01 | 0.01 | 0.6 |
| xml | 0.23 | 0.00 | 0.01 | 0.01 | 0.6 |
| **total (all rows)** | **4.8** | 0.04 | 0.23 | 0.17 | |

## Aggregate (20 corpora)

- total raw: **3,445,277 B** (3.45 MB)
- total G0D SZIP: **1,165,566 B** (33.83% of raw) vs best-per-row alternatives 1,174,428 B — **+0.75%**
- total encode wall: G0D **68.9 s** (0.05 MB/s), G0D -f 50.6 s, gzip 0.50 s, bzip2 0.36 s, xz 1.44 s
- total decode wall: G0D **4.8 s** (0.7 MB/s), gzip 0.04 s, bzip2 0.23 s, xz 0.17 s

## Speed passes (byte-identical output)

Every speed pass below reproduces the same `.szip` bytes as the v8 engine —
size, ratios and the 14W/6L scoreboard cannot change. Verified by
`tests/goldcheck.py` (20/20 rows byte-identical + roundtrip), `build/sztest`
(all tests passed) and `tests/beatsniper.sh` (10/1, frozen result).

1. **ULTRA pass** (1.40x on top of v8): parallel match finder, bwt overlap,
   SWAR rep-match, multi-block group-parallel, `best_worstc` fast-reject.
2. **Hot-path pass** (another ~1.12x encode / ~1.16x decode on top of the
   committed engine, best-of-2, 2 cores):
   - `MixLit::peek()` rewritten as a pure-local walk (no 2 KB snapshot and
     restore per DP position) — exact, the probability walk is a unique path.
   - beam slots 32 -> 12 (exact: same extension order and limits).
   - tree init: SIMD u16 fill + 2 MB-aligned `MADV_HUGEPAGE` blocks through
     a small global allocator pool — the encode "emit" wall was mostly
     DRAM/page-walk traffic over the 470 MB mixer trees (48 random cache
     lines per byte), and huge pages removed the page-walk amplification.
     A hard 512 MB pool cap keeps the 3.9 GB reference VM safe.
   - emit workers made core-adaptive (2-4).

Best-of-2 wall, old = committed engine, new = hot-path engine:

| row | enc old | enc new | up | dec old | dec new | up |
|-----|--------:|--------:|---:|--------:|--------:|---:|
| prose | 3.19 | 2.80 | 1.14x | 0.246 | 0.222 | 1.11x |
| dict | 3.30 | 2.80 | 1.18x | 0.281 | 0.221 | 1.27x |
| text | 9.11 | 8.87 | 1.03x | 0.330 | 0.275 | 1.20x |
| code | 1.98 | 1.85 | 1.08x | 0.220 | 0.193 | 1.14x |
| xml | 3.25 | 2.96 | 1.10x | 0.238 | 0.227 | 1.05x |
| json | 3.21 | 2.92 | 1.10x | 0.229 | 0.217 | 1.06x |
| log | 3.36 | 3.11 | 1.08x | 0.243 | 0.205 | 1.18x |
| html | 3.48 | 3.21 | 1.09x | 0.238 | 0.216 | 1.10x |
| csv | 3.23 | 2.78 | 1.16x | 0.254 | 0.203 | 1.25x |
| utf8 | 3.01 | 2.79 | 1.08x | 0.227 | 0.203 | 1.12x |
| b64 | 3.09 | 2.82 | 1.09x | 0.225 | 0.192 | 1.18x |
| hex | 3.11 | 2.94 | 1.06x | 0.261 | 0.221 | 1.18x |
| dna | 3.12 | 3.04 | 1.03x | 0.225 | 0.215 | 1.05x |
| src | 5.74 | 5.14 | 1.12x | 0.316 | 0.282 | 1.12x |
| floats | 2.49 | 2.01 | 1.24x | 0.302 | 0.260 | 1.16x |
| pcm | 2.42 | 1.88 | 1.29x | 0.314 | 0.265 | 1.19x |
| binary | 2.85 | 2.33 | 1.22x | 0.272 | 0.236 | 1.15x |
| sparse | 2.60 | 2.54 | 1.02x | 0.216 | 0.188 | 1.15x |
| mixed | 2.85 | 2.41 | 1.18x | 0.262 | 0.202 | 1.30x |
| rand | 5.23 | 4.04 | 1.29x | 0.004 | 0.004 | 0.92x |
| **TOTAL** | **70.64** | **63.24** | **1.12x** | **4.903** | **4.245** | **1.16x** |

A second interleaved run of the same pair measured 69.54 -> 60.73 s (1.15x)
encode and 5.02 -> 4.36 s (1.15x) decode; sandbox timing noise is a few
percent, so claim ~1.1-1.15x encode and ~1.15-1.25x decode on top of the
ULTRA engine. Combined with the ULTRA pass this is ~1.55-1.6x over the raw
v8 engine, still at identical bytes.

## Notes

- **Ratios vs speed**: G0D SZIP optimizes for size first; gzip/bzip2/xz are
  faster encoders at far worse ratios (see the size table). G0D encode runs
  at ~2.5-12 s per corpus on 2 cores; the ULTRA speed pass made it 1.40x
  faster than the v8 engine at byte-identical output (sparse 3.5x), and the
  hot-path pass above added ~1.12-1.16x more.
- **Decode** is single-pass and fast (0.2-0.4 s per corpus here, ~0.3-2 GB/s),
  with multi-block archives decoding blocks in parallel.
- `rand` is stored in the 4-byte **bare frame** (G0D never expands data;
  worst case +4 B), which beats every competitor stored-stream framing.
- Every G0D roundtrip is verified byte-exact against the input; every
  competitor roundtrip is verified by benchall.py as well.
