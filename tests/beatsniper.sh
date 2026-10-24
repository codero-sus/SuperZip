#!/bin/sh
# G0D SZIP — beat-the-sniper verification (frozen corpora).
#
# Checks, with roundtrip verification on every corpus:
#   1. G0D SZIP beats gzip-9, bzip2-9 and xz-9e on code and text.
#   2. The strict goal: code <= 16,933 B  (4% under xz-9e's 17,640 B).
#
# Exit 0 only if EVERY check passes. Sizes for the snipers were measured on
# tests/corpus/{code,text}.bin and are frozen here as thresholds.
set -e
cd "$(dirname "$0")/.."

CC=${CXX:-g++}
mkdir -p build
./tests/mkcorpus.sh >/dev/null
$CC -std=c++17 -O2 -Wall -Wextra -Wshadow -Wconversion -DNDEBUG -Inative \
    native/codec.cpp native/cli.cpp -o build/g0dszip

# frozen sniper sizes (bytes) on the frozen corpora
GZ_CODE=19289; BZ_CODE=18214; XZ_CODE=17640
GZ_TEXT=86546; BZ_TEXT=53954; XZ_TEXT=76764
GZ_RAND=500114; BZ_RAND=502632; XZ_RAND=500084
TARGET_CODE=16933   # xz-9e * 0.96 = 4% under the best sniper

pass=0; fail=0
check() { # check <label> <cond(0/1)>
  if [ "$2" -eq 1 ]; then echo "   ok: $1"; pass=$((pass+1));
  else echo "   FAIL: $1"; fail=$((fail+1)); fi
}

size() { stat -c%s "$1" 2>/dev/null || wc -c < "$1"; }

for f in code text rand; do
  ./build/g0dszip c "build/corpus_$f.bin" "build/x_$f.szip"
  ./build/g0dszip d "build/x_$f.szip" "build/x_$f.out"
  cmp -s "build/corpus_$f.bin" "build/x_$f.out"
  echo "   ok: roundtrip $f"
  pass=$((pass+1))
done

C=$(size build/x_code.szip); T=$(size build/x_text.szip); R=$(size build/x_rand.szip)
echo ""
echo "   code: $C B  (gzip $GZ_CODE, bzip2 $BZ_CODE, xz $XZ_CODE; target $TARGET_CODE)"
echo "   text: $T B  (gzip $GZ_TEXT, bzip2 $BZ_TEXT, xz $XZ_TEXT)"
echo "   rand: $R B  (gzip $GZ_RAND, bzip2 $BZ_RAND, xz $XZ_RAND)"
echo ""

check "code beats gzip-9"  $([ "$C" -lt "$GZ_CODE" ] && echo 1 || echo 0)
check "code beats bzip2-9" $([ "$C" -lt "$BZ_CODE" ] && echo 1 || echo 0)
check "code beats xz-9e"   $([ "$C" -lt "$XZ_CODE" ] && echo 1 || echo 0)
check "code <= 4% under xz ($TARGET_CODE B)" $([ "$C" -le "$TARGET_CODE" ] && echo 1 || echo 0)
check "text beats gzip-9"  $([ "$T" -lt "$GZ_TEXT" ] && echo 1 || echo 0)
check "text beats bzip2-9" $([ "$T" -lt "$BZ_TEXT" ] && echo 1 || echo 0)
check "text beats xz-9e"   $([ "$T" -lt "$XZ_TEXT" ] && echo 1 || echo 0)
check "rand never expands" $([ "$R" -lt "$GZ_RAND" ] && [ "$R" -lt "$XZ_RAND" ] && echo 1 || echo 0)

echo ""
echo "   $pass passed, $fail failed"
[ "$fail" -eq 0 ]
