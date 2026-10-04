#include "backends.h"
#include "buffer_pool.h"
#include "checksum.h"
#include "sparse.h"
#include "metadata.h"
#include "resume.h"
#include "signals.h"

typedef struct {
    int                 src_fd;
    off_t               total_size;
    off_t               start_offset;
    buffer_pool_t      *pool;
    const copy_options_t *opts;
    checksum_ctx_t     *chk_ctx;
    int                 error_code;
} reader_ctx_t;

typedef struct {
    int                 dest_fd;
    const char         *journal_path;
    const struct stat  *src_st;
    buffer_pool_t      *pool;
    const copy_options_t *opts;
    progress_meter_t   *prog;
    off_t               last_checkpoint_offset;
    int                 error_code;
} writer_ctx_t;

static void *reader_thread_fn(void *arg) {
    reader_ctx_t *ctx = (reader_ctx_t *)arg;
    signals_block_in_thread();

    off_t curr = ctx->start_offset;
    posix_fadvise(ctx->src_fd, curr, 0, POSIX_FADV_SEQUENTIAL | POSIX_FADV_WILLNEED);

    if (ctx->opts->sparse_aware && sparse_is_supported(ctx->src_fd)) {
        file_extent_t extent;
        while (curr < ctx->total_size && !g_stop_requested) {
            int ext_res = sparse_get_next_extent(ctx->src_fd, ctx->total_size, &curr, &extent);
            if (ext_res <= 0) break;

            if (extent.is_hole) {
                if (ctx->chk_ctx) {
                    checksum_update_zeros(ctx->chk_ctx, (size_t)extent.length);
                }

                buffer_slot_t *slot = buffer_pool_acquire_write_slot(ctx->pool);
                if (!slot) break;

                slot->offset = extent.offset;
                slot->len = (size_t)extent.length;
                slot->is_hole = true;
                slot->is_eof = false;
                buffer_pool_commit_write_slot(ctx->pool, slot);
            } else {
                off_t ext_remain = extent.length;
                off_t ext_pos = extent.offset;

                while (ext_remain > 0 && !g_stop_requested) {
                    buffer_slot_t *slot = buffer_pool_acquire_write_slot(ctx->pool);
                    if (!slot) break;

                    size_t to_read = (size_t)((ext_remain < (off_t)slot->capacity) ? ext_remain : (off_t)slot->capacity);
                    ssize_t n = pread(ctx->src_fd, slot->data, to_read, ext_pos);
                    if (n <= 0) {
                        if (n < 0) ctx->error_code = errno;
                        slot->error_code = ctx->error_code;
                        buffer_pool_commit_write_slot(ctx->pool, slot);
                        break;
                    }

                    slot->offset = ext_pos;
                    slot->len = (size_t)n;
                    slot->is_hole = false;
                    slot->is_eof = false;

                    if (ctx->chk_ctx) {
                        checksum_update(ctx->chk_ctx, slot->data, (size_t)n);
                    }

                    atomic_fetch_add(&g_stats.total_bytes_read, n);
                    buffer_pool_commit_write_slot(ctx->pool, slot);

                    ext_remain -= n;
                    ext_pos += n;
                }
            }
        }
    } else {
        while (curr < ctx->total_size && !g_stop_requested) {
            buffer_slot_t *slot = buffer_pool_acquire_write_slot(ctx->pool);
            if (!slot) break;

            size_t to_read = (size_t)(((ctx->total_size - curr) < (off_t)slot->capacity) ? (ctx->total_size - curr) : (off_t)slot->capacity);
            ssize_t n = pread(ctx->src_fd, slot->data, to_read, curr);
            if (n <= 0) {
                if (n < 0) ctx->error_code = errno;
                slot->error_code = ctx->error_code;
                buffer_pool_commit_write_slot(ctx->pool, slot);
                break;
            }

            slot->offset = curr;
            slot->len = (size_t)n;
            slot->is_hole = (ctx->opts->sparse_aware && sparse_is_buffer_zero(slot->data, (size_t)n));
            slot->is_eof = false;

            if (ctx->chk_ctx) {
                checksum_update(ctx->chk_ctx, slot->data, (size_t)n);
            }

            atomic_fetch_add(&g_stats.total_bytes_read, n);
            buffer_pool_commit_write_slot(ctx->pool, slot);

            curr += n;
        }
    }

    /* Send EOF sentinel slot */
    buffer_slot_t *eof_slot = buffer_pool_acquire_write_slot(ctx->pool);
    if (eof_slot) {
        eof_slot->is_eof = true;
        eof_slot->len = 0;
        buffer_pool_commit_write_slot(ctx->pool, eof_slot);
    }

    return NULL;
}

static void *writer_thread_fn(void *arg) {
    writer_ctx_t *ctx = (writer_ctx_t *)arg;
    signals_block_in_thread();

    off_t last_checkpoint = ctx->last_checkpoint_offset;

    while (!g_stop_requested) {
        buffer_slot_t *slot = buffer_pool_acquire_read_slot(ctx->pool);
        if (!slot) break;

        if (slot->is_eof) {
            buffer_pool_release_read_slot(ctx->pool, slot);
            break;
        }

        if (slot->error_code != 0) {
            ctx->error_code = slot->error_code;
            buffer_pool_release_read_slot(ctx->pool, slot);
            break;
        }

        if (slot->is_hole) {
            /* Seek over hole in destination */
            atomic_fetch_add(&g_stats.total_sparse_bytes_skipped, slot->len);
            atomic_fetch_add(&g_stats.total_holes_detected, 1);
            if (ctx->prog) {
                progress_update(ctx->prog, 0, (off_t)slot->len);
            }
            last_checkpoint = slot->offset + slot->len;
        } else {
            /* Write data extent to disk */
            size_t written = 0;
            while (written < slot->len) {
                ssize_t w = pwrite(ctx->dest_fd, (const char *)slot->data + written,
                                   slot->len - written, slot->offset + written);
                if (w <= 0) {
                    ctx->error_code = errno;
                    break;
                }
                written += (size_t)w;
            }

            if (ctx->error_code != 0) {
                buffer_pool_release_read_slot(ctx->pool, slot);
                break;
            }

            atomic_fetch_add(&g_stats.total_bytes_written, written);
            if (ctx->prog) {
                progress_update(ctx->prog, (off_t)written, 0);
            }

            last_checkpoint = slot->offset + slot->len;

            /* Checkpoint resume journal periodically (every 16MB) */
            if (ctx->opts->resume && ((size_t)(last_checkpoint % (16 * 1024 * 1024)) < slot->len)) {
                resume_save_checkpoint(ctx->journal_path, ctx->src_st,
                                       last_checkpoint, 0, RESUME_STATUS_IN_PROGRESS);
            }
        }

        buffer_pool_release_read_slot(ctx->pool, slot);
    }

    ctx->last_checkpoint_offset = last_checkpoint;
    return NULL;
}

int copy_file_pipeline(const char *src_path, const char *dest_path, const copy_options_t *opts, progress_meter_t *prog) {
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
        close(src_fd);
        return 0;
    }

    /* Resume state check */
    off_t start_offset = 0;
    char journal_path[1024];
    resume_get_journal_path(dest_path, journal_path, sizeof(journal_path));
    resume_record_t resume_rec;

    if (opts->resume) {
        if (resume_read_journal(journal_path, &st, &resume_rec) == 0) {
            start_offset = resume_rec.copied_offset;
            atomic_fetch_add(&g_stats.total_files_resumed, 1);
            if (opts->verbose) {
                printf("[Resume] Overlapped pipeline resuming '%s' at offset %ld / %ld\n",
                       src_path, (long)start_offset, (long)st.st_size);
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

    size_t q_depth = opts->queue_depth ? opts->queue_depth : COPYFAST_DEFAULT_QUEUE_DEPTH;
    size_t b_size  = opts->buffer_size ? opts->buffer_size : COPYFAST_DEFAULT_BUFFER_SIZE;

    buffer_pool_t *pool = buffer_pool_create(q_depth, b_size);
    if (!pool) {
        close(src_fd);
        close(dest_fd);
        return -1;
    }

    checksum_ctx_t chk_ctx;
    checksum_ctx_t *p_chk = NULL;
    if (opts->verify_checksum && opts->checksum_type != CHECKSUM_NONE) {
        checksum_init(&chk_ctx, opts->checksum_type);
        p_chk = &chk_ctx;
    }

    reader_ctx_t r_ctx = {
        .src_fd = src_fd,
        .total_size = st.st_size,
        .start_offset = start_offset,
        .pool = pool,
        .opts = opts,
        .chk_ctx = p_chk,
        .error_code = 0
    };

    writer_ctx_t w_ctx = {
        .dest_fd = dest_fd,
        .journal_path = journal_path,
        .src_st = &st,
        .pool = pool,
        .opts = opts,
        .prog = prog,
        .last_checkpoint_offset = start_offset,
        .error_code = 0
    };

    pthread_t r_thread, w_thread;
    pthread_create(&r_thread, NULL, reader_thread_fn, &r_ctx);
    pthread_create(&w_thread, NULL, writer_thread_fn, &w_ctx);

    pthread_join(r_thread, NULL);
    pthread_join(w_thread, NULL);

    /* Truncate destination to total size to preserve trailing holes */
    if (ftruncate(dest_fd, st.st_size) != 0) {
        /* Handled */
    }

    int ret_status = 0;
    if (r_ctx.error_code != 0 || w_ctx.error_code != 0) {
        ret_status = -1;
    }

    if (g_stop_requested) {
        if (opts->resume) {
            fdatasync(dest_fd);
            resume_save_checkpoint(journal_path, &st, w_ctx.last_checkpoint_offset, 0, RESUME_STATUS_INTERRUPTED);
        }
        ret_status = -2;
    }

    buffer_pool_destroy(pool);

    if (ret_status == 0) {
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
                    ret_status = -3;
                } else if (opts->verbose) {
                    printf("[Verified] %s: %s\n", dest_path, dest_hash);
                }
            }
        }

        /* Metadata preservation */
        if (opts->preserve_metadata) {
            metadata_preserve_fd(src_fd, dest_fd, &st);
        }

        /* Clean up resume journal on success */
        if (opts->resume) {
            resume_cleanup_journal(journal_path);
        }

        atomic_fetch_add(&g_stats.total_files_copied, 1);
    } else if (ret_status != -2) {
        atomic_fetch_add(&g_stats.total_files_failed, 1);
    }

    close(src_fd);
    close(dest_fd);

    return ret_status;
}
