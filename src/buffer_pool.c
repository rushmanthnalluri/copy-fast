#include "buffer_pool.h"

buffer_pool_t *buffer_pool_create(size_t queue_depth, size_t buffer_size) {
    if (queue_depth == 0 || buffer_size == 0) return NULL;

    buffer_pool_t *pool = (buffer_pool_t *)calloc(1, sizeof(buffer_pool_t));
    if (!pool) return NULL;

    pool->queue_depth = queue_depth;
    pool->buffer_size = buffer_size;
    pool->slots = (buffer_slot_t *)calloc(queue_depth, sizeof(buffer_slot_t));
    if (!pool->slots) {
        free(pool);
        return NULL;
    }

    for (size_t i = 0; i < queue_depth; ++i) {
        /* Allocate 4KB-aligned buffer memory */
        void *buf = NULL;
        int ret = posix_memalign(&buf, 4096, buffer_size);
        if (ret != 0 || !buf) {
            /* Cleanup previously allocated buffers on failure */
            for (size_t j = 0; j < i; ++j) {
                free(pool->slots[j].data);
            }
            free(pool->slots);
            free(pool);
            return NULL;
        }
        pool->slots[i].data = buf;
        pool->slots[i].capacity = buffer_size;
        pool->slots[i].len = 0;
        pool->slots[i].offset = 0;
        pool->slots[i].is_hole = false;
        pool->slots[i].is_eof = false;
        pool->slots[i].error_code = 0;
    }

    pthread_mutex_init(&pool->mutex, NULL);
    pthread_cond_init(&pool->cond_not_empty, NULL);
    pthread_cond_init(&pool->cond_not_full, NULL);
    pool->is_closed = false;

    return pool;
}

void buffer_pool_close(buffer_pool_t *pool) {
    if (!pool) return;
    pthread_mutex_lock(&pool->mutex);
    pool->is_closed = true;
    pthread_cond_broadcast(&pool->cond_not_empty);
    pthread_cond_broadcast(&pool->cond_not_full);
    pthread_mutex_unlock(&pool->mutex);
}

void buffer_pool_destroy(buffer_pool_t *pool) {
    if (!pool) return;

    buffer_pool_close(pool);

    for (size_t i = 0; i < pool->queue_depth; ++i) {
        if (pool->slots[i].data) {
            free(pool->slots[i].data);
            pool->slots[i].data = NULL;
        }
    }

    pthread_mutex_destroy(&pool->mutex);
    pthread_cond_destroy(&pool->cond_not_empty);
    pthread_cond_destroy(&pool->cond_not_full);

    free(pool->slots);
    free(pool);
}

static inline void get_rel_timeout(struct timespec *ts, long ms) {
    clock_gettime(CLOCK_REALTIME, ts);
    ts->tv_nsec += ms * 1000000L;
    if (ts->tv_nsec >= 1000000000L) {
        ts->tv_sec += ts->tv_nsec / 1000000000L;
        ts->tv_nsec %= 1000000000L;
    }
}

buffer_slot_t *buffer_pool_acquire_write_slot(buffer_pool_t *pool) {
    if (!pool) return NULL;

    pthread_mutex_lock(&pool->mutex);

    /* Backpressure: wait while bounded queue is full, waking periodically to check stop flag */
    while (pool->count == pool->queue_depth && !pool->is_closed && !g_stop_requested) {
        struct timespec ts;
        get_rel_timeout(&ts, 100);
        pthread_cond_timedwait(&pool->cond_not_full, &pool->mutex, &ts);
    }

    if (pool->is_closed || g_stop_requested) {
        pthread_mutex_unlock(&pool->mutex);
        return NULL;
    }

    buffer_slot_t *slot = &pool->slots[pool->write_idx];
    slot->len = 0;
    slot->is_hole = false;
    slot->is_eof = false;
    slot->error_code = 0;

    pthread_mutex_unlock(&pool->mutex);
    return slot;
}

void buffer_pool_commit_write_slot(buffer_pool_t *pool, buffer_slot_t *slot) {
    if (!pool || !slot) return;

    pthread_mutex_lock(&pool->mutex);

    pool->write_idx = (pool->write_idx + 1) % pool->queue_depth;
    pool->count++;

    /* Signal consumer that a buffer slot is ready */
    pthread_cond_signal(&pool->cond_not_empty);

    pthread_mutex_unlock(&pool->mutex);
}

buffer_slot_t *buffer_pool_acquire_read_slot(buffer_pool_t *pool) {
    if (!pool) return NULL;

    pthread_mutex_lock(&pool->mutex);

    /* Consumer waits while bounded queue has no ready buffers */
    while (pool->count == 0 && !pool->is_closed && !g_stop_requested) {
        struct timespec ts;
        get_rel_timeout(&ts, 100);
        pthread_cond_timedwait(&pool->cond_not_empty, &pool->mutex, &ts);
    }

    if (pool->count == 0) {
        pthread_mutex_unlock(&pool->mutex);
        return NULL;
    }

    buffer_slot_t *slot = &pool->slots[pool->read_idx];
    pthread_mutex_unlock(&pool->mutex);
    return slot;
}

void buffer_pool_release_read_slot(buffer_pool_t *pool, buffer_slot_t *slot) {
    if (!pool || !slot) return;

    pthread_mutex_lock(&pool->mutex);

    pool->read_idx = (pool->read_idx + 1) % pool->queue_depth;
    pool->count--;

    /* Signal producer that a buffer slot has become free */
    pthread_cond_signal(&pool->cond_not_full);

    pthread_mutex_unlock(&pool->mutex);
}
