#ifndef COPYFAST_BUFFER_POOL_H
#define COPYFAST_BUFFER_POOL_H

#include "copyfast.h"

typedef struct {
    void   *data;
    size_t  capacity;
    size_t  len;
    off_t   offset;
    bool    is_hole;
    bool    is_eof;
    int     error_code;
} buffer_slot_t;

typedef struct {
    buffer_slot_t  *slots;
    size_t          queue_depth;
    size_t          buffer_size;

    /* Circular queue indexing */
    size_t          read_idx;   /* Consumer index */
    size_t          write_idx;  /* Producer index */
    size_t          count;      /* Number of filled slots ready for consumer */

    /* Concurrency control */
    pthread_mutex_t mutex;
    pthread_cond_t  cond_not_empty; /* Signaled when slot is filled */
    pthread_cond_t  cond_not_full;  /* Signaled when slot is consumed */

    bool            is_closed;      /* Set if stream terminated or error occurred */
} buffer_pool_t;

/* Lifecycle */
buffer_pool_t *buffer_pool_create(size_t queue_depth, size_t buffer_size);
void           buffer_pool_destroy(buffer_pool_t *pool);
void           buffer_pool_close(buffer_pool_t *pool);

/* Producer operations (Reader thread) */
buffer_slot_t *buffer_pool_acquire_write_slot(buffer_pool_t *pool);
void           buffer_pool_commit_write_slot(buffer_pool_t *pool, buffer_slot_t *slot);

/* Consumer operations (Writer thread) */
buffer_slot_t *buffer_pool_acquire_read_slot(buffer_pool_t *pool);
void           buffer_pool_release_read_slot(buffer_pool_t *pool, buffer_slot_t *slot);

#endif /* COPYFAST_BUFFER_POOL_H */
