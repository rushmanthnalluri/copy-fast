#include "sparse.h"

#ifndef SEEK_DATA
#define SEEK_DATA 3
#endif
#ifndef SEEK_HOLE
#define SEEK_HOLE 4
#endif

bool sparse_is_supported(int fd) {
    off_t off = lseek(fd, 0, SEEK_HOLE);
    if (off == (off_t)-1 && (errno == EINVAL || errno == ENOTTY)) {
        return false;
    }
    /* Reset position */
    lseek(fd, 0, SEEK_SET);
    return true;
}

int sparse_get_next_extent(int fd, off_t total_size, off_t *curr_offset, file_extent_t *extent) {
    if (!extent || !curr_offset) return -1;
    if (*curr_offset >= total_size) return 0; /* EOF */

    off_t pos = *curr_offset;

    /* Check where the next hole is relative to current pos */
    off_t hole_pos = lseek(fd, pos, SEEK_HOLE);
    if (hole_pos == (off_t)-1) {
        /* If SEEK_HOLE not supported or error */
        if (errno == ENXIO) {
            /* Pos is at or past EOF */
            return 0;
        }
        /* Fallback: treat entire rest of file as one data extent */
        extent->offset = pos;
        extent->length = total_size - pos;
        extent->is_hole = false;
        *curr_offset = total_size;
        return 1;
    }

    if (hole_pos == pos) {
        /* Currently at the start of a hole! Find where the next data extent begins */
        off_t data_pos = lseek(fd, pos, SEEK_DATA);
        if (data_pos == (off_t)-1) {
            if (errno == ENXIO) {
                /* Hole extends all the way to EOF */
                extent->offset = pos;
                extent->length = total_size - pos;
                extent->is_hole = true;
                *curr_offset = total_size;
                return 1;
            }
            return -1;
        }

        if (data_pos > total_size) {
            data_pos = total_size;
        }

        extent->offset = pos;
        extent->length = data_pos - pos;
        extent->is_hole = true;
        *curr_offset = data_pos;
        return 1;
    } else {
        /* Currently inside a data extent. It runs from pos up to hole_pos */
        if (hole_pos > total_size) {
            hole_pos = total_size;
        }
        extent->offset = pos;
        extent->length = hole_pos - pos;
        extent->is_hole = false;
        *curr_offset = hole_pos;
        return 1;
    }
}

bool sparse_is_buffer_zero(const void *buf, size_t len) {
    const uint8_t *p = (const uint8_t *)buf;

    /* Check 64-bit words for speed */
    while (len >= 8 && ((uintptr_t)p & 7) != 0) {
        if (*p++ != 0) return false;
        len--;
    }

    const uint64_t *p64 = (const uint64_t *)(const void *)p;
    while (len >= 8) {
        if (*p64++ != 0) return false;
        len -= 8;
    }

    p = (const uint8_t *)p64;
    while (len > 0) {
        if (*p++ != 0) return false;
        len--;
    }

    return true;
}

int sparse_punch_hole(int fd, off_t offset, off_t len) {
#if defined(FALLOC_FL_PUNCH_HOLE) && defined(FALLOC_FL_KEEP_SIZE)
    return fallocate(fd, FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE, offset, len);
#else
    (void)fd; (void)offset; (void)len;
    return -1;
#endif
}
