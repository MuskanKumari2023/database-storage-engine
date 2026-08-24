#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

echo "Building benchmark..."
g++ -std=c++17 -O2 -Wall -Wextra -I. \
  memtable/memtable.cpp \
  wal/wal.cpp \
  sstable/bloom_filter.cpp \
  sstable/sparse_index.cpp \
  sstable/sstable.cpp \
  engine/engine.cpp \
  benchmarks/benchmark.cpp \
  -o benchmarks/benchmark

echo "Running benchmark..."
./benchmarks/benchmark

if command -v python3 >/dev/null 2>&1; then
  PYTHON="python3"
  if [[ -x "$ROOT/benchmarks/.venv/bin/python" ]]; then
    PYTHON="$ROOT/benchmarks/.venv/bin/python"
  fi
  if "$PYTHON" -c "import matplotlib" 2>/dev/null; then
    echo "Plotting..."
    "$PYTHON" benchmarks/plot_benchmarks.py
  else
    echo "matplotlib not installed — skip plotting."
    echo "Setup: python3 -m venv benchmarks/.venv && benchmarks/.venv/bin/pip install matplotlib"
  fi
fi
