#include "progress.h"
#include <sys/ioctl.h>

static void format_size(off_t bytes, char *buf, size_t buf_len) {
    if (bytes >= (off_t)(1024ULL * 1024ULL * 1024ULL)) {
        snprintf(buf, buf_len, "%.2f GiB", (double)bytes / (1024.0 * 1024.0 * 1024.0));
    } else if (bytes >= (off_t)(1024ULL * 1024ULL)) {
        snprintf(buf, buf_len, "%.2f MiB", (double)bytes / (1024.0 * 1024.0));
    } else if (bytes >= (off_t)1024ULL) {
        snprintf(buf, buf_len, "%.2f KiB", (double)bytes / 1024.0);
    } else {
        snprintf(buf, buf_len, "%ld B", (long)bytes);
    }
}

static void render_progress_line(progress_meter_t *prog) {
    if (prog->is_quiet) return;

    off_t cur = atomic_load(&prog->current_bytes);
    off_t total = prog->total_bytes;
    double pct = (total > 0) ? ((double)cur / (double)total) * 100.0 : 100.0;
    if (pct > 100.0) pct = 100.0;

    struct timespec now;
    copyfast_get_time(&now);
    double elapsed = copyfast_get_elapsed_sec(&prog->start_time, &now);

    double mbps = (elapsed > 0.05) ? ((double)cur / (1024.0 * 1024.0)) / elapsed : 0.0;

    char cur_str[32], tot_str[32];
    format_size(cur, cur_str, sizeof(cur_str));
    format_size(total, tot_str, sizeof(tot_str));

    int eta_sec = 0;
    if (mbps > 0.01 && cur < total) {
        double rem_mb = (double)(total - cur) / (1024.0 * 1024.0);
        eta_sec = (int)(rem_mb / mbps);
    }

    if (prog->is_tty) {
        struct winsize ws;
        int cols = 80;
        if (ioctl(STDERR_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 20) {
            cols = ws.ws_col;
        }

        /* Progress bar */
        int bar_width = cols - 50;
        if (bar_width < 10) bar_width = 10;
        if (bar_width > 30) bar_width = 30;

        int filled = (int)((pct / 100.0) * bar_width);
        char bar[64];
        for (int i = 0; i < bar_width; ++i) {
            if (i < filled) bar[i] = '=';
            else if (i == filled) bar[i] = '>';
            else bar[i] = ' ';
        }
        bar[bar_width] = '\0';

        fprintf(stderr, "\r\033[K[%s] %5.1f%% %s/%s [%6.1f MB/s] ETA %02d:%02d",
                bar, pct, cur_str, tot_str, mbps, eta_sec / 60, eta_sec % 60);
        fflush(stderr);
    }
}

static void *progress_thread_fn(void *arg) {
    progress_meter_t *prog = (progress_meter_t *)arg;
    struct timespec req = {0, 100000000L}; /* 100ms interval */

    while (prog->thread_running && !g_stop_requested) {
        render_progress_line(prog);
        nanosleep(&req, NULL);
    }
    return NULL;
}

progress_meter_t *progress_create(off_t total_bytes, bool quiet) {
    progress_meter_t *prog = (progress_meter_t *)calloc(1, sizeof(progress_meter_t));
    if (!prog) return NULL;

    prog->total_bytes = total_bytes;
    atomic_init(&prog->current_bytes, 0);
    atomic_init(&prog->sparse_skipped, 0);
    prog->is_quiet = quiet;
    prog->is_tty = isatty(STDERR_FILENO);
    copyfast_get_time(&prog->start_time);
    prog->last_update_time = prog->start_time;

    if (!prog->is_quiet && prog->is_tty) {
        prog->thread_running = true;
        pthread_create(&prog->thread, NULL, progress_thread_fn, prog);
    }

    return prog;
}

void progress_set_file(progress_meter_t *prog, const char *filename) {
    if (!prog) return;
    prog->current_filename = filename;
}

void progress_update(progress_meter_t *prog, off_t bytes_added, off_t sparse_skipped) {
    if (!prog) return;
    atomic_fetch_add(&prog->current_bytes, bytes_added);
    if (sparse_skipped > 0) {
        atomic_fetch_add(&prog->sparse_skipped, sparse_skipped);
    }
}

void progress_finish(progress_meter_t *prog) {
    if (!prog || prog->is_finished) return;
    prog->is_finished = true;
    if (prog->thread_running) {
        prog->thread_running = false;
        pthread_join(prog->thread, NULL);
    }
    render_progress_line(prog);
    if (!prog->is_quiet && prog->is_tty) {
        fprintf(stderr, "\n");
    }
}

void progress_destroy(progress_meter_t *prog) {
    if (!prog) return;
    progress_finish(prog);
    free(prog);
}
