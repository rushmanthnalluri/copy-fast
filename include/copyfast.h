/**
 * CopyFast — High-Throughput Linux Copy/Backup Engine
 * CoreLinux · C11 · Filesystems · Concurrency · Memory
 */

#ifndef COPYFAST_H
#define COPYFAST_H

#define _GNU_SOURCE 1

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <pthread.h>
#include <stdatomic.h>

#define COPYFAST_VERSION "1.0.0"
#define COPYFAST_DEFAULT_BUFFER_SIZE (1024 * 1024) /* 1 MB */
#define COPYFAST_DEFAULT_QUEUE_DEPTH 8
#define COPYFAST_DEFAULT_JOBS        4
#define COPYFAST_RESUME_SUFFIX       ".copyfast.resume"

/* Execution Backends */
typedef enum {
    BACKEND_PIPELINE = 0, /* Threaded reader -> ring buffer -> writer */
    BACKEND_NAIVE    = 1, /* Baseline sequential read -> write */
    BACKEND_URING    = 2  /* Linux io_uring asynchronous backend */
} copy_backend_t;

/* Checksum Types */
typedef enum {
    CHECKSUM_NONE   = 0,
    CHECKSUM_XXH64  = 1,
    CHECKSUM_SHA256 = 2
} checksum_type_t;

/* Configuration options parsed from CLI */
typedef struct {
    copy_backend_t  backend;
    size_t          buffer_size;
    size_t          queue_depth;
    int             jobs;
    bool            verify_checksum;
    checksum_type_t checksum_type;
    bool            preserve_metadata;
    bool            sparse_aware;
    bool            resume;
    bool            direct_io;
    bool            recursive;
    bool            quiet;
    bool            verbose;
    bool            force_overwrite;
    const char     *custom_resume_path;
} copy_options_t;

/* Global atomic runtime statistics */
typedef struct {
    atomic_uint_least64_t total_bytes_read;
    atomic_uint_least64_t total_bytes_written;
    atomic_uint_least64_t total_sparse_bytes_skipped;
    atomic_uint_least64_t total_holes_detected;
    atomic_uint_least64_t total_files_copied;
    atomic_uint_least64_t total_files_resumed;
    atomic_uint_least64_t total_files_failed;
    atomic_uint_least64_t total_bytes_expected;
    struct timespec       start_time;
    struct timespec       end_time;
} copy_stats_t;

extern volatile sig_atomic_t g_stop_requested;
extern copy_stats_t          g_stats;

/* Time helper functions */
double copyfast_get_elapsed_sec(const struct timespec *start, const struct timespec *end);
void   copyfast_get_time(struct timespec *ts);

/* Size parsing helper (e.g., 64K, 1M, 16M) */
int copyfast_parse_size(const char *str, size_t *out_size);

#endif /* COPYFAST_H */
