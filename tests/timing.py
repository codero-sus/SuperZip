#!/usr/bin/env python3
"""Best-of-2 timing for G0DSZIP encode+decode across all 20 corpus rows.
Usage: python3 tests/timing.py <old-bin> <new-bin>"""
import subprocess, sys, time, os

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

def best_of_2(cmd, out, err):
    ts = []
    for _ in range(2):
        t = time.time()
        subprocess.run(cmd, stdout=out, stderr=err)
        ts.append(time.time() - t)
    return min(ts)

def main():
    old, new = sys.argv[1], sys.argv[2]
    null = open(os.devnull, "w")
    eo = en = 0.0; do = dn = 0.0
    print(f"{'row':8} {'enc-old':>8} {'enc-new':>8} {'speedup':>7} {'dec-old':>8} {'dec-new':>8} {'speedup':>7}")
    for name, path in ROWS:
        results = {}
        for tag, b in (("old", old), ("new", new)):
            e = best_of_2([b, "c", path, "/tmp/tw.szip"], null, null)
            d = best_of_2([b, "d", "/tmp/tw.szip", "/tmp/tw.out"], null, null)
            results[tag] = (e, d)
        (e1, d1), (e2, d2) = results["old"], results["new"]
        eo += e1; en += e2; do += d1; dn += d2
        print(f"{name:8} {e1:8.2f} {e2:8.2f} {e1/max(e2,1e-9):6.2f}x {d1:8.3f} {d2:8.3f} {d1/max(d2,1e-9):6.2f}x", flush=True)
    print(f"{'TOTAL':8} {eo:8.2f} {en:8.2f} {eo/en:6.2f}x {do:8.3f} {dn:8.3f} {do/dn:6.2f}x")

if __name__ == "__main__":
    main()
