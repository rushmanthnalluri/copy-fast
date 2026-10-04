# CopyFast — Live Viva & Defense Runbook

This document is your tactical preparation guide and live demonstration script for the **Week-12 OS Project Viva / Presentation**.

---

## 1. Quick Live Demo Script (Step-by-Step)

Have these commands ready in your terminal. They demonstrate every single acceptance test live in under 3 minutes.

### Demo 1: Pipelined Overlap Benchmark vs Naive (Speedup Proof)
Run the internal micro-benchmark suite:
```bash
./benchmark 100M
```
- **What to show the examiner**:
  Point to the summary table. Show that **Pipeline (Threaded)** achieves **~2,600 MB/s** compared to **~1,150 MB/s** for Naive (**2.31x speedup**), and **io_uring** achieves **~2,050 MB/s**.
- **What to explain**:
  *"Naive copy alternates between blocking reads and blocking writes, wasting one device while waiting for the other. CopyFast uses an overlapped ring buffer with reader and writer threads running concurrently, saturating the bus."*

---

### Demo 2: Sparse File Preservation (Holes Not Expanded)
Create a 100 MB sparse file with a 98 MB hole:
```bash
# 1. Create 100MB sparse file with 1MB at start and 1MB at 99MB
dd if=/dev/urandom of=/tmp/sparse_orig.bin bs=1M count=1 seek=0 2>/dev/null
dd if=/dev/urandom of=/tmp/sparse_orig.bin bs=1M count=1 seek=99 2>/dev/null

# 2. Copy using CopyFast sparse engine
./copyfast -S -v /tmp/sparse_orig.bin /tmp/sparse_copy.bin

# 3. Inspect apparent size vs allocated disk blocks
ls -lh -s /tmp/sparse_orig.bin /tmp/sparse_copy.bin
```
- **What to show the examiner**:
  Show that `ls -s` reports both files only take **~2 MB of physical disk blocks** despite having an apparent size of **100 MB**.
- **What to explain**:
  *"Standard copy utilities read zeroes and write physical blocks, inflating the destination to 100 MB. CopyFast uses `lseek(SEEK_HOLE)` and `lseek(SEEK_DATA)` to detect holes and advances destination offsets without issuing writes, preserving disk space and avoiding I/O thrashing."*

---

### Demo 3: Live Interruption (SIGINT) & Resumable Transfer
Start a copy of a large file, interrupt it with `Ctrl+C`, inspect the journal, and resume:
```bash
# 1. Generate 200MB test file
head -c 209715200 </dev/urandom > /tmp/large_file.bin

# 2. Launch CopyFast with resume enabled, and press Ctrl+C midway
./copyfast -r -s 64K /tmp/large_file.bin /tmp/large_copy.bin
# Press Ctrl+C after ~1-2 seconds!

# 3. Show the examiner the durable state journal
ls -l /tmp/large_copy.bin.copyfast.resume

# 4. Resume the copy with verification!
./copyfast -r -v /tmp/large_file.bin /tmp/large_copy.bin
```
- **What to show the examiner**:
  Point out:
  1. `[Interrupted] State journal saved. Re-run with --resume to continue.`
  2. The second invocation prints: `[Resume] Resuming '/tmp/large_file.bin' at offset X / 209715200`.
  3. Transfer finishes rapidly from where it stopped and reports `Verification: PASS`.
  4. The journal file is automatically cleaned up on success.
- **What to explain**:
  *"CopyFast uses an atomic state journal (`resume_record_t`) with CRC32 header validation. On SIGINT, signals are masked on worker threads, dirty buffers are flushed with `fdatasync()`, and the progress checkpoint is committed atomically."*

---

### Demo 4: Parallel Multi-File Work Queue (Directory Recursion)
Run the 1,000-file parallel speedup benchmark:
```bash
./bench/run_benchmarks.sh
```
- **What to show the examiner**:
  Show that 4 worker threads copy 1,000 files in **~0.03 seconds** compared to **~0.31 seconds** for 1 worker (**10.49x speedup**).
- **What to explain**:
  *"For directories, CopyFast traverses the directory hierarchy and pushes file jobs into a bounded `work_queue_t`. Worker threads pull items concurrently with backpressure, and directory timestamps are restored post-order."*

---

### Demo 5: Valgrind Memory Discipline (Zero Leaks)
```bash
make valgrind
```
- **What to show the examiner**:
  ```
  All heap blocks were freed -- no leaks are possible
  ERROR SUMMARY: 0 errors from 0 contexts
  ```
- **What to explain**:
  *"Week 8 focus: strict memory and file descriptor discipline. All ring buffers, mutexes, condition variables, and descriptors are freed and closed on all exit paths."*

---

## 2. Top 10 Viva Questions & Model Answers

### Q1: Why does pipelined double-buffering outperform standard blocking read/write?
**Answer**:
> *"Standard `read()` then `write()` executes in serial lockstep: while reading, the write channel and destination storage controller sit completely idle; while writing, the read channel sits idle. CopyFast decouples them into producer and consumer threads sharing a bounded circular buffer pool. While the writer thread flushes block $N$ via `pwrite()`, the reader thread concurrently prefetches block $N+1$ via `pread()`. This keeps both storage interfaces continuously saturated, yielding up to a 2.3x speedup."*

### Q2: What is backpressure and how is it implemented in CopyFast?
**Answer**:
> *"Backpressure prevents the fast producer (reader) from overwhelming the slower consumer (writer) and consuming unlimited RAM. In `buffer_pool.c`, the ring buffer is bounded to $K$ slots (e.g. 8 x 1MB). When all slots are full (`count == queue_depth`), the reader blocks on `pthread_cond_timedwait(&pool->cond_not_full)`. As soon as the writer drains a slot, it signals `cond_not_full`, unblocking the reader. Conversely, when the pool is empty, the writer blocks on `cond_not_empty`."*

### Q3: How does `lseek(SEEK_HOLE)` and `lseek(SEEK_DATA)` work under Linux?
**Answer**:
> *"Starting in Linux 3.1, virtual filesystems (ext4, XFS, Btrfs) expose file allocation extent maps via `lseek(fd, offset, SEEK_HOLE)` and `lseek(fd, offset, SEEK_DATA)`. `SEEK_HOLE` returns the offset of the next unallocated block of zeroes, while `SEEK_DATA` returns the start of the next allocated extent. CopyFast reads only data extents and creates holes in the target by seeking forward or truncating (`ftruncate`), which prevents the filesystem from allocating physical disk blocks for zero regions."*

### Q4: Why did you use `pthread_cond_timedwait` instead of unconditional `pthread_cond_wait`?
**Answer**:
> *"In POSIX, `pthread_cond_wait` does not reliably unblock on signals like `SIGINT`. If a writer thread is suspended or terminates while the reader is waiting on `cond_not_full`, an unconditional wait can deadlock the process during an interruption. By using `pthread_cond_timedwait` with a 100ms periodic timeout, threads awaken periodically, evaluate `g_stop_requested`, flush dirty buffers, and terminate cleanly without hanging."*

### Q5: What is the architectural difference between your Threaded Pipeline and `io_uring`?
**Answer**:
> *"The threaded pipeline uses kernel threads (`pthreads`) synchronized via mutexes and condition variables. Each read and write involves a syscall and thread scheduling. Linux `io_uring` is a true kernel asynchronous interface using shared memory lockless circular rings: a Submission Queue (SQ) and a Completion Queue (CQ). Userspace writes multiple read/write requests (SQEs) and submits them in a single batch with `io_uring_submit()`. Completions (CQEs) are reaped without thread context switches."*

### Q6: How is crash consistency guaranteed in the `.copyfast.resume` journal?
**Answer**:
> *"The journal record (`resume_record_t`) contains source device, inode, total size, mtime, byte offset, and a CRC32 checksum. To guarantee atomicity, CopyFast first flushes destination data via `fdatasync()`, writes the journal to a temporary file (`.resume.tmp.<pid>`), calls `fdatasync()` on the journal file, and then atomically renames it over the target journal using the POSIX `rename()` syscall. This guarantees that an abrupt power loss can never leave a partially written, corrupted journal."*

### Q7: How does CopyFast preserve nanosecond timestamps without clock drift?
**Answer**:
> *"Standard `utime()` only supports 1-second resolution. CopyFast uses modern Linux `futimens()` on open file descriptors and `utimensat(AT_FDCWD, path, times, AT_SYMLINK_NOFOLLOW)` on paths. It passes `struct timespec times[2]` containing `st.st_atim` and `st.st_mtim` directly from `fstat()`, preserving exact sub-second / nanosecond file timestamps."*

### Q8: How did you solve the on-the-fly checksum problem for sparse files?
**Answer**:
> *"Standard checksum tools (`sha256sum`) treat holes as virtual zeroes (`0x00`). When CopyFast skips holes to avoid disk writes, hashing only the data extents would produce a mismatched hash. We engineered `checksum_update_zeros()`, which feeds zero bytes from an in-memory buffer into the streaming SHA-256 state without any disk I/O. As a result, CopyFast computes the exact byte-for-byte SHA-256 of the sparse file on the fly while retaining 100% disk sparsity."*

### Q9: Why is `posix_memalign` used for buffer allocation?
**Answer**:
> *"Memory allocated via standard `malloc` is only 8- or 16-byte aligned. `posix_memalign(&buf, 4096, buffer_size)` aligns buffer allocations to 4KB page boundaries. This matches the CPU L1/L2 cache line and disk sector boundaries, optimizes DMA page tables, eliminates cache line boundary crossing penalties, and allows optional `O_DIRECT` direct I/O without page copy overhead."*

### Q10: How does the recursive directory copier avoid corrupted directory timestamps?
**Answer**:
> *"When a file is created inside a directory, the kernel automatically updates the parent directory's `mtime`. If CopyFast set directory timestamps during pre-order traversal, creating child files would immediately overwrite the timestamp. CopyFast records directory timestamps in a stack during traversal and restores them in **post-order** after all worker threads have completed copying all child files."*
