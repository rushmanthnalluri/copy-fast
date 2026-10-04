#include "copyfast.h"
#include "backends.h"
#include "checksum.h"
#include <sys/resource.h>

typedef struct {
    const char *name;
    copy_backend_t backend;
    double elapsed_sec;
    double user_cpu_sec;
    double sys_cpu_sec;
    double throughput_mbps;
} bench_result_t;

static const char *g_bench_files[] = {
    "/tmp/copyfast_bench_src.dat",
    "/tmp/copyfast_bench_dest.dat",
    "/tmp/copyfast_bench_sparse.dat",
    "/tmp/copyfast_bench_sparse_dest.dat"
};

static void cleanup_bench_files(void) {
    for (size_t i = 0; i < 4; ++i) {
        unlink(g_bench_files[i]);
    }
}

static void bench_sig_handler(int sig) {
    (void)sig;
    cleanup_bench_files();
    _exit(130);
}

static void create_test_file(const char *path, size_t size, bool sparse) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0) return;

    if (sparse) {
        /* Write 1MB data at 0, 1MB at 250MB, 1MB at size - 1MB */
        char *buf = (char *)malloc(1024 * 1024);
        if (buf) {
            memset(buf, 0xAA, 1024 * 1024);
            ssize_t w = pwrite(fd, buf, 1024 * 1024, 0);
            (void)w;
            if (size > 20 * 1024 * 1024) {
                w = pwrite(fd, buf, 1024 * 1024, size / 2);
                (void)w;
            }
            if (size > 2 * 1024 * 1024) {
                w = pwrite(fd, buf, 1024 * 1024, size - (1024 * 1024));
                (void)w;
            }
            free(buf);
        }
        if (ftruncate(fd, size) != 0) {
            /* Handled */
        }
    } else {
        const size_t chunk = 1024 * 1024;
        char *buf = (char *)malloc(chunk);
        if (buf) {
            for (size_t i = 0; i < chunk; ++i) buf[i] = (char)(i & 0xFF);
            size_t written = 0;
            while (written < size) {
                size_t to_write = (size - written < chunk) ? (size - written) : chunk;
                ssize_t w = pwrite(fd, buf, to_write, written);
                if (w <= 0) break;
                written += (size_t)w;
            }
            free(buf);
        }
    }
    fsync(fd);
    close(fd);
}

static void run_single_benchmark(const char *src_path, const char *dest_path,
                                copy_backend_t backend, const char *backend_name,
                                size_t file_size, bench_result_t *res) {
    unlink(dest_path);

    copy_options_t opts = {
        .backend = backend,
        .buffer_size = 1024 * 1024, /* 1 MB */
        .queue_depth = 8,
        .jobs = 4,
        .verify_checksum = false,
        .checksum_type = CHECKSUM_NONE,
        .preserve_metadata = false,
        .sparse_aware = true,
        .resume = false,
        .quiet = true,
        .verbose = false
    };

    /* Reset global stats */
    atomic_store(&g_stats.total_bytes_written, 0);
    atomic_store(&g_stats.total_bytes_read, 0);
    atomic_store(&g_stats.total_sparse_bytes_skipped, 0);

    struct rusage ru_start, ru_end;
    struct timespec ts_start, ts_end;

    getrusage(RUSAGE_SELF, &ru_start);
    clock_gettime(CLOCK_MONOTONIC, &ts_start);

    copy_file_dispatch(src_path, dest_path, &opts, NULL);

    clock_gettime(CLOCK_MONOTONIC, &ts_end);
    getrusage(RUSAGE_SELF, &ru_end);

    double elapsed = copyfast_get_elapsed_sec(&ts_start, &ts_end);
    double ucpu = (ru_end.ru_utime.tv_sec - ru_start.ru_utime.tv_sec) +
                  (ru_end.ru_utime.tv_usec - ru_start.ru_utime.tv_usec) / 1e6;
    double scpu = (ru_end.ru_stime.tv_sec - ru_start.ru_stime.tv_sec) +
                  (ru_end.ru_stime.tv_usec - ru_start.ru_stime.tv_usec) / 1e6;

    res->name = backend_name;
    res->backend = backend;
    res->elapsed_sec = elapsed;
    res->user_cpu_sec = ucpu;
    res->sys_cpu_sec = scpu;
    res->throughput_mbps = (elapsed > 0.0001) ? ((double)file_size / (1024.0 * 1024.0)) / elapsed : 0.0;

    unlink(dest_path);
}

int main(int argc, char **argv) {
    atexit(cleanup_bench_files);
    signal(SIGINT, bench_sig_handler);
    signal(SIGTERM, bench_sig_handler);

    size_t test_size = 100 * 1024 * 1024; /* 100 MB default */
    if (argc > 1) {
        copyfast_parse_size(argv[1], &test_size);
    }

    const char *src_file = "/tmp/copyfast_bench_src.dat";
    const char *dest_file = "/tmp/copyfast_bench_dest.dat";
    const char *sparse_src = "/tmp/copyfast_bench_sparse.dat";
    const char *sparse_dest = "/tmp/copyfast_bench_sparse_dest.dat";

    printf("\n========================================================================================\n");
    printf("   CopyFast Micro-Benchmark Suite: Naive vs Pipeline vs io_uring\n");
    printf("========================================================================================\n");
    printf(" Generating %.1f MB test payload in /tmp...\n", (double)test_size / (1024.0 * 1024.0));
    create_test_file(src_file, test_size, false);

    bench_result_t results[3];
    run_single_benchmark(src_file, dest_file, BACKEND_NAIVE, "Naive (Sequential)", test_size, &results[0]);
    run_single_benchmark(src_file, dest_file, BACKEND_PIPELINE, "Pipeline (Threaded)", test_size, &results[1]);
    run_single_benchmark(src_file, dest_file, BACKEND_URING, "io_uring (Async SQ/CQ)", test_size, &results[2]);

    printf("\n--- Test Payload: %.1f MB Dense File ---\n", (double)test_size / (1024.0 * 1024.0));
    printf("%-24s | %10s | %14s | %10s | %10s | %8s\n",
           "Backend", "Time (s)", "Throughput", "User CPU", "Sys CPU", "Speedup");
    printf("-------------------------+------------+----------------+------------+------------+---------\n");

    double baseline_time = results[0].elapsed_sec;
    for (int i = 0; i < 3; ++i) {
        double speedup = (results[i].elapsed_sec > 0.0001) ? baseline_time / results[i].elapsed_sec : 1.0;
        printf("%-24s | %9.4f s | %10.1f MB/s | %9.3f s | %9.3f s | %6.2fx\n",
               results[i].name, results[i].elapsed_sec, results[i].throughput_mbps,
               results[i].user_cpu_sec, results[i].sys_cpu_sec, speedup);
    }

    /* Sparse file benchmark */
    size_t sparse_size = 500 * 1024 * 1024; /* 500 MB sparse with 99% holes */
    printf("\n--- Test Payload: %.1f MB Sparse File (99%% Hole Ratio) ---\n", (double)sparse_size / (1024.0 * 1024.0));
    create_test_file(sparse_src, sparse_size, true);

    bench_result_t sp_res[3];
    run_single_benchmark(sparse_src, sparse_dest, BACKEND_NAIVE, "Naive (Sparse)", sparse_size, &sp_res[0]);
    run_single_benchmark(sparse_src, sparse_dest, BACKEND_PIPELINE, "Pipeline (Sparse)", sparse_size, &sp_res[1]);
    run_single_benchmark(sparse_src, sparse_dest, BACKEND_URING, "io_uring (Sparse)", sparse_size, &sp_res[2]);

    printf("%-24s | %10s | %14s | %10s | %10s | %8s\n",
           "Backend", "Time (s)", "Throughput", "User CPU", "Sys CPU", "Speedup");
    printf("-------------------------+------------+----------------+------------+------------+---------\n");
    for (int i = 0; i < 3; ++i) {
        double speedup = (sp_res[i].elapsed_sec > 0.0001) ? sp_res[0].elapsed_sec / sp_res[i].elapsed_sec : 1.0;
        printf("%-24s | %9.4f s | %10.1f MB/s | %9.3f s | %9.3f s | %6.2fx\n",
               sp_res[i].name, sp_res[i].elapsed_sec, sp_res[i].throughput_mbps,
               sp_res[i].user_cpu_sec, sp_res[i].sys_cpu_sec, speedup);
    }
    printf("========================================================================================\n\n");

    unlink(src_file);
    unlink(dest_file);
    unlink(sparse_src);
    unlink(sparse_dest);

    return 0;
}
