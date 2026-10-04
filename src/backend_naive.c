#include "backends.h"
#include "checksum.h"
#include "sparse.h"
#include "metadata.h"
#include "resume.h"

int copy_file_naive(const char *src_path, const char *dest_path, const copy_options_t *opts, progress_meter_t *prog) {
    int src_fd = open(src_path, O_RDONLY);
    if (src_fd < 0) {
        fprintf(stderr, "Error opening source '%s': %s\n", src_path, strerror(errno));
        atomic_fetch_add(&g_stats.total_files_failed, 1);
        return -1;
    }

    struct stat st;
    if (fstat(src_fd, &st) != 0) {
        close(src_fd);
        atomic_fetch_add(&g_stats.total_files_failed, 1);
        return -1;
    }

    if (!S_ISREG(st.st_mode)) {
        fprintf(stderr, "Skipping non-regular file '%s'\n", src_path);
        close(src_fd);
        return 0;
    }

    /* Check resume */
    off_t start_offset = 0;
    char journal_path[1024];
    resume_get_journal_path(dest_path, journal_path, sizeof(journal_path));
    resume_record_t resume_rec;

    if (opts->resume) {
        if (resume_read_journal(journal_path, &st, &resume_rec) == 0) {
            start_offset = resume_rec.copied_offset;
            atomic_fetch_add(&g_stats.total_files_resumed, 1);
            if (opts->verbose) {
                printf("[Resume] Resuming '%s' from byte %ld / %ld\n", src_path, (long)start_offset, (long)st.st_size);
            }
        }
    }

    int open_flags = O_WRONLY | O_CREAT;
    if (start_offset == 0) {
        open_flags |= O_TRUNC;
    }

    int dest_fd = open(dest_path, open_flags, 0666);
    if (dest_fd < 0) {
        fprintf(stderr, "Error creating destination '%s': %s\n", dest_path, strerror(errno));
        close(src_fd);
        atomic_fetch_add(&g_stats.total_files_failed, 1);
        return -1;
    }

    size_t buf_size = opts->buffer_size ? opts->buffer_size : COPYFAST_DEFAULT_BUFFER_SIZE;
    void *buf = NULL;
    int mem_res = posix_memalign(&buf, 4096, buf_size);
    if (mem_res != 0 || !buf) {
        close(src_fd);
        close(dest_fd);
        return -1;
    }

    checksum_ctx_t chk_ctx;
    if (opts->verify_checksum && opts->checksum_type != CHECKSUM_NONE) {
        checksum_init(&chk_ctx, opts->checksum_type);
    }

    off_t current_offset = start_offset;
    if (start_offset > 0) {
        lseek(src_fd, start_offset, SEEK_SET);
        lseek(dest_fd, start_offset, SEEK_SET);
    }

    /* Sequential I/O advice */
    posix_fadvise(src_fd, current_offset, 0, POSIX_FADV_SEQUENTIAL);

    int status = 0;

    if (opts->sparse_aware && sparse_is_supported(src_fd)) {
        /* Sparse-aware copy loop */
        file_extent_t extent;
        while (current_offset < st.st_size && !g_stop_requested) {
            int ext_res = sparse_get_next_extent(src_fd, st.st_size, &current_offset, &extent);
            if (ext_res <= 0) break;

            if (extent.is_hole) {
                /* Skip hole in destination without writing zeroes */
                lseek(dest_fd, extent.offset + extent.length, SEEK_SET);
                atomic_fetch_add(&g_stats.total_sparse_bytes_skipped, extent.length);
                atomic_fetch_add(&g_stats.total_holes_detected, 1);
                if (prog) progress_update(prog, 0, extent.length);
                if (opts->verify_checksum && opts->checksum_type != CHECKSUM_NONE) {
                    checksum_update_zeros(&chk_ctx, (size_t)extent.length);
                }
            } else {
                /* Copy data extent */
                off_t ext_remain = extent.length;
                lseek(src_fd, extent.offset, SEEK_SET);
                lseek(dest_fd, extent.offset, SEEK_SET);

                while (ext_remain > 0 && !g_stop_requested) {
                    size_t to_read = (size_t)((ext_remain < (off_t)buf_size) ? ext_remain : (off_t)buf_size);
                    ssize_t n_read = read(src_fd, buf, to_read);
                    if (n_read <= 0) {
                        if (n_read < 0) status = -1;
                        break;
                    }

                    ssize_t n_written = 0;
                    while (n_written < n_read) {
                        ssize_t w = write(dest_fd, (char *)buf + n_written, (size_t)(n_read - n_written));
                        if (w <= 0) {
                            status = -1;
                            break;
                        }
                        n_written += w;
                    }

                    if (status != 0) break;

                    if (opts->verify_checksum && opts->checksum_type != CHECKSUM_NONE) {
                        checksum_update(&chk_ctx, buf, (size_t)n_read);
                    }

                    ext_remain -= n_read;
                    atomic_fetch_add(&g_stats.total_bytes_read, n_read);
                    atomic_fetch_add(&g_stats.total_bytes_written, n_written);
                    if (prog) progress_update(prog, n_written, 0);
                }
            }

            if (status != 0) break;
        }

        /* Ensure destination file length matches source (handles trailing holes) */
        if (ftruncate(dest_fd, st.st_size) != 0) {
            /* Handled */
        }
    } else {
        /* Standard blocking read/write loop */
        while (current_offset < st.st_size && !g_stop_requested) {
            size_t to_read = (size_t)(((st.st_size - current_offset) < (off_t)buf_size) ? (st.st_size - current_offset) : (off_t)buf_size);
            ssize_t n_read = read(src_fd, buf, to_read);
            if (n_read <= 0) {
                if (n_read < 0) status = -1;
                break;
            }

            /* In-memory zero detection fallback for sparsity */
            if (opts->sparse_aware && sparse_is_buffer_zero(buf, (size_t)n_read)) {
                lseek(dest_fd, n_read, SEEK_CUR);
                atomic_fetch_add(&g_stats.total_sparse_bytes_skipped, n_read);
                atomic_fetch_add(&g_stats.total_holes_detected, 1);
                if (prog) progress_update(prog, 0, n_read);
            } else {
                ssize_t n_written = 0;
                while (n_written < n_read) {
                    ssize_t w = write(dest_fd, (char *)buf + n_written, (size_t)(n_read - n_written));
                    if (w <= 0) {
                        status = -1;
                        break;
                    }
                    n_written += w;
                }
                if (status != 0) break;

                atomic_fetch_add(&g_stats.total_bytes_written, n_written);
                if (prog) progress_update(prog, n_written, 0);
            }

            if (opts->verify_checksum && opts->checksum_type != CHECKSUM_NONE) {
                checksum_update(&chk_ctx, buf, (size_t)n_read);
            }

            current_offset += n_read;
            atomic_fetch_add(&g_stats.total_bytes_read, n_read);

            /* Save periodic resume checkpoint if requested */
            if (opts->resume && (current_offset % (16 * 1024 * 1024) == 0)) {
                resume_save_checkpoint(journal_path, &st, current_offset, 0, RESUME_STATUS_IN_PROGRESS);
            }
        }
        if (ftruncate(dest_fd, st.st_size) != 0) {
            /* Handled */
        }
    }

    free(buf);

    if (g_stop_requested) {
        if (opts->resume) {
            fdatasync(dest_fd);
            resume_save_checkpoint(journal_path, &st, current_offset, 0, RESUME_STATUS_INTERRUPTED);
        }
        close(src_fd);
        close(dest_fd);
        return -2;
    }

    if (status != 0) {
        close(src_fd);
        close(dest_fd);
        atomic_fetch_add(&g_stats.total_files_failed, 1);
        return -1;
    }

    /* Verification pass */
    if (opts->verify_checksum && opts->checksum_type != CHECKSUM_NONE) {
        fdatasync(dest_fd);
        char dest_hash[128] = {0};
        char src_hash[128] = {0};
        if (start_offset > 0) {
            checksum_file(src_path, opts->checksum_type, src_hash, sizeof(src_hash));
        } else {
            checksum_final(&chk_ctx, src_hash, sizeof(src_hash));
        }
        if (checksum_file(dest_path, opts->checksum_type, dest_hash, sizeof(dest_hash)) == 0) {
            if (strcmp(src_hash, dest_hash) != 0) {
                fprintf(stderr, "Checksum mismatch for '%s': src=%s dest=%s\n", dest_path, src_hash, dest_hash);
                close(src_fd);
                close(dest_fd);
                atomic_fetch_add(&g_stats.total_files_failed, 1);
                return -3;
            }
            if (opts->verbose) {
                printf("[Verified] %s: %s\n", dest_path, dest_hash);
            }
        }
    }

    /* Metadata preservation */
    if (opts->preserve_metadata) {
        metadata_preserve_fd(src_fd, dest_fd, &st);
    }

    close(src_fd);
    close(dest_fd);

    /* Clean up resume journal */
    if (opts->resume) {
        resume_cleanup_journal(journal_path);
    }

    atomic_fetch_add(&g_stats.total_files_copied, 1);
    return 0;
}
