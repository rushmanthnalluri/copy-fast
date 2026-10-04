#ifndef COPYFAST_METADATA_H
#define COPYFAST_METADATA_H

#include "copyfast.h"

/* Copy metadata (permissions, ownership, nanosecond timestamps) from src to dest */
int metadata_preserve_fd(int src_fd, int dest_fd, const struct stat *src_st);
int metadata_preserve_path(const char *src_path, const char *dest_path, const struct stat *src_st);

/* Set file timestamps with nanosecond precision */
int metadata_set_timestamps(int fd, const struct timespec times[2]);

#endif /* COPYFAST_METADATA_H */
