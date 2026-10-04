#ifndef COPYFAST_WORK_QUEUE_H
#define COPYFAST_WORK_QUEUE_H

#include "copyfast.h"
#include "progress.h"

typedef struct work_item {
    char *src_path;
    char *dest_path;
    struct work_item *next;
} work_item_t;

typedef struct {
    work_item_t    *head;
    work_item_t    *tail;
    size_t          count;
    size_t          capacity;
    pthread_mutex_t mutex;
    pthread_cond_t  cond_not_empty;
    pthread_cond_t  cond_not_full;
    bool            done;
} work_queue_t;

work_queue_t *work_queue_create(size_t capacity);
void          work_queue_destroy(work_queue_t *q);
void          work_queue_push(work_queue_t *q, const char *src, const char *dest);
work_item_t  *work_queue_pop(work_queue_t *q);
void          work_queue_set_done(work_queue_t *q);

/* Copy an entire directory recursively using parallel worker pool */
int copy_directory_recursive(const char *src_dir, const char *dest_dir,
                            const copy_options_t *opts, progress_meter_t *prog);

#endif /* COPYFAST_WORK_QUEUE_H */
