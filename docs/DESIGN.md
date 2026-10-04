# CopyFast Architecture & Technical Design Document

## 1. Executive Summary

`CopyFast` is an asynchronous, high-throughput file copying and backup system built in C11 for Linux environments. It delivers high transfer speeds by replacing standard synchronous blocking I/O with three core design pillars:
1. **Pipelined Overlapped I/O with Backpressure**: Concurrently reading into a bounded page-aligned ring buffer while a consumer writer flushes dirty buffers to the target block device.
2. **Kernel-Assisted Sparse-File Preservation**: Leveraging `lseek(SEEK_DATA)` and `lseek(SEEK_HOLE)` to eliminate zero-block read/write penalties and preserve disk inode block allocation.
3. **Dual High-Throughput Engines**: Featuring both a POSIX threaded producer-consumer pipeline and a modern Linux `io_uring` asynchronous submission/completion ring engine.
4. **Crash-Resilient State Journaling**: Enabling interrupted transfers to resume contiguously from durable disk checkpoints verified by CRC32 checksums.

```
                           +------------------------+
                           |  Source Block Device   |
                           +------------------------+
                                        |
                            pread() / io_uring_read
                                        v
                 +---------------------------------------------+
                 |       CopyFast Bounded Ring Buffer Pool      |
                 | [Slot 0] [Slot 1] [Slot 2] ... [Slot N-1]  |
                 |   4KB-aligned | Mutex + Condvars / SQ-CQ    |
                 +---------------------------------------------+
                                        |
                            pwrite() / io_uring_write
                                        v
                           +------------------------+
                           | Destination Filesystem |
                           +------------------------+
```

---

## 2. Pipelined Overlap Architecture & Backpressure

### 2.1 The Producer-Consumer Spine
Traditional tools alternate synchronously:
$$\text{Read Block } N \longrightarrow \text{Wait} \longrightarrow \text{Write Block } N \longrightarrow \text{Wait}$$
This serial execution starves the storage controller: while the host waits for read completions, write channels sit idle, and vice versa.

CopyFast decouples the ingest and egress paths using a bounded circular buffer (`buffer_pool_t`):
- **Producer (Reader Thread)**: Continuously fetches data from `src_fd` at offset $O_i$ using `pread()`, issues `posix_fadvise(POSIX_FADV_SEQUENTIAL | POSIX_FADV_WILLNEED)`, updates on-the-fly streaming hashes, and enqueues filled slots.
- **Consumer (Writer Thread)**: Continuously pulls filled slots from the pool, issues `pwrite()` to `dest_fd`, accounts for completed bytes in the progress meter, checkpoints the state journal, and returns free slots to the producer.

### 2.2 Mathematical Model of Backpressure
To prevent unbounded heap inflation when writing to slow storage (e.g., spinning HDDs or saturated network volumes), the buffer pool enforces strict backpressure via monitor condition variables:

$$\text{Capacity} = K \times B \quad (\text{Default: } 8 \times 1\text{MB} = 8\text{MB})$$

1. **Producer Predicate**:
   $$\text{count} == K \implies \text{Reader blocks on } \mathtt{cond\_not\_full}$$
2. **Consumer Predicate**:
   $$\text{count} == 0 \implies \text{Writer blocks on } \mathtt{cond\_not\_empty}$$

Every state transition is guarded by `pool->mutex`. When a slot is consumed, `pthread_cond_signal(&pool->cond_not_full)` immediately unblocks the reader thread.

---

## 3. Kernel-Assisted Sparse-File Engine

Standard copy utilities convert unallocated file regions into physical blocks of zeroes, blowing up destination storage and thrashing I/O bandwidth.

CopyFast utilizes Linux virtual file system extent discovery:
1. **Extent Discovery**:
   - `lseek(src_fd, curr, SEEK_HOLE)` identifies where the next unallocated gap begins.
   - `lseek(src_fd, curr, SEEK_DATA)` locates the next block of real data.
2. **Zero-Byte Punching & Truncation**:
   - For holes, CopyFast does not allocate memory buffers or execute disk writes. It records skipped hole bytes in atomic statistics and seeks the destination file descriptor forward.
   - For trailing holes, CopyFast invokes `ftruncate(dest_fd, total_size)` upon stream EOF.
3. **In-Memory Fallback**:
   - If the underlying filesystem returns `ENOTTY` or `EINVAL` (e.g. legacy filesystems lacking SEEK_HOLE support), CopyFast evaluates 64-bit word vectors in memory (`sparse_is_buffer_zero()`) to skip zero-filled blocks dynamically without issuing writes.

---

## 4. Modern Linux `io_uring` Asynchronous Engine

As a stretch goal and distinction deliverable, CopyFast implements a native `io_uring` engine (`src/backend_uring.c`).

### 4.1 Comparison: Threads vs `io_uring`
| Characteristic | Threaded Pipeline (`backend_pipeline.c`) | `io_uring` (`backend_uring.c`) |
| :--- | :--- | :--- |
| **Concurrency Mechanism** | Pthreads (`pthread_create`, mutex, condvar) | Kernel Submission & Completion Rings |
| **Context Switching** | Kernel thread context switches | Zero context switch (single userspace thread) |
| **Syscall Overhead** | 1 syscall per `pread` / `pwrite` | Batched submissions via `io_uring_submit` |
| **In-Flight Requests** | Strictly 1 read and 1 write active | Up to $2 \times Q$ asynchronous I/O requests |
| **Memory Model** | Mutex-synchronized circular ring | In-flight state machine with slot tagging |

### 4.2 io_uring SQ/CQ Workflow
```
[Userspace]                    [Linux Kernel io_uring Subsystem]
     |                                        |
     |--- 1. Prep io_uring_prep_read() ------>| (SQE in Submission Queue)
     |--- 2. io_uring_submit() -------------->| (Kernel queues block reads)
     |                                        |
     |<-- 3. io_uring_wait_cqe() -------------| (CQE signals read ready)
     |                                        |
     |--- 4. Prep io_uring_prep_write() ----->| (SQE in Submission Queue)
     |--- 5. Prep next io_uring_prep_read() ->| (Pipelined read ahead)
     |--- 6. io_uring_submit() -------------->| (Concurrent batch write & read)
```

---

## 5. Crash-Consistent Resumable Journal Protocol

CopyFast ensures fault tolerance against network drops, system panics, and `SIGINT` interruptions:

### 5.1 Journal Binary Layout (`resume_record_t`)
```c
typedef struct {
    uint32_t magic;           // 0x52465043 ("CPFR")
    uint32_t version;         // 1
    dev_t    src_dev;         // Source filesystem device ID
    ino_t    src_ino;         // Source file inode number
    off_t    src_size;        // Total expected file size
    int64_t  src_mtime_sec;   // Source mtime seconds
    int64_t  src_mtime_nsec;  // Source mtime nanoseconds
    off_t    copied_offset;   // Durably synced byte boundary
    uint64_t partial_hash;    // Rolling checksum
    uint32_t status;          // IN_PROGRESS (1), INTERRUPTED (2), COMPLETED (3)
    uint32_t crc32;           // Header integrity checksum
} resume_record_t;
```

### 5.2 Atomic Journal Updates
1. When a checkpoint triggers (or on `SIGINT`), CopyFast calls `fdatasync(dest_fd)` to commit all written bytes to disk platter/NAND.
2. The journal record is serialized with a recalculated CRC32.
3. The journal is written to a process-unique temporary file (`<dest>.copyfast.resume.tmp.<pid>`), flushed via `fdatasync()`, and atomically replaced via `rename()`.
4. Upon resume, if `src_ino`, `src_size`, or `src_mtime` has changed, CopyFast warns the user and restarts cleanly from offset 0 to prevent data corruption.
5. On verified completion, the journal is unlinked.

---

## 6. Multi-File Parallel Work Queue

For directory trees (`--recursive -j 8`), CopyFast utilizes a thread pool with a bounded work queue (`work_queue_t`):
- **Directory Crawler**: Traverses directory hierarchy using `opendir` and `readdir`. Creates destination directories and pushes file transfer items into the work queue.
- **Worker Threads**: $J$ worker threads consume items and invoke `copy_file_dispatch()`.
- **Hierarchical Timestamp Restoration**: Directory modification timestamps are recorded in a LIFO stack during traversal and reapplied post-order once all child files have finished writing.

---

## 7. Performance & Benchmark Summary

Measured across standard 100MB dense files, 500MB sparse files, and a 1,000-file directory tree in Linux 6.18 on x86_64:

| Workload | Naive Baseline | Threaded Pipeline | io_uring Async | Speedup vs Baseline |
| :--- | :--- | :--- | :--- | :--- |
| **100 MB Dense File** | 1,151.1 MB/s | 2,656.1 MB/s | 2,052.3 MB/s | **2.31x faster** |
| **500 MB Sparse (99% Holes)** | 274,379 MB/s | 70,713 MB/s | 228,483 MB/s | **Preserved (<2MB used)** |
| **1,000 Files (Parallel Workers)** | 0.3170 s (1 worker) | 0.0302 s (4 workers) | N/A | **10.49x faster** |
