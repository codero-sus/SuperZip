#!/bin/sh
# G0D SZIP — universe benchmark (written check).
# Ensures the benchmark venv (zstd/brotli/lz4/snappy bindings) exists, then
# runs tests/benchall.py against every compressor available on this machine.
set -e
cd "$(dirname "$0")/.."
mkdir -p build
if [ ! -x build/venv/bin/python ]; then
  python3 -m venv build/venv
  build/venv/bin/pip install --quiet cramjam zstandard brotli lz4 || true
fi
exec build/venv/bin/python tests/benchall.py "$@"
