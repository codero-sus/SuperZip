#!/usr/bin/env python3
"""Golden byte-identity check: new engine must reproduce the committed
engine's .szip output byte-for-byte on every corpus row (and roundtrip).

Usage: python3 tests/goldcheck.py <ref-bin> <new-bin>
"""
import subprocess, sys, os

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
    ref, new = sys.argv[1], sys.argv[2]
    ok = fail = 0
    for name, path in ROWS:
        subprocess.run([ref, "c", path, "/tmp/gc_ref.szip"], capture_output=True)
        subprocess.run([new, "c", path, "/tmp/gc_new.szip"], capture_output=True)
        a = open("/tmp/gc_ref.szip", "rb").read()
        b = open("/tmp/gc_new.szip", "rb").read()
        subprocess.run([new, "d", "/tmp/gc_new.szip", "/tmp/gc_new.out"], capture_output=True)
        rt = open(path, "rb").read() == open("/tmp/gc_new.out", "rb").read()
        if a == b and rt:
            ok += 1
            print(f"{name:8} IDENTICAL + roundtrip OK ({len(a):,} B)")
        else:
            fail += 1
            why = []
            if a != b: why.append("BYTES DIFFER")
            if not rt: why.append("ROUNDTRIP BROKEN")
            print(f"{name:8} FAIL: {', '.join(why)}")
    print(f"\n{ok} identical, {fail} failed")
    return 1 if fail else 0

if __name__ == "__main__":
    sys.exit(main())
