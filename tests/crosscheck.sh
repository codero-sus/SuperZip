#!/bin/sh
# G0D SZIP — cross-engine compatibility checks.
# Proves the C++ engine, the Rust engine and the merged Rust+C++ engine
# produce/consume byte-identical .szip archives.
set -e

cd "$(dirname "$0")/.."
mkdir -p build

echo "== building C++ engine =="
make all >/dev/null

# test corpora
cat native/*.cpp native/*.hpp > build/x_code.bin
python3 -c "
import random
random.seed(2)
w='alpha beta gamma delta epsilon zeta eta theta superzip g0d szip compress archive'.split()
out=[]; n=0
while n < 200000:
    x=random.choice(w); out.append(x); n+=len(x)+1
open('build/x_text.bin','w').write(' '.join(out))
" 2>/dev/null || echo "alpha beta gamma delta superzip" > build/x_text.bin
head -c 200000 /dev/urandom > build/x_rand.bin
: > build/x_empty.bin

CPP=build/g0dszip

roundtrip() { # engine_enc engine_dec file
  "$1" c "$3" build/x.szip
  "$2" d build/x.szip build/x.out
  cmp -s "$3" build/x.out
}

echo "== C++ engine self-roundtrips =="
for f in x_empty.bin x_code.bin x_text.bin x_rand.bin; do
  roundtrip "$CPP" "$CPP" "build/$f"
  echo "   ok: $f"
done

if ! command -v cargo >/dev/null 2>&1; then
  echo "== cargo not found: skipping Rust/cross-engine checks =="
  echo "   (the Rust workspace in rust/ is ready; run this script where cargo exists)"
  exit 0
fi

echo "== building Rust workspace (szip-core, szip-sys, CLIs) =="
cargo build --release --manifest-path rust/Cargo.toml >/dev/null

RS=../rust/target/release/g0dszip
RSPP=../rust/target/release/g0dszip-cpp
RS=$(pwd)/rust/target/release/g0dszip
RSPP=$(pwd)/rust/target/release/g0dszip-cpp

echo "== Rust engine self-roundtrips =="
for f in x_empty.bin x_code.bin x_text.bin x_rand.bin; do
  roundtrip "$RS" "$RS" "build/$f"
  echo "   ok: $f"
done

echo "== merged engine (Rust+C++) self-roundtrips =="
for f in x_empty.bin x_code.bin x_text.bin x_rand.bin; do
  roundtrip "$RSPP" "$RSPP" "build/$f"
  echo "   ok: $f"
done

echo "== cross-engine decode matrix =="
for f in x_code.bin x_text.bin x_rand.bin; do
  for enc in "$CPP" "$RS" "$RSPP"; do
    "$enc" c "build/$f" build/x.szip
    for dec in "$CPP" "$RS" "$RSPP"; do
      "$dec" d build/x.szip build/x.out
      cmp -s "build/$f" build/x.out
    done
  done
  echo "   ok: $f decodes on all engines"
done

echo "== determinism: encoders must be byte-identical =="
for f in x_code.bin x_text.bin x_rand.bin; do
  "$CPP" c "build/$f" build/a1.szip
  "$RS" c "build/$f" build/a2.szip
  "$RSPP" c "build/$f" build/a3.szip
  cmp -s build/a1.szip build/a2.szip
  cmp -s build/a1.szip build/a3.szip
  echo "   ok: $f"
done

echo "ALL CROSSCHECKS PASSED"
