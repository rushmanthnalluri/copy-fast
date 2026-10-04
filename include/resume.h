#ifndef COPYFAST_RESUME_H
#define COPYFAST_RESUME_H

#include "copyfast.h"

#define RESUME_MAGIC   0x52465043 /* 'C' 'P' 'F' 'R' */
#define RESUME_VERSION 1

typedef enum {
    RESUME_STATUS_IN_PROGRESS = 1,
    RESUME_STATUS_INTERRUPTED = 2,
    RESUME_STATUS_COMPLETED   = 3
} resume_status_t;

typedef struct {
    uint32_t magic;
    uint32_t version;
    dev_t    src_dev;
    ino_t    src_ino;
    off_t    src_size;
    int64_t  src_mtime_sec;
    int64_t  src_mtime_nsec;
    off_t    copied_offset;
    uint64_t partial_hash;
    uint32_t status;
    uint32_t crc32;
} resume_record_t;

/* Generate standard journal path for a destination file */
int  resume_get_journal_path(const char *dest_path, char *out_path, size_t out_path_size);

/* Read and validate an existing resume journal */
int  resume_read_journal(const char *journal_path, const struct stat *src_st, resume_record_t *out_rec);

/* Save / checkpoint progress to journal file atomically */
int  resume_save_checkpoint(const char *journal_path, const struct stat *src_st,
                            off_t copied_offset, uint64_t partial_hash, resume_status_t status);

/* Remove journal file upon successful completion */
void resume_cleanup_journal(const char *journal_path);

#endif /* COPYFAST_RESUME_H */
