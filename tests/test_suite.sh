#!/usr/bin/env bash
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
COPYFAST="$PROJECT_ROOT/copyfast"

TEST_DIR="/tmp/copyfast_test_$(date +%s)"
mkdir -p "$TEST_DIR"
trap 'rm -rf "$TEST_DIR"' EXIT

RED='\033[0;31m'
GREEN='\033[0;32m'
BLUE='\033[0;34m'
YELLOW='\033[1;33m'
NC='\033[0m'

pass_count=0
fail_count=0

run_test() {
    local name="$1"
    shift
    echo -e "${BLUE}[TEST]${NC} Running: $name..."
    if "$@"; then
        echo -e "${GREEN}[PASS]${NC} $name"
        pass_count=$((pass_count + 1))
    else
        echo -e "${RED}[FAIL]${NC} $name"
        fail_count=$((fail_count + 1))
    fi
}

# Ensure binary is built
make -C "$PROJECT_ROOT" copyfast

# =========================================================================
# TEST 1: Byte-identical Copy & Verification Across Backends
# =========================================================================
test_backends_integrity() {
    local src="$TEST_DIR/payload.bin"
    head -c 10485760 </dev/urandom > "$src" # 10 MB payload
    local src_hash=$(sha256sum "$src" | awk '{print $1}')

    for backend in naive pipeline uring; do
        local dest="$TEST_DIR/dest_${backend}.bin"
        "$COPYFAST" -Q -b "$backend" -v "$src" "$dest"
        local dest_hash=$(sha256sum "$dest" | awk '{print $1}')

        if [ "$src_hash" != "$dest_hash" ]; then
            echo "Hash mismatch on backend $backend: $src_hash vs $dest_hash"
            return 1
        fi
    done
    return 0
}

# =========================================================================
# TEST 2: Metadata Preservation (Mode Permissions and Timestamps)
# =========================================================================
test_metadata_preservation() {
    local src="$TEST_DIR/meta_src.txt"
    local dest="$TEST_DIR/meta_dest.txt"
    echo "Metadata preservation test" > "$src"

    chmod 0751 "$src"
    touch -d "2024-01-15 12:34:56" "$src"

    "$COPYFAST" -Q -p "$src" "$dest"

    local src_mode=$(stat -c "%a" "$src")
    local dest_mode=$(stat -c "%a" "$dest")
    if [ "$src_mode" != "$dest_mode" ]; then
        echo "Mode mismatch: src=$src_mode dest=$dest_mode"
        return 1
    fi

    local src_mtime=$(stat -c "%Y" "$src")
    local dest_mtime=$(stat -c "%Y" "$dest")
    if [ "$src_mtime" != "$dest_mtime" ]; then
        echo "Timestamp mismatch: src=$src_mtime dest=$dest_mtime"
        return 1
    fi

    return 0
}

# =========================================================================
# TEST 3: Sparse File Hole Preservation (Holes Not Expanded)
# =========================================================================
test_sparse_preservation() {
    local src="$TEST_DIR/sparse_src.img"
    local dest="$TEST_DIR/sparse_dest.img"

    # Create 50MB file with 1MB data at 0 and 1MB at 49MB (48MB hole)
    dd if=/dev/urandom of="$src" bs=1M count=1 seek=0 2>/dev/null
    dd if=/dev/urandom of="$src" bs=1M count=1 seek=49 2>/dev/null

    local src_blocks=$(stat -c "%b" "$src")

    "$COPYFAST" -Q -S "$src" "$dest"

    local dest_blocks=$(stat -c "%b" "$dest")
    local src_hash=$(sha256sum "$src" | awk '{print $1}')
    local dest_hash=$(sha256sum "$dest" | awk '{print $1}')

    if [ "$src_hash" != "$dest_hash" ]; then
        echo "Sparse file content corrupted!"
        return 1
    fi

    # Destination blocks should not be significantly larger than source blocks
    # A non-sparse copy would allocate ~102,400 512-byte blocks (50MB)
    # A sparse copy should only use ~4,000 blocks (2MB)
    if [ "$dest_blocks" -gt $((src_blocks * 2 + 100)) ]; then
        echo "Sparse file was expanded to dense: src_blocks=$src_blocks dest_blocks=$dest_blocks"
        return 1
    fi

    return 0
}

# =========================================================================
# TEST 4: Resumable Copy After Interruption
# =========================================================================
test_resumable_copy() {
    local src="$TEST_DIR/huge_src.bin"
    local dest="$TEST_DIR/huge_dest.bin"

    # Create 100MB file
    head -c 104857600 </dev/urandom > "$src"
    local src_hash=$(sha256sum "$src" | awk '{print $1}')

    # Start copy in background and kill it after 100ms
    "$COPYFAST" -Q -r -s 64K "$src" "$dest" &
    local copy_pid=$!
    sleep 0.1
    kill -INT $copy_pid 2>/dev/null || true
    wait $copy_pid 2>/dev/null || true

    # Resume the copy
    "$COPYFAST" -Q -r -v "$src" "$dest"

    local dest_hash=$(sha256sum "$dest" | awk '{print $1}')
    if [ "$src_hash" != "$dest_hash" ]; then
        echo "Resumed file checksum mismatch!"
        return 1
    fi

    # Ensure journal file was cleanly unlinked
    if [ -f "${dest}.copyfast.resume" ]; then
        echo "Resume journal was not cleaned up after completion!"
        return 1
    fi

    return 0
}

# =========================================================================
# TEST 5: Recursive Directory Tree & Symlink Copy
# =========================================================================
test_recursive_tree() {
    local src_dir="$TEST_DIR/tree_src"
    local dest_dir="$TEST_DIR/tree_dest"

    mkdir -p "$src_dir/subdir1/nested" "$src_dir/subdir2"
    echo "file1" > "$src_dir/file1.txt"
    head -c 500000 </dev/urandom > "$src_dir/subdir1/large.bin"
    echo "nested content" > "$src_dir/subdir1/nested/inner.txt"
    ln -s "file1.txt" "$src_dir/link_to_file1"

    "$COPYFAST" -Q -R -p -j 4 "$src_dir" "$dest_dir"

    # Compare directory trees with diff -r
    diff -r "$src_dir" "$dest_dir"
    return $?
}

# =========================================================================
# TEST 6: Zero-byte and Edge Case Files
# =========================================================================
test_edge_cases() {
    local empty_src="$TEST_DIR/empty.txt"
    local empty_dest="$TEST_DIR/empty_dest.txt"
    touch "$empty_src"

    "$COPYFAST" -Q "$empty_src" "$empty_dest"
    if [ -s "$empty_dest" ]; then
        echo "Empty file copy resulted in non-empty destination!"
        return 1
    fi
    return 0
}

# =========================================================================
# TEST 7: Sparse File with In-Flight Cryptographic Verification (-S -v)
# =========================================================================
test_sparse_with_verification() {
    local src="$TEST_DIR/sparse_ver_src.img"
    local dest="$TEST_DIR/sparse_ver_dest.img"

    # Create 20MB file with 1MB data, 18MB hole, 1MB data
    dd if=/dev/urandom of="$src" bs=1M count=1 seek=0 2>/dev/null
    dd if=/dev/urandom of="$src" bs=1M count=1 seek=19 2>/dev/null

    "$COPYFAST" -Q -S -v "$src" "$dest"

    local src_hash=$(sha256sum "$src" | awk '{print $1}')
    local dest_hash=$(sha256sum "$dest" | awk '{print $1}')

    if [ "$src_hash" != "$dest_hash" ]; then
        echo "Hash mismatch on sparse verification: $src_hash vs $dest_hash"
        return 1
    fi
    return 0
}

# =========================================================================
# TEST 8: Parameter Sweep: Buffer Sizes (64KB, 4MB) & Queue Depths (2, 16)
# =========================================================================
test_param_sweep() {
    local src="$TEST_DIR/param_src.bin"
    head -c 16777216 </dev/urandom > "$src" # 16 MB
    local src_hash=$(sha256sum "$src" | awk '{print $1}')

    # Small buffers, shallow queue
    local dest1="$TEST_DIR/param_dest1.bin"
    "$COPYFAST" -Q -s 64K -q 2 -v "$src" "$dest1"
    local h1=$(sha256sum "$dest1" | awk '{print $1}')
    if [ "$src_hash" != "$h1" ]; then
        echo "Param sweep failed on 64K/q2"
        return 1
    fi

    # Large buffers, deep queue
    local dest2="$TEST_DIR/param_dest2.bin"
    "$COPYFAST" -Q -s 4M -q 16 -v "$src" "$dest2"
    local h2=$(sha256sum "$dest2" | awk '{print $1}')
    if [ "$src_hash" != "$h2" ]; then
        echo "Param sweep failed on 4M/q16"
        return 1
    fi

    return 0
}

# =========================================================================
# TEST 9: Corrupted / Mismatched Resume Journal Recovery
# =========================================================================
test_corrupted_journal_recovery() {
    local src="$TEST_DIR/journal_test_src.bin"
    local dest="$TEST_DIR/journal_test_dest.bin"
    head -c 2097152 </dev/urandom > "$src"

    # Plant a corrupted fake journal
    echo "CORRUPTED_GARBAGE_JOURNAL_DATA" > "${dest}.copyfast.resume"

    # Should detect bad journal, warn, and copy whole file cleanly
    "$COPYFAST" -Q -r -v "$src" "$dest"

    local src_hash=$(sha256sum "$src" | awk '{print $1}')
    local dest_hash=$(sha256sum "$dest" | awk '{print $1}')

    if [ "$src_hash" != "$dest_hash" ]; then
        echo "Failed to recover from corrupted journal!"
        return 1
    fi
    return 0
}

# =========================================================================
# Run All Tests
# =========================================================================
echo "========================================================="
echo "   CopyFast Automated Test Battery & Acceptance Suite"
echo "========================================================="

run_test "Byte-identical verification across backends" test_backends_integrity
run_test "Metadata preservation (mode & utimensat timestamps)" test_metadata_preservation
run_test "Sparse file hole detection and block preservation" test_sparse_preservation
run_test "Sparse file with cryptographic verification (-S -v)" test_sparse_with_verification
run_test "Crash-resilient resumable transfer after SIGINT" test_resumable_copy
run_test "Corrupted resume journal detection and recovery" test_corrupted_journal_recovery
run_test "Buffer size (64K, 4M) and queue depth (2, 16) sweep" test_param_sweep
run_test "Parallel directory tree and symlink recursion" test_recursive_tree
run_test "Zero-byte and boundary condition handling" test_edge_cases

echo "========================================================="
echo -e "Results: ${GREEN}$pass_count passed${NC}, ${RED}$fail_count failed${NC}"
echo "========================================================="

if [ $fail_count -ne 0 ]; then
    exit 1
fi
exit 0
