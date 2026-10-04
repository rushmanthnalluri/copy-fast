#include "backends.h"
#include "metadata.h"

int copy_file_dispatch(const char *src_path, const char *dest_path, const copy_options_t *opts, progress_meter_t *prog) {
    struct stat st;
    if (lstat(src_path, &st) != 0) {
        fprintf(stderr, "Cannot stat '%s': %s\n", src_path, strerror(errno));
        atomic_fetch_add(&g_stats.total_files_failed, 1);
        return -1;
    }

    /* Handle symbolic link */
    if (S_ISLNK(st.st_mode)) {
        char target[1024];
        ssize_t len = readlink(src_path, target, sizeof(target) - 1);
        if (len < 0) {
            fprintf(stderr, "readlink failed on '%s': %s\n", src_path, strerror(errno));
            atomic_fetch_add(&g_stats.total_files_failed, 1);
            return -1;
        }
        target[len] = '\0';

        unlink(dest_path); /* Remove destination if exists */
        if (symlink(target, dest_path) != 0) {
            fprintf(stderr, "symlink failed '%s' -> '%s': %s\n", dest_path, target, strerror(errno));
            atomic_fetch_add(&g_stats.total_files_failed, 1);
            return -1;
        }

        if (opts->preserve_metadata) {
            metadata_preserve_path(src_path, dest_path, &st);
        }

        atomic_fetch_add(&g_stats.total_files_copied, 1);
        return 0;
    }

    switch (opts->backend) {
        case BACKEND_NAIVE:
            return copy_file_naive(src_path, dest_path, opts, prog);
        case BACKEND_URING:
            return copy_file_uring(src_path, dest_path, opts, prog);
        case BACKEND_PIPELINE:
        default:
            /* If running parallel worker jobs and file is small (< 512KB), avoid per-file pthread creation */
            if (opts->jobs > 1 && st.st_size < (512 * 1024)) {
                return copy_file_naive(src_path, dest_path, opts, prog);
            }
            return copy_file_pipeline(src_path, dest_path, opts, prog);
    }
}
