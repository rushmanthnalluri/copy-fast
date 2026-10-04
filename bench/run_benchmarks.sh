#!/usr/bin/env bash
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

echo "=== Building CopyFast & Benchmark suite ==="
make -C "$PROJECT_ROOT" -j$(nproc) all

echo ""
echo "=== Running CopyFast Micro-Benchmarks ==="
"$PROJECT_ROOT/benchmark" 100M

echo ""
echo "=== Running Multi-file Parallel Work-Queue Benchmark (1,000 files) ==="
SRC_TREE="/tmp/copyfast_bench_tree_src"
DEST_TREE="/tmp/copyfast_bench_tree_dest"

rm -rf "$SRC_TREE" "$DEST_TREE"
mkdir -p "$SRC_TREE"

echo "Creating 1,000 synthetic files in $SRC_TREE..."
for i in $(seq 1 1000); do
    head -c 32768 </dev/urandom > "$SRC_TREE/file_$i.bin"
done

echo "Benchmarking single-thread vs 4-worker parallel copyfast..."

# 1. Single worker
START_1=$(date +%s%N)
"$PROJECT_ROOT/copyfast" -Q -R -j 1 "$SRC_TREE" "$DEST_TREE"
END_1=$(date +%s%N)
TIME_1=$(echo "scale=4; ($END_1 - $START_1)/1000000000" | bc)
rm -rf "$DEST_TREE"

# 2. 4 parallel workers
START_4=$(date +%s%N)
"$PROJECT_ROOT/copyfast" -Q -R -j 4 "$SRC_TREE" "$DEST_TREE"
END_4=$(date +%s%N)
TIME_4=$(echo "scale=4; ($END_4 - $START_4)/1000000000" | bc)
rm -rf "$DEST_TREE"

# 3. 8 parallel workers
START_8=$(date +%s%N)
"$PROJECT_ROOT/copyfast" -Q -R -j 8 "$SRC_TREE" "$DEST_TREE"
END_8=$(date +%s%N)
TIME_8=$(echo "scale=4; ($END_8 - $START_8)/1000000000" | bc)
rm -rf "$DEST_TREE"

echo "---------------------------------------------------------"
echo " Workers | Elapsed Time (s) | Relative Speedup"
echo "---------+------------------+------------------"
echo "    1    |    ${TIME_1} s     |      1.00x (Baseline)"
if command -v bc >/dev/null 2>&1; then
    SPEEDUP_4=$(echo "scale=2; $TIME_1 / $TIME_4" | bc)
    SPEEDUP_8=$(echo "scale=2; $TIME_1 / $TIME_8" | bc)
    echo "    4    |    ${TIME_4} s     |      ${SPEEDUP_4}x"
    echo "    8    |    ${TIME_8} s     |      ${SPEEDUP_8}x"
fi
echo "---------------------------------------------------------"

rm -rf "$SRC_TREE"
echo "Multi-file benchmark complete!"
