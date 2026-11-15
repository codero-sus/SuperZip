#!/usr/bin/env python3
"""Generate the 20-corpus universe benchmark set (deterministic, stdlib only).

16 synthetic "data types" of 256 KiB each; the classic 4 corpora (code, text,
rand, src) come from tests/corpus/ and are handled by benchall.py.  Every
generator is seeded, so `python3 tests/corpus20.py` reproduces the exact same
bytes on any machine with Python 3.

usage: python3 tests/corpus20.py [outdir]   (default: build/corpus20)
"""
import base64
import json
import math
import os
import random
import struct
import sys

SIZE = 128 * 1024


def fit(data, size=SIZE):
    if isinstance(data, str):
        data = data.encode("utf-8")
    data = bytes(data)
    if len(data) >= size:
        return data[:size]
    # repeat the tail pattern-free: pad by cycling (deterministic)
    out = bytearray(data)
    while len(out) < size:
        out += data[: min(len(data), size - len(out))]
    return bytes(out)


def zipf(rng, items, k=1.3):
    w = [1.0 / (i + 1) ** k for i in range(len(items))]
    return rng.choices(items, weights=w, k=1)[0]


WORDS = """the of and to in a is that for it as was with be by on not he this
are or his from at which but have an had they you were their one all we can
her has there been if more when will would who so no she other its may these
than then some very time upon about could them into only first any my now
such like our over man me even most made after also did many before must
through back years where much your way well down should because each just
those people mr how too little state good make world still own see men work
long get here between both life being under never day same another know
while last might us great old year off come since against go came right used
take three states himself few house use during without again place american
around however home small found mrs thought went say part once general high
upon school every don't does got united left number course war until always
away something fact though water less public put think almost hand enough
far took head yet government system better set told nothing night end why
called didn't eyes find going look asked later knew point next city business
case order stay early gave really name certain however several home""".split()

PROSE_BIGRAMS = None  # built lazily


def prose_sentence(rng, nwords=None):
    n = nwords or rng.randint(6, 22)
    words = [zipf(rng, WORDS) for _ in range(n)]
    s = " ".join(words)
    if rng.random() < 0.12:
        s += ", " + " ".join(zipf(rng, WORDS) for _ in range(3))
    return s[0].upper() + s[1:] + rng.choice(". . . ? !".split())


def gen_prose(rng):
    paras = []
    while sum(len(p) for p in paras) < SIZE:
        para = " ".join(prose_sentence(rng) for _ in range(rng.randint(4, 9)))
        paras.append(para + "\n\n")
    return fit("".join(paras))


def gen_dna(rng):
    motifs = [bytes(rng.choices(b"ACGT", k=rng.randint(5, 24)))
              for _ in range(40)]
    out = bytearray()
    while len(out) < SIZE:
        r = rng.random()
        if r < 0.55:
            out += rng.choice(motifs)
            if rng.random() < 0.3:
                out += rng.choice(motifs[:8])  # tandem repeats
        elif r < 0.75:
            out += bytes(rng.choices(b"ACGT", k=rng.randint(1, 40)))
        elif r < 0.85:
            out += b"N" * rng.randint(3, 30)  # unknown runs
        else:
            m = rng.choice(motifs)  # reverse complement
            tbl = bytes.maketrans(b"ACGT", b"TGCA")
            out += m.translate(tbl)[::-1]
    return fit(out)


def gen_log(rng):
    ips = [f"10.0.{rng.randint(0, 4)}.{rng.randint(1, 254)}" for _ in range(40)]
    paths = ["/", "/api/v1/items", "/api/v1/users", "/static/app.js",
             "/static/main.css", "/images/logo.png", "/health", "/login",
             "/search?q=widget", "/index.html"]
    uas = ["Mozilla/5.0 (X11; Linux x86_64) Chrome/120.0",
           "Mozilla/5.0 (Windows NT 10.0) Firefox/121.0",
           "curl/8.4.0", "Googlebot/2.1", "python-requests/2.31"]
    out = []
    total = 0
    while total < SIZE:
        ip = zipf(rng, ips)
        ts = f"28/Oct/2026:{rng.randint(0,23):02d}:{rng.randint(0,59):02d}:{rng.randint(0,59):02d} +0000"
        st = rng.choices([200, 200, 200, 200, 304, 404, 500], k=1)[0]
        line = (f'{ip} - - [{ts}] "GET {zipf(rng, paths)} HTTP/1.1" {st} '
                f'{rng.randint(120, 88000)} "-" "{zipf(rng, uas)}"\n')
        out.append(line)
        total += len(line)
    return fit("".join(out))


def gen_json(rng):
    tags = ["alpha", "beta", "gamma", "delta", "core", "edge", "stable", "dev"]
    recs = []
    total = 0
    i = 0
    while total < SIZE:
        rec = {
            "id": i,
            "name": f"item-{i % 997:04d}",
            "active": rng.random() < 0.8,
            "score": round(rng.gauss(50, 12), 3),
            "tags": [zipf(rng, tags) for _ in range(rng.randint(1, 4))],
            "owner": {"name": zipf(rng, WORDS), "dept": f"dept-{rng.randint(1, 9)}"},
            "metrics": {"cpu": round(rng.random(), 4), "mem": round(rng.random(), 4)},
        }
        s = json.dumps(rec, sort_keys=True) + "\n"
        recs.append(s)
        total += len(s)
        i += 1
    return fit("".join(recs))


def gen_xml(rng):
    cities = ["Delhi", "Mumbai", "Bengaluru", "Pune", "Jaipur", "Chennai"]
    out = []
    total = 0
    out.append('<?xml version="1.0" encoding="UTF-8"?>\n<catalog>\n')
    while total < SIZE:
        rec = (f'  <product id="{rng.randint(1000, 9999)}" sku="SKU-{rng.randint(100,999)}">\n'
               f'    <name>{zipf(rng, WORDS)} {zipf(rng, WORDS)}</name>\n'
               f'    <price currency="INR">{rng.randint(99, 99999)}.{rng.randint(0,99):02d}</price>\n'
               f'    <stock warehouse="{zipf(rng, cities)}">{rng.randint(0, 500)}</stock>\n'
               f'    <desc>{prose_sentence(rng, 8)}</desc>\n'
               f'  </product>\n')
        out.append(rec)
        total += len(rec)
    out.append('</catalog>\n')
    return fit("".join(out))


def gen_csv(rng):
    cats = ["A", "B", "C", "D"]
    out = ["date,region,category,units,price,revenue\n"]
    total = 0
    while total < SIZE:
        row = (f"2026-{rng.randint(1,12):02d}-{rng.randint(1,28):02d},"
               f"r{rng.randint(1,6)},{zipf(rng, cats)},"
               f"{rng.randint(0,400)},{rng.randint(100,25000)/100:.2f},"
               f"{rng.randint(0,999999)/100:.2f}\n")
        out.append(row)
        total += len(row)
    return fit("".join(out))


def gen_html(rng):
    nav = ['<a href="/products">Products</a>', '<a href="/about">About</a>',
           '<a href="/contact">Contact</a>', '<a href="/blog">Blog</a>']
    out = ['<!DOCTYPE html>\n<html lang="en">\n<head><meta charset="utf-8">']
    total = 0
    while total < SIZE:
        page = (f'\n<title>{prose_sentence(rng, 4)}</title>\n'
                f'<link rel="stylesheet" href="/static/main.css">\n</head>\n<body>\n'
                f'<nav>{"".join(nav)}</nav>\n<main>\n<h1>{prose_sentence(rng, 5)}</h1>\n'
                f'<p>{prose_sentence(rng, 18)}</p>\n<p>{prose_sentence(rng, 22)}</p>\n'
                f'<div class="card"><span class="price">Rs {rng.randint(100, 9000)}</span>'
                f'<button>Add</button></div>\n</main>\n<footer><p>&copy; 2026</p></footer>\n</body>\n</html>\n')
        out.append(page)
        total += len(page)
    return fit("".join(out))


def gen_binary(rng):
    out = bytearray()
    out += b"\x7fELF" + bytes([2, 1, 1, 0])  # ELF-ish magic
    out += struct.pack("<HHIQQQIHHHHHH", 2, 62, 1, 0x401000, 64, 0, 0, 64, 56, 0, 0, 0, 0)
    out += b"\0" * 24
    # .text: opcode-like bytes (x86-64 statistics), imm32s, calls to shared plt
    text = bytearray()
    while len(text) < SIZE * 0.55:
        r = rng.random()
        if r < 0.5:
            text += bytes([rng.choice([0x48, 0x89, 0x8b, 0x83, 0x8d, 0xe8, 0xe9,
                                       0x55, 0x5d, 0xc3, 0x0f, 0x84, 0x74, 0x75])])
        elif r < 0.75:
            text += rng.randbytes(rng.randint(1, 3))
        elif r < 0.9:
            text += struct.pack("<i", rng.randint(-128, 128))  # small immediates
        else:
            text += struct.pack("<i", rng.randint(0x400000, 0x500000))  # code addr
        if rng.random() < 0.02:
            text += b"\0" * rng.randint(1, 16)  # alignment padding
    out += text
    # .rodata: C strings + zero padding
    while len(out) < SIZE * 0.8:
        out += rng.choice(WORDS).encode() + b"=%s\n\0"
        if rng.random() < 0.3:
            out += b"\0" * rng.randint(8, 64)
    # .data: pointers with shared high bytes + small ints
    while len(out) < SIZE:
        out += struct.pack("<Q", 0x0000000000400000 + rng.randint(0, 0x20000))
    return fit(out)


def gen_pcm(rng):
    out = bytearray()
    rate = 8000
    f1, f2 = 220.0, 330.0
    t = 0.0
    while len(out) < SIZE:
        env = 0.55 + 0.45 * math.sin(2 * math.pi * t / (rate * 3.1))
        v = (0.45 * math.sin(2 * math.pi * f1 * t / rate) +
             0.28 * math.sin(2 * math.pi * f2 * t / rate) +
             0.12 * math.sin(2 * math.pi * (f1 * 2.02) * t / rate))
        v = v * env + rng.uniform(-0.015, 0.015)
        out += struct.pack("<h", int(max(-1, min(1, v)) * 32000))
        t += 1
        if t % rate == 0:  # occasional slow vibrato drift
            f1 = 200 + 60 * math.sin(t / (rate * 7))
    return fit(out)


def gen_sparse(rng):
    out = bytearray(SIZE)
    pos = 0
    n = 0
    while pos < SIZE - 256:
        pos += rng.randint(4096, 20000)
        if pos >= SIZE - 256:
            break
        rec = struct.pack("<III", n, rng.randint(0, 2**20), rng.randint(1, 99))
        rec += rng.randbytes(rng.randint(8, 96))
        out[pos:pos + len(rec)] = rec
        n += 1
    return bytes(out)


def gen_b64(rng):
    inner = []
    while sum(len(x) for x in inner) < SIZE * 3 // 4:
        inner.append(gen_prose(random.Random(rng.getrandbits(32))))
        inner.append(gen_json(random.Random(rng.getrandbits(32)))[:8192])
    raw = fit(b"".join(inner), SIZE * 3 // 4)
    enc = base64.b64encode(raw)
    lines = [enc[i:i + 76].decode() + "\n" for i in range(0, len(enc), 76)]
    return fit("".join(lines))


def gen_hex(rng):
    raw = gen_binary(random.Random(7))
    out = []
    for off in range(0, min(len(raw), SIZE * 3 // 4), 16):
        chunk = raw[off:off + 16]
        hexs = " ".join(f"{b:02x}" for b in chunk)
        asc = "".join(chr(b) if 32 <= b < 127 else "." for b in chunk)
        out.append(f"{off:08x}  {hexs:<47}  |{asc}|\n")
    return fit("".join(out))


def gen_floats(rng):
    vals = []
    t = 0.0
    while len(vals) * 8 < SIZE * 0.7:
        v = (math.sin(t / 900) * 40 + math.sin(t / 77) * 6 +
             math.sin(t / 13) * 1.5 + rng.gauss(0, 0.4))
        vals.append(v)
        t += 1
    while len(vals) * 8 < SIZE:
        vals.append(round(rng.uniform(0, 1), 3))  # quantized readings
    return fit(struct.pack("<" + "d" * len(vals), *vals))


def gen_dict(rng):
    words = sorted(set(WORDS) | {w * 2 for w in WORDS[:40]} |
                   {w + "ing" for w in WORDS} | {w + "ed" for w in WORDS} |
                   {w + "s" for w in WORDS})
    out = []
    total = 0
    i = 0
    while total < SIZE:
        w = words[i % len(words)]
        line = f"{w}\t{rng.randint(1, 50000)}\n"
        out.append(line)
        total += len(line)
        i += 1
    return fit("".join(out))


def gen_utf8(rng):
    scripts = [
        ["the", "quick", "brown", "fox", "jumps", "over", "lazy", "dog"],
        ["привет", "мир", "это", "простой", "тестовый", "текст", "строка"],
        ["你好", "世界", "这是", "一段", "测试", "文字", "内容"],
        ["bonjour", "le", "monde", "voici", "un", "texte", "simple"],
        ["Hola", "el", "mundo", "esto", "es", "una", "prueba"],
    ]
    out = []
    total = 0
    while total < SIZE:
        s = " ".join(zipf(rng, rng.choice(scripts)) for _ in range(rng.randint(5, 18)))
        out.append(s + rng.choice(["。", ". ", ".\n", "! ", "？"]))
        total += len(s)
    return fit("".join(out))


def gen_mixed(parts, rng):
    names = sorted(parts)
    out = bytearray()
    chunk = 16 * 1024
    idx = 0
    while len(out) < SIZE:
        name = names[idx % len(names)]
        data = parts[name]
        off = rng.randrange(0, max(1, len(data) - chunk))
        out += data[off:off + chunk]
        idx += 1
    return bytes(out)


GENS = {
    "prose": (gen_prose, 11), "dna": (gen_dna, 12), "log": (gen_log, 13),
    "json": (gen_json, 14), "xml": (gen_xml, 15), "csv": (gen_csv, 16),
    "html": (gen_html, 17), "binary": (gen_binary, 18), "pcm": (gen_pcm, 19),
    "sparse": (gen_sparse, 20), "b64": (gen_b64, 21), "hex": (gen_hex, 22),
    "floats": (gen_floats, 23), "dict": (gen_dict, 24), "utf8": (gen_utf8, 25),
}


def main():
    outdir = sys.argv[1] if len(sys.argv) > 1 else os.path.join("build", "corpus20")
    os.makedirs(outdir, exist_ok=True)
    parts = {}
    for name, (fn, seed) in sorted(GENS.items()):
        data = fn(random.Random(seed))
        assert len(data) == SIZE, (name, len(data))
        parts[name] = data
        path = os.path.join(outdir, name + ".bin")
        with open(path, "wb") as f:
            f.write(data)
        print(f"{name:8s} {len(data):8d} B  {path}")
    mixed = gen_mixed(parts, random.Random(26))
    with open(os.path.join(outdir, "mixed.bin"), "wb") as f:
        f.write(mixed)
    print(f"{'mixed':8s} {len(mixed):8d} B  {os.path.join(outdir, 'mixed.bin')}")


if __name__ == "__main__":
    main()
