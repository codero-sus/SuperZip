#!/usr/bin/env python3
"""Golden gate for G0D SZIP.

  1. `--max` tier (deep search) must reproduce the frozen v8u golden sizes
     byte-count-for-byte-count on every row AND roundtrip exactly — this is
     the byte-identity guarantee for the 14W/6L scoreboard era.
  2. the DEFAULT tier must roundtrip every row exactly.

Usage: python3 tests/goldcheck.py [binary]     (default build/g0dszip)
Exit 0 only if every check passes.
"""
import subprocess, sys, os

GOLD = {"prose":26408,"dict":34438,"text":53000,"code":17186,"xml":13914,"json":10752,
        "log":8917,"html":12015,"csv":32601,"utf8":14763,"b64":24407,"hex":34243,
        "dna":19005,"src":52132,"floats":92795,"pcm":101873,"binary":64266,
        "sparse":780,"mixed":52067,"rand":500004}
ROWS = [("prose","build/corpus20/prose.bin"),("dict","build/corpus20/dict.bin"),
        ("text","build/corpus_text.bin"),("code","build/corpus_code.bin"),
        ("xml","build/corpus20/xml.bin"),("json","build/corpus20/json.bin"),
        ("log","build/corpus20/log.bin"),("html","build/corpus20/html.bin"),
        ("csv","build/corpus20/csv.bin"),("utf8","build/corpus20/utf8.bin"),
        ("b64","build/corpus20/b64.bin"),("hex","build/corpus20/hex.bin"),
        ("dna","build/corpus20/dna.bin"),("src","tests/corpus/src.bin"),
        ("floats","build/corpus20/floats.bin"),("pcm","build/corpus20/pcm.bin"),
        ("binary","build/corpus20/binary.bin"),("sparse","build/corpus20/sparse.bin"),
        ("mixed","build/corpus20/mixed.bin"),("rand","build/corpus_rand.bin")]

def main():
    binpath = sys.argv[1] if len(sys.argv) > 1 else "build/g0dszip"
    ok = fail = 0
    for name, path in ROWS:
        raw = open(path, "rb").read()
        # --max: golden size + roundtrip
        subprocess.run([binpath, "c", "--max", path, "/tmp/gc_m.szip"], capture_output=True)
        sz = os.path.getsize("/tmp/gc_m.szip")
        subprocess.run([binpath, "d", "/tmp/gc_m.szip", "/tmp/gc_m.out"], capture_output=True)
        rt_m = raw == open("/tmp/gc_m.out", "rb").read()
        # default: roundtrip
        subprocess.run([binpath, "c", path, "/tmp/gc_d.szip"], capture_output=True)
        subprocess.run([binpath, "d", "/tmp/gc_d.szip", "/tmp/gc_d.out"], capture_output=True)
        rt_d = raw == open("/tmp/gc_d.out", "rb").read()
        if sz == GOLD[name] and rt_m and rt_d:
            ok += 1
            print(f"{name:8} --max golden {sz:,} OK + roundtrips OK")
        else:
            fail += 1
            why = []
            if sz != GOLD[name]: why.append(f"--max size {sz} != {GOLD[name]}")
            if not rt_m: why.append("--max roundtrip BROKEN")
            if not rt_d: why.append("default roundtrip BROKEN")
            print(f"{name:8} FAIL: {', '.join(why)}")
    print(f"\n{ok} passed, {fail} failed")
    return 1 if fail else 0

if __name__ == "__main__":
    sys.exit(main())
