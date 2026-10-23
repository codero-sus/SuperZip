#!/bin/sh
# G0D SZIP — deterministic benchmark corpora recipes.
# Writes build/corpus_{code,text,rand}.bin
#   code : frozen tests/corpus/code.bin (hand-written sources at freeze time)
#   text : frozen tests/corpus/text.bin (word-salad, python3 seed 1)
#   rand : /dev/urandom 500000 (fresh per run; ratio ~100% is the point)
set -e
cd "$(dirname "$0")/.."
mkdir -p build
cat native/*.cpp native/*.hpp native/*.h docs/*.md README.md \
  | grep -v '^' /dev/null >/dev/null 2>&1 || true
# exclude generated tables.hpp from the code corpus
rm -f build/corpus_code.bin
for f in native/*.cpp native/*.hpp native/*.h docs/*.md README.md; do
  [ "$f" = "native/tables.hpp" ] && continue
  cat "$f" >> build/corpus_code.bin
done
python3 - <<'EOF'
import random
random.seed(1)
words=("the quick brown fox jumps over lazy dog compression algorithm entropy coder "
       "range arithmetic match finder window hash chain block archive format superzip "
       "god szip data stream bytes bits probability model adaptive update literal "
       "offset length token payload header footer integrity checksum").split()
out=[]; size=0
while size < 500000:
    w=random.choice(words); out.append(w); size+=len(w)+1
open('build/corpus_text.bin','w').write(' '.join(out))
EOF
head -c 500000 /dev/urandom > build/corpus_rand.bin
wc -c build/corpus_*.bin
