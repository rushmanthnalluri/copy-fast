#include "work_queue.h"
#include "backends.h"
#include "metadata.h"
#include "signals.h"
#include <dirent.h>

typedef struct dir_meta_node {
    char *path;
    struct timespec times[2];
    mode_t mode;
    struct dir_meta_node *next;
} dir_meta_node_t;

work_queue_t *work_queue_create(size_t capacity) {
    work_queue_t *q = (work_queue_t *)calloc(1, sizeof(work_queue_t));
    if (!q) return NULL;

    q->capacity = capacity > 0 ? capacity : 1024;
    pthread_mutex_init(&q->mutex, NULL);
    pthread_cond_init(&q->cond_not_empty, NULL);
    pthread_cond_init(&q->cond_not_full, NULL);
    q->done = false;

    return q;
}

void work_queue_destroy(work_queue_t *q) {
    if (!q) return;

    pthread_mutex_lock(&q->mutex);
    work_item_t *curr = q->head;
    while (curr) {
        work_item_t *next = curr->next;
        free(curr->src_path);
        free(curr->dest_path);
        free(curr);
        curr = next;
    }
    pthread_mutex_unlock(&q->mutex);

    pthread_mutex_destroy(&q->mutex);
    pthread_cond_destroy(&q->cond_not_empty);
    pthread_cond_destroy(&q->cond_not_full);
    free(q);
}

static inline void get_rel_timeout(struct timespec *ts, long ms) {
    clock_gettime(CLOCK_REALTIME, ts);
    ts->tv_nsec += ms * 1000000L;
    if (ts->tv_nsec >= 1000000000L) {
        ts->tv_sec += ts->tv_nsec / 1000000000L;
        ts->tv_nsec %= 1000000000L;
    }
}

void work_queue_push(work_queue_t *q, const char *src, const char *dest) {
    if (!q || !src || !dest) return;

    work_item_t *item = (work_item_t *)calloc(1, sizeof(work_item_t));
    if (!item) return;

    item->src_path = strdup(src);
    item->dest_path = strdup(dest);

    pthread_mutex_lock(&q->mutex);

    /* Backpressure on scanner if queue reaches limit */
    while (q->count >= q->capacity && !q->done && !g_stop_requested) {
        struct timespec ts;
        get_rel_timeout(&ts, 100);
        pthread_cond_timedwait(&q->cond_not_full, &q->mutex, &ts);
    }

    if (q->done || g_stop_requested) {
        free(item->src_path);
        free(item->dest_path);
        free(item);
        pthread_mutex_unlock(&q->mutex);
        return;
    }

    if (q->tail) {
        q->tail->next = item;
        q->tail = item;
    } else {
        q->head = q->tail = item;
    }
    q->count++;

    pthread_cond_signal(&q->cond_not_empty);
    pthread_mutex_unlock(&q->mutex);
}

work_item_t *work_queue_pop(work_queue_t *q) {
    if (!q) return NULL;

    pthread_mutex_lock(&q->mutex);

    while (q->count == 0 && !q->done && !g_stop_requested) {
        struct timespec ts;
        get_rel_timeout(&ts, 100);
        pthread_cond_timedwait(&q->cond_not_empty, &q->mutex, &ts);
    }

    if (q->count == 0) {
        pthread_mutex_unlock(&q->mutex);
        return NULL;
    }

    work_item_t *item = q->head;
    q->head = item->next;
    if (!q->head) {
        q->tail = NULL;
    }
    q->count--;

    pthread_cond_signal(&q->cond_not_full);
    pthread_mutex_unlock(&q->mutex);

    return item;
}

void work_queue_set_done(work_queue_t *q) {
    if (!q) return;

    pthread_mutex_lock(&q->mutex);
    q->done = true;
    pthread_cond_broadcast(&q->cond_not_empty);
    pthread_cond_broadcast(&q->cond_not_full);
    pthread_mutex_unlock(&q->mutex);
}

typedef struct {
    work_queue_t        *queue;
    const copy_options_t *opts;
    progress_meter_t    *prog;
    int                  worker_id;
} worker_thread_arg_t;

static void *parallel_worker_fn(void *arg) {
    worker_thread_arg_t *warg = (worker_thread_arg_t *)arg;
    signals_block_in_thread();

    while (!g_stop_requested) {
        work_item_t *item = work_queue_pop(warg->queue);
        if (!item) break;

        if (warg->opts->verbose) {
            printf("[Worker %d] Copying '%s' -> '%s'\n", warg->worker_id, item->src_path, item->dest_path);
        }

        copy_file_dispatch(item->src_path, item->dest_path, warg->opts, warg->prog);

        free(item->src_path);
        free(item->dest_path);
        free(item);
    }

    return NULL;
}

static void scan_directory_tree(const char *src_dir, const char *dest_dir,
                                work_queue_t *q, dir_meta_node_t **meta_list,
                                const copy_options_t *opts) {
    DIR *d = opendir(src_dir);
    if (!d) {
        fprintf(stderr, "Cannot open directory '%s': %s\n", src_dir, strerror(errno));
        return;
    }

    struct stat dir_st;
    if (stat(src_dir, &dir_st) == 0) {
        /* Ensure destination directory exists */
        mkdir(dest_dir, dir_st.st_mode & 07777);

        /* Save directory metadata for post-order timestamp restoration */
        if (opts->preserve_metadata) {
            dir_meta_node_t *node = (dir_meta_node_t *)malloc(sizeof(dir_meta_node_t));
            if (node) {
                node->path = strdup(dest_dir);
                node->times[0] = dir_st.st_atim;
                node->times[1] = dir_st.st_mtim;
                node->mode = dir_st.st_mode & 07777;
                node->next = *meta_list;
                *meta_list = node;
            }
        }
    }

    struct dirent *entry;
    while ((entry = readdir(d)) != NULL && !g_stop_requested) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        char src_sub[2048];
        char dest_sub[2048];
        snprintf(src_sub, sizeof(src_sub), "%s/%s", src_dir, entry->d_name);
        snprintf(dest_sub, sizeof(dest_sub), "%s/%s", dest_dir, entry->d_name);

        struct stat st;
        if (lstat(src_sub, &st) != 0) {
            continue;
        }

        if (S_ISDIR(st.st_mode)) {
            scan_directory_tree(src_sub, dest_sub, q, meta_list, opts);
        } else {
            /* File, symlink, etc. Add to work queue */
            atomic_fetch_add(&g_stats.total_bytes_expected, st.st_size);
            work_queue_push(q, src_sub, dest_sub);
        }
    }

    closedir(d);
}

int copy_directory_recursive(const char *src_dir, const char *dest_dir,
                            const copy_options_t *opts, progress_meter_t *prog) {
    struct stat src_st;
    if (stat(src_dir, &src_st) != 0 || !S_ISDIR(src_st.st_mode)) {
        fprintf(stderr, "'%s' is not a directory\n", src_dir);
        return -1;
    }

    /* Create destination top-level directory */
    mkdir(dest_dir, src_st.st_mode & 07777);

    work_queue_t *q = work_queue_create(1024);
    if (!q) return -1;

    int num_workers = opts->jobs > 0 ? opts->jobs : COPYFAST_DEFAULT_JOBS;
    pthread_t *threads = (pthread_t *)calloc((size_t)num_workers, sizeof(pthread_t));
    worker_thread_arg_t *args = (worker_thread_arg_t *)calloc((size_t)num_workers, sizeof(worker_thread_arg_t));

    for (int i = 0; i < num_workers; ++i) {
        args[i].queue = q;
        args[i].opts = opts;
        args[i].prog = prog;
        args[i].worker_id = i + 1;
        pthread_create(&threads[i], NULL, parallel_worker_fn, &args[i]);
    }

    dir_meta_node_t *dir_meta_list = NULL;

    /* Scan directory tree and push jobs to queue */
    scan_directory_tree(src_dir, dest_dir, q, &dir_meta_list, opts);

    /* Signal completion of directory scanning */
    work_queue_set_done(q);

    /* Wait for all worker threads to finish copying */
    for (int i = 0; i < num_workers; ++i) {
        pthread_join(threads[i], NULL);
    }

    /* Post-order restore directory timestamps and modes */
    dir_meta_node_t *curr_meta = dir_meta_list;
    while (curr_meta) {
        dir_meta_node_t *next_meta = curr_meta->next;
        chmod(curr_meta->path, curr_meta->mode);
        utimensat(AT_FDCWD, curr_meta->path, curr_meta->times, 0);
        free(curr_meta->path);
        free(curr_meta);
        curr_meta = next_meta;
    }

    free(threads);
    free(args);
    work_queue_destroy(q);

    return 0;
}
