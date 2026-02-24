/*
 * progress.c - General-purpose progress bar library
 * Include this file directly in your .c files (single-header style).
 * Requires: platform.c (must be included before this file)
 *
 * Features:
 *   - Customizable fields (percentage, counts, rate, ETA, custom text)
 *   - Renders at the bottom of the terminal without disrupting scrollback
 *   - Thread-safe updates
 *   - Disappears cleanly when done
 */

#ifndef PROGRESS_C
#define PROGRESS_C

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Requires platform.c to be included first for threads, mutex, terminal width, time */

/* ── Configuration ────────────────────────────────────────────────── */

#define PROG_MAX_FIELDS   8
#define PROG_BAR_MIN_W   10
#define PROG_UPDATE_HZ   15   /* max redraws per second */

/* Field types that can be shown on the progress bar */
typedef enum {
    PROG_FIELD_NONE = 0,
    PROG_FIELD_PERCENT,       /* 42% */
    PROG_FIELD_COUNT,         /* 123/456 */
    PROG_FIELD_FILE_RATE,     /* 42 files/s */
    PROG_FIELD_DATA_RATE,     /* 12.3 MB/s */
    PROG_FIELD_ETA,           /* ETA 01:23 */
    PROG_FIELD_CUSTOM,        /* user-provided string */
    PROG_FIELD_BAR,           /* [████░░░░░░] */
} prog_field_type;

typedef struct {
    prog_field_type type;
    char            custom[64]; /* for PROG_FIELD_CUSTOM */
} prog_field_t;

typedef struct {
    /* Configuration (set before prog_start) */
    long long       total_files;
    long long       total_bytes;
    prog_field_t    fields[PROG_MAX_FIELDS];
    int             field_count;
    int             print_paths;    /* if true, prints relative file paths above bar */

    /* Runtime state (updated via prog_update / prog_advance) */
    long long       done_files;
    long long       done_bytes;
    double          start_time;
    double          last_draw;
    int             finished;
    int             bar_lines;      /* how many lines the bar occupies (always 1) */
    int             started;

    plat_mutex_t    mtx;
} prog_bar_t;

/* ── Internal helpers ─────────────────────────────────────────────── */

static void prog_format_bytes_rate(double bytes_per_sec, char *buf, size_t len) {
    if (bytes_per_sec < 1024.0)
        snprintf(buf, len, "%.0f B/s", bytes_per_sec);
    else if (bytes_per_sec < 1024.0*1024)
        snprintf(buf, len, "%.1f KB/s", bytes_per_sec / 1024.0);
    else if (bytes_per_sec < 1024.0*1024*1024)
        snprintf(buf, len, "%.1f MB/s", bytes_per_sec / (1024.0*1024));
    else
        snprintf(buf, len, "%.2f GB/s", bytes_per_sec / (1024.0*1024*1024));
}

static void prog_format_eta(double seconds, char *buf, size_t len) {
    if (seconds < 0 || seconds > 359999) {
        snprintf(buf, len, "ETA --:--");
        return;
    }
    int s = (int)seconds;
    int h = s / 3600; s %= 3600;
    int m = s / 60;   s %= 60;
    if (h > 0)
        snprintf(buf, len, "ETA %d:%02d:%02d", h, m, s);
    else
        snprintf(buf, len, "ETA %02d:%02d", m, s);
}

static void prog_render_bar(int width, double frac, char *buf, size_t len) {
    (void)len;
    if (width < 3) { buf[0] = '\0'; return; }
    int inner = width - 2; /* subtract [ and ] */
    int filled = (int)(frac * inner + 0.5);
    if (filled > inner) filled = inner;
    if (filled < 0) filled = 0;

    buf[0] = '[';
    int i;
    for (i = 0; i < filled; i++) buf[1+i] = '#';
    for (; i < inner; i++)       buf[1+i] = '-';
    buf[1+inner] = ']';
    buf[2+inner] = '\0';
}

/* Move cursor to bottom of visible area and draw */
static void prog_draw(prog_bar_t *p) {
    double now = plat_time();
    if (!p->finished && (now - p->last_draw) < (1.0 / PROG_UPDATE_HZ))
        return;
    p->last_draw = now;

    double elapsed = now - p->start_time;
    double frac = 0;
    if (p->total_bytes > 0)
        frac = (double)p->done_bytes / p->total_bytes;
    else if (p->total_files > 0)
        frac = (double)p->done_files / p->total_files;
    if (frac > 1.0) frac = 1.0;

    double files_per_sec = elapsed > 0.01 ? p->done_files / elapsed : 0;
    double bytes_per_sec = elapsed > 0.01 ? p->done_bytes / elapsed : 0;

    double eta = -1;
    if (p->total_bytes > 0 && bytes_per_sec > 0)
        eta = (p->total_bytes - p->done_bytes) / bytes_per_sec;
    else if (p->total_files > 0 && files_per_sec > 0)
        eta = (p->total_files - p->done_files) / files_per_sec;

    int term_w = plat_terminal_width();

    /* Build each field's text */
    char parts[PROG_MAX_FIELDS][128];
    int  part_lens[PROG_MAX_FIELDS];
    int  bar_idx = -1;
    int  total_fixed = 0;

    for (int i = 0; i < p->field_count; i++) {
        parts[i][0] = '\0';
        switch (p->fields[i].type) {
            case PROG_FIELD_PERCENT:
                snprintf(parts[i], sizeof(parts[i]), "%3d%%", (int)(frac * 100));
                break;
            case PROG_FIELD_COUNT:
                snprintf(parts[i], sizeof(parts[i]), "%lld/%lld", p->done_files, p->total_files);
                break;
            case PROG_FIELD_FILE_RATE:
                snprintf(parts[i], sizeof(parts[i]), "%.1f files/s", files_per_sec);
                break;
            case PROG_FIELD_DATA_RATE:
                prog_format_bytes_rate(bytes_per_sec, parts[i], sizeof(parts[i]));
                break;
            case PROG_FIELD_ETA:
                prog_format_eta(eta, parts[i], sizeof(parts[i]));
                break;
            case PROG_FIELD_CUSTOM:
                snprintf(parts[i], sizeof(parts[i]), "%s", p->fields[i].custom);
                break;
            case PROG_FIELD_BAR:
                bar_idx = i;
                break;
            default: break;
        }
        if (p->fields[i].type != PROG_FIELD_BAR) {
            part_lens[i] = (int)strlen(parts[i]);
            total_fixed += part_lens[i];
        } else {
            part_lens[i] = 0;
        }
    }

    /* Compute bar width from remaining space */
    int separators = (p->field_count > 1) ? (p->field_count - 1) * 3 : 0; /* " | " */
    int bar_w = term_w - total_fixed - separators - 1;
    if (bar_w < PROG_BAR_MIN_W) bar_w = PROG_BAR_MIN_W;

    if (bar_idx >= 0) {
        prog_render_bar(bar_w, frac, parts[bar_idx], sizeof(parts[bar_idx]));
        part_lens[bar_idx] = (int)strlen(parts[bar_idx]);
    }

    /* Assemble line */
    char line[2048];
    line[0] = '\0';
    for (int i = 0; i < p->field_count; i++) {
        if (i > 0) strcat(line, " | ");
        strncat(line, parts[i], sizeof(line) - strlen(line) - 1);
    }

    /* Truncate to terminal width */
    int line_len = (int)strlen(line);
    if (line_len > term_w - 1) line[term_w - 1] = '\0';

    /* Save cursor, go to bottom, clear line, print, restore cursor */
    fprintf(stderr, "\033[s\033[%d;1H\033[2K%s\033[u", 9999, line);
    fflush(stderr);
}

/* ── Public API ───────────────────────────────────────────────────── */

static void prog_init(prog_bar_t *p) {
    memset(p, 0, sizeof(*p));
    plat_mutex_init(&p->mtx);
}

/* Add a field to the bar. Call before prog_start(). */
static void prog_add_field(prog_bar_t *p, prog_field_type type) {
    if (p->field_count >= PROG_MAX_FIELDS) return;
    p->fields[p->field_count].type = type;
    p->field_count++;
}

static void prog_add_custom(prog_bar_t *p, const char *text) {
    if (p->field_count >= PROG_MAX_FIELDS) return;
    p->fields[p->field_count].type = PROG_FIELD_CUSTOM;
    strncpy(p->fields[p->field_count].custom, text, 63);
    p->field_count++;
}

static void prog_set_custom(prog_bar_t *p, int field_idx, const char *text) {
    if (field_idx < 0 || field_idx >= p->field_count) return;
    if (p->fields[field_idx].type != PROG_FIELD_CUSTOM) return;
    plat_mutex_lock(&p->mtx);
    strncpy(p->fields[field_idx].custom, text, 63);
    plat_mutex_unlock(&p->mtx);
}

static void prog_start(prog_bar_t *p, long long total_files, long long total_bytes) {
    p->total_files = total_files;
    p->total_bytes = total_bytes;
    p->done_files  = 0;
    p->done_bytes  = 0;
    p->finished    = 0;
    p->started     = 1;
    p->start_time  = plat_time();
    p->last_draw   = 0;

    /* Scroll up one line to make room at the bottom */
    fprintf(stderr, "\n\033[1A");
    fflush(stderr);
}

/* Print a file path above the progress bar (scrolls normally) */
static void prog_print_path(prog_bar_t *p, const char *path) {
    if (!p->print_paths) return;
    plat_mutex_lock(&p->mtx);
    /* Save cursor, move to bottom-1, scroll content up, print path, restore */
    fprintf(stderr, "\033[s\033[%d;1H\033[2K%s\033[u", 9999, path);
    fflush(stderr);
    /* Actually just print normally since the bar is at the very bottom */
    fprintf(stderr, "\033[s%s\n\033[u", path);
    fflush(stderr);
    plat_mutex_unlock(&p->mtx);
}

/* Advance the progress (thread-safe) */
static void prog_advance(prog_bar_t *p, int files, long long bytes) {
    plat_mutex_lock(&p->mtx);
    p->done_files += files;
    p->done_bytes += bytes;
    prog_draw(p);
    plat_mutex_unlock(&p->mtx);
}

/* Force a redraw */
static void prog_redraw(prog_bar_t *p) {
    plat_mutex_lock(&p->mtx);
    p->last_draw = 0;
    prog_draw(p);
    plat_mutex_unlock(&p->mtx);
}

/* Clear the bar and clean up */
static void prog_finish(prog_bar_t *p) {
    plat_mutex_lock(&p->mtx);
    p->finished = 1;
    /* Clear the bottom line */
    fprintf(stderr, "\033[s\033[%d;1H\033[2K\033[u", 9999);
    fflush(stderr);
    plat_mutex_unlock(&p->mtx);
    plat_mutex_destroy(&p->mtx);
}

#endif /* PROGRESS_C */