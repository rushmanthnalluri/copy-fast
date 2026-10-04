#include "copyfast.h"
#include "backends.h"
#include "work_queue.h"
#include "progress.h"
#include "signals.h"
#include <getopt.h>

static void print_banner(void) {
    fprintf(stderr,
        "  ____                   _____         _   \n"
        " / ___|___  _ __  _   _ |  ___|_ _ ___| |_ \n"
        "| |   / _ \\| '_ \\| | | || |_ / _` / __| __|\n"
        "| |__| (_) | |_) | |_| ||  _| (_| \\__ \\ |_ \n"
        " \\____\\___/| .__/ \\__, ||_|  \\__,_|___/\\__|\n"
        "           |_|    |___/                    \n"
        " CopyFast v" COPYFAST_VERSION " — High-Throughput Linux Async Copy Engine\n\n");
}

static void print_syllabus(void) {
    printf("===============================================================================\n");
    printf(" CopyFast OS Syllabus Synchronization Matrix (Weeks 1 - 12)\n");
    printf("===============================================================================\n");
    printf(" Wk | Focus                     | CopyFast Subsystem & Implementation\n");
    printf("----+---------------------------+----------------------------------------------\n");
    printf(" W1 | Syscall Boundary & REPL   | src/backend_naive.c (read/write loop, progress)\n");
    printf(" W2 | Memory Model & Linker     | src/buffer_pool.c (posix_memalign, ownership)\n");
    printf(" W3 | Lexing & Argument Parsing | src/main.c (getopt_long, unit conversion)\n");
    printf(" W4 | Processes & Scheduling    | src/work_queue.c (recursive directory traversal)\n");
    printf(" W5 | Metadata & Exit Codes     | src/metadata.c (fchmod, fchown, futimens)\n");
    printf(" W6 | Signals & Interruption    | src/signals.c (sigaction, SIGINT atomic flush)\n");
    printf(" W7 | Bounded Buffer & Pipes    | src/buffer_pool.c (producer-consumer backpressure)\n");
    printf(" W8 | Memory Discipline (VM)    | valgrind-clean lifecycle, no descriptor leaks\n");
    printf(" W9 | Files, Inodes & Layout    | src/sparse.c (SEEK_HOLE/SEEK_DATA) & src/resume.c\n");
    printf("W10 | Threads & Synchronization | src/backend_pipeline.c (reader/writer pthreads)\n");
    printf("W11 | Semaphores & Verification | src/checksum.c (streaming SHA256 & xxHash64)\n");
    printf("W12 | Async I/O & Benchmarking  | src/backend_uring.c (Linux io_uring SQ/CQ rings)\n");
    printf("===============================================================================\n");
}

static void print_usage(const char *prog_name) {
    printf("Usage: %s [OPTIONS] SOURCE DESTINATION\n\n", prog_name);
    printf("Options:\n");
    printf("  -b, --backend=NAME      Copy engine: 'pipeline' (default), 'naive', 'uring'\n");
    printf("  -s, --buffer-size=SIZE  Buffer size (e.g., 64K, 1M, 4M, 16M; default: 1M)\n");
    printf("  -q, --queue-depth=N     Number of ring buffer slots (default: 8)\n");
    printf("  -j, --jobs=N            Concurrent worker threads for directories (default: 4)\n");
    printf("  -v, --verify            Verify transfer integrity via checksum pass\n");
    printf("  -c, --checksum=TYPE     Checksum type: 'sha256' (default), 'xxh64'\n");
    printf("  -S, --sparse            Enable sparse file hole detection and skipping\n");
    printf("  -p, --preserve          Preserve mode permissions, ownership, and timestamps\n");
    printf("  -r, --resume            Resume an interrupted copy using state journal\n");
    printf("  -R, --recursive         Recursively copy directories\n");
    printf("  -Q, --quiet             Quiet mode: suppress interactive progress bar\n");
    printf("  -V, --verbose           Verbose output\n");
    printf("      --syllabus          Print 12-week Operating Systems syllabus alignment\n");
    printf("      --version           Show version information\n");
    printf("  -h, --help              Show this help message\n\n");
    printf("Examples:\n");
    printf("  %s -v -S -p huge_vm.qcow2 /mnt/backup/vm.qcow2\n", prog_name);
    printf("  %s -b uring -s 4M -q 16 large_dataset.tar /mnt/ssd/\n", prog_name);
    printf("  %s -R -j 8 -p /home/user/code/ /mnt/backup/code/\n", prog_name);
}

int main(int argc, char **argv) {
    copy_options_t opts = {
        .backend = BACKEND_PIPELINE,
        .buffer_size = COPYFAST_DEFAULT_BUFFER_SIZE,
        .queue_depth = COPYFAST_DEFAULT_QUEUE_DEPTH,
        .jobs = COPYFAST_DEFAULT_JOBS,
        .verify_checksum = false,
        .checksum_type = CHECKSUM_SHA256,
        .preserve_metadata = false,
        .sparse_aware = true,
        .resume = false,
        .direct_io = false,
        .recursive = false,
        .quiet = false,
        .verbose = false,
        .force_overwrite = false,
        .custom_resume_path = NULL
    };

    static struct option long_options[] = {
        {"backend",     required_argument, 0, 'b'},
        {"buffer-size", required_argument, 0, 's'},
        {"queue-depth", required_argument, 0, 'q'},
        {"jobs",        required_argument, 0, 'j'},
        {"verify",      no_argument,       0, 'v'},
        {"checksum",    required_argument, 0, 'c'},
        {"sparse",      no_argument,       0, 'S'},
        {"preserve",    no_argument,       0, 'p'},
        {"resume",      no_argument,       0, 'r'},
        {"recursive",   no_argument,       0, 'R'},
        {"quiet",       no_argument,       0, 'Q'},
        {"verbose",     no_argument,       0, 'V'},
        {"syllabus",    no_argument,       0, 1001},
        {"version",     no_argument,       0, 1002},
        {"help",        no_argument,       0, 'h'},
        {0, 0, 0, 0}
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "b:s:q:j:vc:SpRrdQVh", long_options, NULL)) != -1) {
        switch (opt) {
            case 'b':
                if (strcmp(optarg, "naive") == 0) opts.backend = BACKEND_NAIVE;
                else if (strcmp(optarg, "pipeline") == 0) opts.backend = BACKEND_PIPELINE;
                else if (strcmp(optarg, "uring") == 0) opts.backend = BACKEND_URING;
                else {
                    fprintf(stderr, "Unknown backend '%s'. Use 'pipeline', 'naive', or 'uring'.\n", optarg);
                    return 1;
                }
                break;
            case 's':
                if (copyfast_parse_size(optarg, &opts.buffer_size) != 0 || opts.buffer_size == 0) {
                    fprintf(stderr, "Invalid buffer size: '%s'\n", optarg);
                    return 1;
                }
                break;
            case 'q':
                opts.queue_depth = (size_t)atoi(optarg);
                if (opts.queue_depth < 2) opts.queue_depth = 2;
                break;
            case 'j':
                opts.jobs = atoi(optarg);
                if (opts.jobs < 1) opts.jobs = 1;
                break;
            case 'v':
                opts.verify_checksum = true;
                break;
            case 'c':
                if (strcmp(optarg, "sha256") == 0) opts.checksum_type = CHECKSUM_SHA256;
                else if (strcmp(optarg, "xxh64") == 0) opts.checksum_type = CHECKSUM_XXH64;
                else {
                    fprintf(stderr, "Unknown checksum algorithm '%s'. Use 'sha256' or 'xxh64'.\n", optarg);
                    return 1;
                }
                break;
            case 'S':
                opts.sparse_aware = true;
                break;
            case 'd':
                opts.direct_io = true;
                break;
            case 'p':
                opts.preserve_metadata = true;
                break;
            case 'r':
                opts.resume = true;
                break;
            case 'R':
                opts.recursive = true;
                break;
            case 'Q':
                opts.quiet = true;
                break;
            case 'V':
                opts.verbose = true;
                break;
            case 1001:
                print_syllabus();
                return 0;
            case 1002:
                printf("CopyFast version %s\n", COPYFAST_VERSION);
                return 0;
            case 'h':
            default:
                print_usage(argv[0]);
                return 0;
        }
    }

    if (optind + 2 != argc) {
        print_usage(argv[0]);
        return 1;
    }

    const char *src_path = argv[optind];
    const char *dest_path = argv[optind + 1];

    if (!opts.quiet) {
        print_banner();
    }

    signals_init();

    struct stat src_st;
    if (lstat(src_path, &src_st) != 0) {
        fprintf(stderr, "Error: cannot access source '%s': %s\n", src_path, strerror(errno));
        return 1;
    }

    /* Target directory handling: if dest is a directory, append src basename */
    char resolved_dest[2048];
    struct stat dest_st;
    bool dest_is_dir = (stat(dest_path, &dest_st) == 0 && S_ISDIR(dest_st.st_mode));

    char base[1024];
    copyfast_get_basename(src_path, base, sizeof(base));

    size_t src_len = strlen(src_path);
    bool src_has_trailing_slash = (src_len > 1 && src_path[src_len - 1] == '/');
    size_t dest_len = strlen(dest_path);
    const char *sep = (dest_len > 0 && dest_path[dest_len - 1] == '/') ? "" : "/";

    if (dest_is_dir && !S_ISDIR(src_st.st_mode)) {
        snprintf(resolved_dest, sizeof(resolved_dest), "%s%s%s", dest_path, sep, base);
    } else if (dest_is_dir && S_ISDIR(src_st.st_mode) && !src_has_trailing_slash && strcmp(base, ".") != 0) {
        snprintf(resolved_dest, sizeof(resolved_dest), "%s%s%s", dest_path, sep, base);
    } else {
        snprintf(resolved_dest, sizeof(resolved_dest), "%s", dest_path);
    }

    copyfast_get_time(&g_stats.start_time);

    int copy_result = 0;

    if (S_ISDIR(src_st.st_mode)) {
        if (!opts.recursive) {
            fprintf(stderr, "Error: '%s' is a directory (use -R/--recursive to copy)\n", src_path);
            return 1;
        }
        progress_meter_t *prog = progress_create(0, opts.quiet);
        copy_result = copy_directory_recursive(src_path, resolved_dest, &opts, prog);
        progress_finish(prog);
        progress_destroy(prog);
    } else {
        progress_meter_t *prog = progress_create(src_st.st_size, opts.quiet);
        progress_set_file(prog, src_path);
        copy_result = copy_file_dispatch(src_path, resolved_dest, &opts, prog);
        progress_finish(prog);
        progress_destroy(prog);
    }

    copyfast_get_time(&g_stats.end_time);

    if (g_stop_requested) {
        fprintf(stderr, "\n[Interrupted] Copy operation cancelled by user signal.\n");
        if (opts.resume) {
            fprintf(stderr, "[Interrupted] State journal saved. Re-run with --resume to continue.\n");
        }
        return 130;
    }

    if (copy_result != 0) {
        fprintf(stderr, "\n[Failed] Copy operation encountered errors (status=%d).\n", copy_result);
        return 1;
    }

    /* Print final summary */
    if (!opts.quiet) {
        double elapsed = copyfast_get_elapsed_sec(&g_stats.start_time, &g_stats.end_time);
        uint64_t bytes_written = atomic_load(&g_stats.total_bytes_written);
        uint64_t sparse_skipped = atomic_load(&g_stats.total_sparse_bytes_skipped);
        uint64_t files_copied = atomic_load(&g_stats.total_files_copied);
        double mbps = (elapsed > 0.001) ? ((double)bytes_written / (1024.0 * 1024.0)) / elapsed : 0.0;

        printf("\n================ CopyFast Transfer Summary ================\n");
        printf(" Status:             SUCCESS\n");
        printf(" Backend Engine:     %s\n",
               opts.backend == BACKEND_PIPELINE ? "Overlapped Pipeline (Pthreads)" :
               opts.backend == BACKEND_URING    ? "Linux io_uring (Async SQ/CQ)" : "Naive (Sequential)");
        printf(" Files Transferred:  %lu\n", (unsigned long)files_copied);
        printf(" Data Written:       %.2f MB (%lu bytes)\n", (double)bytes_written / (1024.0 * 1024.0), (unsigned long)bytes_written);
        if (sparse_skipped > 0) {
            printf(" Sparse Holes Saved: %.2f MB (%lu bytes skipped)\n", (double)sparse_skipped / (1024.0 * 1024.0), (unsigned long)sparse_skipped);
        }
        printf(" Elapsed Time:       %.3f seconds\n", elapsed);
        printf(" Effective Speed:    %.2f MB/s\n", mbps);
        if (opts.verify_checksum) {
            printf(" Verification:       PASS (Integrity cryptographically verified)\n");
        }
        printf("===========================================================\n");
    }

    return 0;
}
