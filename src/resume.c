#include "resume.h"
#include <stddef.h>

/* Simple CRC32 for journal integrity */
static uint32_t calc_crc32(const uint8_t *data, size_t len) {
    uint32_t crc = 0xFFFFFFFF;
    for (size_t i = 0; i < len; ++i) {
        crc ^= data[i];
        for (int j = 0; j < 8; ++j) {
            crc = (crc >> 1) ^ (0xEDB88320 & (-(int)(crc & 1)));
        }
    }
    return ~crc;
}

int resume_get_journal_path(const char *dest_path, char *out_path, size_t out_path_size) {
    if (!dest_path || !out_path) return -1;
    int written = snprintf(out_path, out_path_size, "%s%s", dest_path, COPYFAST_RESUME_SUFFIX);
    if (written < 0 || (size_t)written >= out_path_size) return -1;
    return 0;
}

int resume_read_journal(const char *journal_path, const struct stat *src_st, resume_record_t *out_rec) {
    if (!journal_path || !src_st || !out_rec) return -1;

    int fd = open(journal_path, O_RDONLY);
    if (fd < 0) return -1;

    resume_record_t rec;
    ssize_t n = read(fd, &rec, sizeof(rec));
    close(fd);

    if (n != sizeof(rec)) return -1;

    /* Verify Magic and Version */
    if (rec.magic != RESUME_MAGIC || rec.version != RESUME_VERSION) {
        return -1;
    }

    /* Verify CRC32 of fields preceding crc32 member */
    size_t payload_len = offsetof(resume_record_t, crc32);
    uint32_t expected_crc = calc_crc32((const uint8_t *)&rec, payload_len);
    if (expected_crc != rec.crc32) {
        return -1; /* Corrupted journal */
    }

    /* Verify source file match */
    if (rec.src_dev != src_st->st_dev ||
        rec.src_ino != src_st->st_ino ||
        rec.src_size != src_st->st_size ||
        rec.src_mtime_sec != src_st->st_mtim.tv_sec ||
        rec.src_mtime_nsec != src_st->st_mtim.tv_nsec) {
        return -2; /* Source modified or mismatched */
    }

    /* Validate offset */
    if (rec.copied_offset > src_st->st_size) {
        return -1;
    }

    *out_rec = rec;
    return 0;
}

int resume_save_checkpoint(const char *journal_path, const struct stat *src_st,
                            off_t copied_offset, uint64_t partial_hash, resume_status_t status) {
    if (!journal_path || !src_st) return -1;

    resume_record_t rec;
    memset(&rec, 0, sizeof(rec));

    rec.magic = RESUME_MAGIC;
    rec.version = RESUME_VERSION;
    rec.src_dev = src_st->st_dev;
    rec.src_ino = src_st->st_ino;
    rec.src_size = src_st->st_size;
    rec.src_mtime_sec = (int64_t)src_st->st_mtim.tv_sec;
    rec.src_mtime_nsec = (int64_t)src_st->st_mtim.tv_nsec;
    rec.copied_offset = copied_offset;
    rec.partial_hash = partial_hash;
    rec.status = (uint32_t)status;

    size_t payload_len = offsetof(resume_record_t, crc32);
    rec.crc32 = calc_crc32((const uint8_t *)&rec, payload_len);

    /* Atomic journal write: write to temp file then rename */
    char tmp_path[1024];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp.%d", journal_path, (int)getpid());

    int fd = open(tmp_path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) return -1;

    ssize_t n = write(fd, &rec, sizeof(rec));
    if (n != sizeof(rec)) {
        close(fd);
        unlink(tmp_path);
        return -1;
    }

    /* Flush journal to disk before atomic rename */
    fdatasync(fd);
    close(fd);

    if (rename(tmp_path, journal_path) != 0) {
        unlink(tmp_path);
        return -1;
    }

    return 0;
}

void resume_cleanup_journal(const char *journal_path) {
    if (journal_path) {
        unlink(journal_path);
    }
}
