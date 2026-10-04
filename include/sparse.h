#ifndef COPYFAST_SPARSE_H
#define COPYFAST_SPARSE_H

#include "copyfast.h"

typedef struct {
    off_t offset;
    off_t length;
    bool  is_hole;
} file_extent_t;

/* Detect if filesystem supports SEEK_HOLE / SEEK_DATA */
bool sparse_is_supported(int fd);

/* Get next extent (data or hole) from file starting at *curr_offset.
 * Returns 1 if extent found, 0 on EOF, -1 on error. */
int sparse_get_next_extent(int fd, off_t total_size, off_t *curr_offset, file_extent_t *extent);

/* Check if an in-memory buffer is composed entirely of zeros */
bool sparse_is_buffer_zero(const void *buf, size_t len);

/* Punch a hole in destination file if supported (FALLOC_FL_PUNCH_HOLE) */
int sparse_punch_hole(int fd, off_t offset, off_t len);

#endif /* COPYFAST_SPARSE_H */
