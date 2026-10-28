#!/bin/sh
# G0D SZIP — deterministic benchmark corpora recipes.
# Writes build/corpus_{code,text,rand}.bin
#   code : frozen tests/corpus/code.bin (hand-written sources at freeze time)
#   text : frozen tests/corpus/text.bin (word-salad, python3 seed 1)
#   rand : /dev/urandom 500000 (fresh per run; ratio ~100% is the point)
#
# The code/text corpora are FROZEN bytes committed under tests/corpus/ so
# benchmark claims stay reproducible as the project sources evolve.
set -e
cd "$(dirname "$0")/.."
mkdir -p build
cp tests/corpus/code.bin build/corpus_code.bin
cp tests/corpus/text.bin build/corpus_text.bin
head -c 500000 /dev/urandom > build/corpus_rand.bin
wc -c build/corpus_*.bin

# real-world corpus: frozen snapshot of this repository's own source tree
if [ "$1" = "src" ]; then
  out=tests/corpus/src.bin
  { find native tests docs rust -type f ! -name '*.bin' ! -name '*.szip' -print0 |
      sort -z | xargs -0 cat; cat README.md; } > "$out"
  echo "regenerated $out ($(wc -c < "$out") B) — commit it to re-freeze"
fi
