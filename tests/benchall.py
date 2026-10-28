#!/usr/bin/env python3
"""G0D SZIP — benchmark against every compressor available.

Written, runnable check: builds the frozen corpora (tests/mkcorpus.sh), runs
every compressor that exists on this machine at (near) maximum settings,
verifies each roundtrip byte-exact, and prints an honest scoreboard.

  sh tests/benchall.sh          # wrapper: ensures build/venv deps, then runs this

Non-gating: the strict 4%-under-xz gate lives in tests/beatsniper.sh.
This script reports the universe table; exit code 0 as long as every
roundtrip verifies.
"""
import os
import shutil
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD = os.path.join(ROOT, "build")
G0D = os.path.join(BUILD, "g0dszip")


def run(cmd, **kw):
    return subprocess.run(cmd, cwd=ROOT, capture_output=True, **kw)


def fmt(n):
    return f"{n:,}"


class Case:
    def __init__(self, name, compress, decompress):
        self.name = name
        self._c = compress
        self._d = decompress

    def run(self, data):
        t0 = time.perf_counter()
        comp = self._c(data)
        t1 = time.perf_counter()
        raw = self._d(comp)
        t2 = time.perf_counter()
        return comp, raw, t1 - t0, t2 - t1


def g0d_case(fast):
    def c(data):
        p = os.path.join(BUILD, "bench_in")
        open(p, "wb").write(data)
        out = os.path.join(BUILD, "bench_g.szip")
        args = [G0D, "c"] + (["-f"] if fast else []) + [p, out]
        r = run(args)
        if r.returncode != 0:
            raise RuntimeError("g0dszip compress failed")
        return open(out, "rb").read()

    def d(comp):
        pc = os.path.join(BUILD, "bench_comp.szip")
        open(pc, "wb").write(comp)
        out = os.path.join(BUILD, "bench_g.out")
        r = run([G0D, "d", pc, out])
        if r.returncode != 0:
            raise RuntimeError("g0dszip decompress failed")
        return open(out, "rb").read()

    return Case("g0dszip" + (" -f (fast)" if fast else " (best)"), c, d)


def lib_case(name, cfn, dfn):
    return Case(name, cfn, dfn)


def build_cases():
    cases = [g0d_case(False), g0d_case(True)]
    if shutil.which("gzip"):
        cases.append(_shell_case("gzip -9", "gzip -9 -c {inp} > {out}", "gzip -dc {inp} > {out}"))
    if shutil.which("bzip2"):
        cases.append(_shell_case("bzip2 -9", "bzip2 -9 -c {inp} > {out}", "bzip2 -dc {inp} > {out}"))
    if shutil.which("xz"):
        cases.append(_shell_case("xz -9e", "xz -9e -c {inp} > {out}", "xz -dc {inp} > {out}"))
    try:
        import zstandard as zstd

        def zc(data):
            return zstd.compress(data, level=22)

        def zd(comp):
            return zstd.decompress(comp)

        cases.append(lib_case("zstd -22 (ultra)", zc, zd))
    except Exception:
        pass
    try:
        import brotli

        def bc(data):
            return brotli.compress(data, quality=11, lgwin=24)

        def bd(comp):
            return brotli.decompress(comp)

        cases.append(lib_case("brotli -q 11 -w 24", bc, bd))
    except Exception:
        pass
    try:
        import lz4.frame as lz4f

        def lc(data):
            return lz4f.compress(data, compression_level=12, block_linked=True)

        def ld(comp):
            return lz4f.decompress(comp)

        cases.append(lib_case("lz4 hc-12", lc, ld))
    except Exception:
        pass
    try:
        import cramjam

        def sc(data):
            return bytes(cramjam.snappy.compress(data))

        def sd(comp):
            return bytes(cramjam.snappy.decompress(comp))

        cases.append(lib_case("snappy", sc, sd))
    except Exception:
        pass
    return cases


def _shell_case(name, cdecl, ddecl):
    def c(data):
        p = os.path.join(BUILD, "bench_in")
        open(p, "wb").write(data)
        out = os.path.join(BUILD, "bench_c.bin")
        run(["bash", "-c", cdecl.format(inp=p, out=out)])
        return open(out, "rb").read()

    def d(comp):
        pc = os.path.join(BUILD, "bench_comp.bin")
        open(pc, "wb").write(comp)
        out = os.path.join(BUILD, "bench_d.bin")
        r = run(["bash", "-c", ddecl.format(inp=pc, out=out)])
        if r.returncode != 0:
            raise RuntimeError(f"{name} decompress failed")
        return open(out, "rb").read()

    return Case(name, c, d)


def main():
    if not os.path.exists(G0D):
        sys.exit("build/g0dszip missing — run: make  (or the g++ line in the README)")
    run(["bash", "tests/mkcorpus.sh"])
    corpora = [
        ("code", os.path.join(BUILD, "corpus_code.bin")),
        ("text", os.path.join(BUILD, "corpus_text.bin")),
        ("rand", os.path.join(BUILD, "corpus_rand.bin")),
        ("src", os.path.join(ROOT, "tests", "corpus", "src.bin")),
    ]
    # "src" is a frozen real-world corpus (this repository's own source tree
    # snapshot) so universe numbers stay reproducible; regenerate with:
    #   ./tests/mkcorpus.sh src
    if not os.path.exists(corpora[-1][1]):
        sys.exit("tests/corpus/src.bin missing — run ./tests/mkcorpus.sh src")
    # 16 generated data types (deterministic): tests/corpus20.py
    c20 = os.path.join(BUILD, "corpus20")
    want20 = ["b64", "binary", "csv", "dict", "dna", "floats", "hex", "html",
              "json", "log", "mixed", "pcm", "prose", "sparse", "utf8", "xml"]
    if not all(os.path.exists(os.path.join(c20, n + ".bin")) for n in want20):
        run([sys.executable, os.path.join("tests", "corpus20.py"), c20])
    corpora += [(n, os.path.join(c20, n + ".bin")) for n in want20]

    only = None
    for a in sys.argv[1:]:
        if a.startswith("--only="):
            only = set(a.split("=", 1)[1].split(","))
    if only:
        corpora = [c for c in corpora if c[0] in only]
        if not corpora:
            sys.exit(f"--only matched nothing; have: {sorted(n for n, _ in corpora)}")

    cases = build_cases()
    failures = 0
    totals = {}
    print("G0D SZIP — universe benchmark  (single-run wall times, indicative)")
    print(f"compressors found: {len(cases)}")
    for cname, path in corpora:
        data = open(path, "rb").read()
        print(f"\n=== {cname}  ({fmt(len(data))} B) ===")
        rows = []
        for case in cases:
            try:
                comp, raw, tc, td = case.run(data)
                ok = raw == data
            except Exception as e:
                print(f"  {case.name:28s}  FAILED ({e})")
                failures += 1
                continue
            if not ok:
                print(f"  {case.name:28s}  ROUNDTRIP CORRUPT")
                failures += 1
                continue
            rows.append((len(comp), case.name, tc, td))
        rows.sort()
        best_other = None
        for size, name, tc, td in rows:
            ratio = 100.0 * size / len(data) if data else 0
            tag = " <-- G0D" if name.startswith("g0dszip") else ""
            mark = ""
            if not name.startswith("g0dszip") and best_other is None:
                best_other = (size, name)
            print(f"  {name:28s}  {fmt(size):>12s} B  {ratio:6.2f}%"
                  f"  c {tc:6.2f}s  d {td:6.2f}s  roundtrip ok{tag}{mark}")
        g0d = [r for r in rows if r[1].startswith("g0dszip (best)")]
        if g0d and best_other:
            gs = g0d[0][0]
            delta = 100.0 * (best_other[0] - gs) / best_other[0]
            win = gs <= best_other[0]
            totals[cname] = (win, delta, best_other[1])
            print(f"  -> vs best alternative ({best_other[1]} {fmt(best_other[0])} B): "
                  f"{'WIN by %.2f%%' % delta if win else 'behind by %.2f%%' % (-delta)}")
    print("\n=== universe verdict ===")
    wins = sum(1 for v in totals.values() if v[0])
    for k, (win, delta, who) in totals.items():
        print(f"  {k:5s} {'WIN' if win else 'LOSS'}  vs {who}  ({'+' if win else ''}{delta:.2f}%)")
    print(f"\n  {wins}/{len(totals)} first places")
    print("  note: zstd/brotli/lz4/snappy need build/venv (sh tests/benchall.sh);")
    print("        extensible codec IDs in .szip v5 leave room for more models.")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
