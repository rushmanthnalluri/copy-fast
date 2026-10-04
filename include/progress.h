#ifndef COPYFAST_PROGRESS_H
#define COPYFAST_PROGRESS_H

#include "copyfast.h"

typedef struct {
    off_t                 total_bytes;
    atomic_int_least64_t  current_bytes;
    atomic_int_least64_t  sparse_skipped;
    const char           *current_filename;
    struct timespec start_time;
    struct timespec last_update_time;
    off_t       last_bytes;
    double      instant_speed_mbps;
    bool        is_tty;
    bool        is_quiet;
    pthread_t   thread;
    bool        thread_running;
} progress_meter_t;

progress_meter_t *progress_create(off_t total_bytes, bool quiet);
void              progress_destroy(progress_meter_t *prog);
void              progress_set_file(progress_meter_t *prog, const char *filename);
void              progress_update(progress_meter_t *prog, off_t bytes_added, off_t sparse_skipped);
void              progress_finish(progress_meter_t *prog);

#endif /* COPYFAST_PROGRESS_H */
