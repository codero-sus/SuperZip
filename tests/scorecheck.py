#!/usr/bin/env python3
"""G0D SZIP scoreboard gate ("make it the best no loses").

Encodes every corpus row with the DEFAULT engine and asserts:
  1. every row roundtrips byte-exact,
  2. every row that won on the v8u scoreboard (14 rows) STILL WINS against
     the frozen best-alternative size (brotli/bzip2/xz from docs/BENCHMARKS.md),
  3. the loss rows stay losses-or-better (no new losses allowed).

Usage: python3 tests/scorecheck.py [binary]        (default build/g0dszip)
Exit 0 only if every check passes.
"""
import subprocess, sys, os

# (row, path, best-alternative size in bytes) — frozen from docs/BENCHMARKS.md
ROWS = [("prose","build/corpus20/prose.bin",28427),   # bzip2
        ("dict","build/corpus20/dict.bin",28837),     # brotli (loss)
        ("text","build/corpus_text.bin",53954),       # bzip2
        ("code","build/corpus_code.bin",16894),       # brotli (loss)
        ("xml","build/corpus20/xml.bin",13835),       # bzip2 (loss)
        ("json","build/corpus20/json.bin",11133),     # bzip2
        ("log","build/corpus20/log.bin",8948),        # bzip2
        ("html","build/corpus20/html.bin",12036),     # bzip2
        ("csv","build/corpus20/csv.bin",34309),       # bzip2
        ("utf8","build/corpus20/utf8.bin",14981),     # bzip2
        ("b64","build/corpus20/b64.bin",25452),       # bzip2
        ("hex","build/corpus20/hex.bin",37114),       # bzip2
        ("dna","build/corpus20/dna.bin",20218),       # bzip2
        ("src","tests/corpus/src.bin",50871),         # brotli (loss)
        ("floats","build/corpus20/floats.bin",84992), # xz (loss)
        ("pcm","build/corpus20/pcm.bin",114926),      # brotli
        ("binary","build/corpus20/binary.bin",63470), # brotli (loss)
        ("sparse","build/corpus20/sparse.bin",786),   # brotli
        ("mixed","build/corpus20/mixed.bin",53240),   # xz
        ("rand","build/corpus_rand.bin",500005)]      # brotli
WIN_ROWS = {"prose","text","json","log","html","csv","utf8","b64","hex",
            "dna","pcm","sparse","mixed","rand"}  # the 14 golden wins

def main():
    binpath = sys.argv[1] if len(sys.argv) > 1 else "build/g0dszip"
    fail = 0
    wins = losses = 0
    for name, path, alt in ROWS:
        subprocess.run([binpath, "c", path, "/tmp/sc.szip"], capture_output=True)
        sz = os.path.getsize("/tmp/sc.szip")
        subprocess.run([binpath, "d", "/tmp/sc.szip", "/tmp/sc.out"], capture_output=True)
        rt = open(path, "rb").read() == open("/tmp/sc.out", "rb").read()
        won = sz <= alt
        wins += won; losses += (not won)
        tag = "WIN" if won else "loss"
        edge = (alt - sz) * 100.0 / alt
        bad = ""
        if not rt:
            bad = "  ROUNDTRIP BROKEN"; fail += 1
        if name in WIN_ROWS and not won:
            bad += "  *** REGRESSION: golden win flipped ***"; fail += 1
        print(f"{name:8} {sz:8,}  vs {alt:8,}  {edge:+6.2f}%  {tag}{bad}", flush=True)
    print(f"\n{wins}W/{losses}L  ({len(WIN_ROWS)} golden wins required)")
    if wins < len(WIN_ROWS):
        print("FAIL: win count below golden 14W"); fail += 1
    if fail == 0:
        print("scorecheck PASSED")
    return 1 if fail else 0

if __name__ == "__main__":
    sys.exit(main())
