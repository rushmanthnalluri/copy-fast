#include "copyfast.h"
#include <ctype.h>

copy_stats_t g_stats;

void copyfast_get_time(struct timespec *ts) {
    clock_gettime(CLOCK_MONOTONIC, ts);
}

double copyfast_get_elapsed_sec(const struct timespec *start, const struct timespec *end) {
    double sec = (double)(end->tv_sec - start->tv_sec);
    double nsec = (double)(end->tv_nsec - start->tv_nsec);
    return sec + nsec / 1e9;
}

int copyfast_parse_size(const char *str, size_t *out_size) {
    if (!str || !*str || !out_size) return -1;

    char *endptr = NULL;
    errno = 0;
    unsigned long long val = strtoull(str, &endptr, 10);
    if (errno != 0) return -1;

    if (*endptr != '\0') {
        char unit = (char)toupper((unsigned char)*endptr);
        const char *rem = endptr + 1;
        if (*rem == 'B' || *rem == 'b') rem++;
        if (*rem != '\0') return -1;

        if (unit == 'K') {
            val *= 1024ULL;
        } else if (unit == 'M') {
            val *= (1024ULL * 1024ULL);
        } else if (unit == 'G') {
            val *= (1024ULL * 1024ULL * 1024ULL);
        } else if (unit == 'B') {
            /* val unchanged */
        } else {
            return -1;
        }
    }

    *out_size = (size_t)val;
    return 0;
}

int copyfast_get_basename(const char *path, char *out_base, size_t out_size) {
    if (!path || !out_base || out_size == 0) return -1;
    if (!*path) {
        snprintf(out_base, out_size, ".");
        return 0;
    }

    size_t len = strlen(path);
    while (len > 1 && path[len - 1] == '/') {
        len--;
    }

    if (len == 1 && path[0] == '/') {
        snprintf(out_base, out_size, "/");
        return 0;
    }

    size_t start = 0;
    for (size_t i = len; i > 0; --i) {
        if (path[i - 1] == '/') {
            start = i;
            break;
        }
    }

    size_t base_len = len - start;
    if (base_len >= out_size) return -1;

    memcpy(out_base, path + start, base_len);
    out_base[base_len] = '\0';
    return 0;
}
