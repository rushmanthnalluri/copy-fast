#include "metadata.h"

int metadata_preserve_fd(int src_fd, int dest_fd, const struct stat *src_st) {
    struct stat st;
    if (!src_st) {
        if (fstat(src_fd, &st) != 0) return -1;
        src_st = &st;
    }

    int result = 0;

    /* 1. Mode / Permissions */
    mode_t mode = src_st->st_mode & 07777;
    if (fchmod(dest_fd, mode) != 0) {
        result = -1;
    }

    /* 2. Ownership (UID / GID) - ignore EPERM if non-root */
    if (fchown(dest_fd, src_st->st_uid, src_st->st_gid) != 0) {
        if (errno != EPERM) {
            result = -1;
        }
    }

    /* 3. Nanosecond Timestamps (atime & mtime) */
    struct timespec times[2];
    times[0] = src_st->st_atim;
    times[1] = src_st->st_mtim;

    if (futimens(dest_fd, times) != 0) {
        result = -1;
    }

    return result;
}

int metadata_preserve_path(const char *src_path, const char *dest_path, const struct stat *src_st) {
    struct stat st;
    if (!src_st) {
        if (stat(src_path, &st) != 0) return -1;
        src_st = &st;
    }

    int result = 0;

    /* Mode */
    mode_t mode = src_st->st_mode & 07777;
    if (chmod(dest_path, mode) != 0) {
        result = -1;
    }

    /* Ownership */
    if (chown(dest_path, src_st->st_uid, src_st->st_gid) != 0) {
        if (errno != EPERM) {
            result = -1;
        }
    }

    /* Timestamps */
    struct timespec times[2];
    times[0] = src_st->st_atim;
    times[1] = src_st->st_mtim;

    if (utimensat(AT_FDCWD, dest_path, times, AT_SYMLINK_NOFOLLOW) != 0) {
        result = -1;
    }

    return result;
}

int metadata_set_timestamps(int fd, const struct timespec times[2]) {
    return futimens(fd, times);
}
