#include "backends.h"
#include "checksum.h"
#include "sparse.h"
#include "metadata.h"
#include "resume.h"
#include <liburing.h>

#define OP_TYPE_READ  1
#define OP_TYPE_WRITE 2

typedef struct {
    int    slot_id;
    int    op_type;
    off_t  offset;
    size_t len;
} uring_task_t;

typedef enum {
    SLOT_FREE = 0,
    SLOT_READ_PENDING,
    SLOT_WRITE_PENDING
} slot_state_t;

typedef struct {
    void         *buf;
    size_t        capacity;
    slot_state_t  state;
    uring_task_t  task;
} uring_slot_t;

int copy_file_uring(const char *src_path, const char *dest_path, const copy_options_t *opts, progress_meter_t *prog) {
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
                printf("[Resume] io_uring resuming '%s' at offset %ld / %ld\n",
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

    struct io_uring ring;
    int ring_res = io_uring_queue_init((unsigned)(q_depth * 2), &ring, 0);
    if (ring_res < 0) {
        if (opts->verbose) {
            fprintf(stderr, "io_uring_queue_init failed (%d), falling back to pipeline backend\n", ring_res);
        }
        close(src_fd);
        close(dest_fd);
        return copy_file_pipeline(src_path, dest_path, opts, prog);
    }

    uring_slot_t *slots = (uring_slot_t *)calloc(q_depth, sizeof(uring_slot_t));
    if (!slots) {
        io_uring_queue_exit(&ring);
        close(src_fd);
        close(dest_fd);
        return -1;
    }

    for (size_t i = 0; i < q_depth; ++i) {
        void *p = NULL;
        int r = posix_memalign(&p, 4096, b_size);
        (void)r;
        slots[i].buf = p;
        slots[i].capacity = b_size;
        slots[i].state = SLOT_FREE;
        slots[i].task.slot_id = (int)i;
    }

    checksum_ctx_t chk_ctx;
    if (opts->verify_checksum && opts->checksum_type != CHECKSUM_NONE) {
        checksum_init(&chk_ctx, opts->checksum_type);
    }

    posix_fadvise(src_fd, start_offset, 0, POSIX_FADV_SEQUENTIAL | POSIX_FADV_WILLNEED);

    off_t current_read_offset = start_offset;
    int in_flight = 0;
    int status = 0;
    off_t last_checkpoint = start_offset;

    /* Main io_uring event loop */
    while ((current_read_offset < st.st_size || in_flight > 0) && !g_stop_requested) {
        /* Submit reads as long as slots are free and bytes remain */
        while (current_read_offset < st.st_size && in_flight < (int)q_depth && !g_stop_requested) {
            /* Find a free slot */
            int free_slot = -1;
            for (size_t i = 0; i < q_depth; ++i) {
                if (slots[i].state == SLOT_FREE) {
                    free_slot = (int)i;
                    break;
                }
            }
            if (free_slot < 0) break;

            /* Check sparse extent if enabled */
            if (opts->sparse_aware && sparse_is_supported(src_fd)) {
                file_extent_t extent;
                int ext_res = sparse_get_next_extent(src_fd, st.st_size, &current_read_offset, &extent);
                if (ext_res <= 0) break;

                if (extent.is_hole) {
                    atomic_fetch_add(&g_stats.total_sparse_bytes_skipped, extent.length);
                    atomic_fetch_add(&g_stats.total_holes_detected, 1);
                    if (prog) progress_update(prog, 0, extent.length);
                    if (opts->verify_checksum && opts->checksum_type != CHECKSUM_NONE) {
                        checksum_update_zeros(&chk_ctx, (size_t)extent.length);
                    }
                    continue;
                } else {
                    current_read_offset = extent.offset;
                }
            }

            size_t to_read = (size_t)(((st.st_size - current_read_offset) < (off_t)b_size) ?
                                      (st.st_size - current_read_offset) : (off_t)b_size);

            struct io_uring_sqe *sqe = io_uring_get_sqe(&ring);
            if (!sqe) break;

            slots[free_slot].state = SLOT_READ_PENDING;
            slots[free_slot].task.op_type = OP_TYPE_READ;
            slots[free_slot].task.offset = current_read_offset;
            slots[free_slot].task.len = to_read;

            io_uring_prep_read(sqe, src_fd, slots[free_slot].buf, (unsigned)to_read, current_read_offset);
            io_uring_sqe_set_data(sqe, &slots[free_slot].task);

            current_read_offset += to_read;
            in_flight++;
        }

        if (in_flight == 0 && current_read_offset >= st.st_size) {
            break;
        }

        /* Submit SQEs and wait for at least 1 completion */
        int submitted = io_uring_submit(&ring);
        (void)submitted;

        struct io_uring_cqe *cqe = NULL;
        int wait_ret = io_uring_wait_cqe(&ring, &cqe);
        if (wait_ret < 0) {
            if (errno == EINTR) continue;
            status = -1;
            break;
        }

        if (!cqe) continue;

        uring_task_t *task = (uring_task_t *)io_uring_cqe_get_data(cqe);
        int res = cqe->res;
        io_uring_cqe_seen(&ring, cqe);
        in_flight--;

        if (!task) continue;
        int s_idx = task->slot_id;

        if (res < 0) {
            fprintf(stderr, "io_uring operation failed (res=%d): %s\n", res, strerror(-res));
            status = -1;
            slots[s_idx].state = SLOT_FREE;
            break;
        }

        if (task->op_type == OP_TYPE_READ) {
            size_t bytes_read = (size_t)res;
            atomic_fetch_add(&g_stats.total_bytes_read, bytes_read);

            if (bytes_read < task->len && current_read_offset > (off_t)(task->offset + bytes_read)) {
                current_read_offset = task->offset + bytes_read;
            }

            if (opts->verify_checksum && opts->checksum_type != CHECKSUM_NONE) {
                checksum_update(&chk_ctx, slots[s_idx].buf, bytes_read);
            }

            /* Sparse in-memory check fallback */
            if (opts->sparse_aware && sparse_is_buffer_zero(slots[s_idx].buf, bytes_read)) {
                atomic_fetch_add(&g_stats.total_sparse_bytes_skipped, bytes_read);
                atomic_fetch_add(&g_stats.total_holes_detected, 1);
                if (prog) progress_update(prog, 0, (off_t)bytes_read);
                slots[s_idx].state = SLOT_FREE;
            } else {
                /* Queue write SQE immediately */
                struct io_uring_sqe *w_sqe = io_uring_get_sqe(&ring);
                if (w_sqe) {
                    slots[s_idx].state = SLOT_WRITE_PENDING;
                    slots[s_idx].task.op_type = OP_TYPE_WRITE;
                    slots[s_idx].task.offset = task->offset;
                    slots[s_idx].task.len = bytes_read;

                    io_uring_prep_write(w_sqe, dest_fd, slots[s_idx].buf, (unsigned)bytes_read, task->offset);
                    io_uring_sqe_set_data(w_sqe, &slots[s_idx].task);
                    in_flight++;
                } else {
                    /* Synchronous fallback if SQ is full */
                    ssize_t sync_w = pwrite(dest_fd, slots[s_idx].buf, bytes_read, task->offset);
                    (void)sync_w;
                    atomic_fetch_add(&g_stats.total_bytes_written, bytes_read);
                    if (prog) progress_update(prog, (off_t)bytes_read, 0);
                    slots[s_idx].state = SLOT_FREE;
                }
            }
        } else if (task->op_type == OP_TYPE_WRITE) {
            size_t bytes_written = (size_t)res;
            atomic_fetch_add(&g_stats.total_bytes_written, bytes_written);
            if (prog) progress_update(prog, (off_t)bytes_written, 0);

            slots[s_idx].state = SLOT_FREE;
            last_checkpoint = task->offset + bytes_written;

            if (opts->resume && ((size_t)(last_checkpoint % (16 * 1024 * 1024)) < bytes_written)) {
                resume_save_checkpoint(journal_path, &st, last_checkpoint, 0, RESUME_STATUS_IN_PROGRESS);
            }
        }
    }

    /* Drain any remaining completions */
    io_uring_submit(&ring);
    while (in_flight > 0) {
        struct io_uring_cqe *cqe = NULL;
        if (io_uring_wait_cqe(&ring, &cqe) == 0 && cqe) {
            uring_task_t *t = (uring_task_t *)io_uring_cqe_get_data(cqe);
            if (t && cqe->res > 0 && t->op_type == OP_TYPE_WRITE) {
                atomic_fetch_add(&g_stats.total_bytes_written, cqe->res);
                if (prog) progress_update(prog, (off_t)cqe->res, 0);
            }
            io_uring_cqe_seen(&ring, cqe);
            in_flight--;
        } else {
            break;
        }
    }

    if (ftruncate(dest_fd, st.st_size) != 0) {
        /* Handled */
    }

    /* Free resources */
    for (size_t i = 0; i < q_depth; ++i) {
        free(slots[i].buf);
    }
    free(slots);
    io_uring_queue_exit(&ring);

    if (g_stop_requested) {
        if (opts->resume) {
            fdatasync(dest_fd);
            resume_save_checkpoint(journal_path, &st, last_checkpoint, 0, RESUME_STATUS_INTERRUPTED);
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
                printf("[Verified io_uring] %s: %s\n", dest_path, dest_hash);
            }
        }
    }

    /* Metadata preservation */
    if (opts->preserve_metadata) {
        metadata_preserve_fd(src_fd, dest_fd, &st);
    }

    close(src_fd);
    close(dest_fd);

    if (opts->resume) {
        resume_cleanup_journal(journal_path);
    }

    atomic_fetch_add(&g_stats.total_files_copied, 1);
    return 0;
}
