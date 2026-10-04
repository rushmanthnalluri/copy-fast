#ifndef COPYFAST_BACKENDS_H
#define COPYFAST_BACKENDS_H

#include "copyfast.h"
#include "progress.h"

int copy_file_naive(const char *src_path, const char *dest_path, const copy_options_t *opts, progress_meter_t *prog);
int copy_file_pipeline(const char *src_path, const char *dest_path, const copy_options_t *opts, progress_meter_t *prog);
int copy_file_uring(const char *src_path, const char *dest_path, const copy_options_t *opts, progress_meter_t *prog);

/* Universal copy dispatcher that selects configured backend */
int copy_file_dispatch(const char *src_path, const char *dest_path, const copy_options_t *opts, progress_meter_t *prog);

#endif /* COPYFAST_BACKENDS_H */
