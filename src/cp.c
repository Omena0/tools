/*
 * cp.c - Cross-platform file copy with progress bar and glob support
 *
 * Usage: cp [options] <source...> <destination>
 *
 * Features:
 *   - ~ expands to home directory
 *   - Glob patterns: *.txt, dir/*, *something*
 *   - Recursive directory copy
 *   - Multi-threaded parallel file operations
 *   - Progress bar with %, count, rate, ETA
 *   - Compatible as drop-in replacement for system cp
 *   - Set TOOLS_INTERNAL=1 env var for quiet mode (no progress bar)
 *
 * Build: gcc -O2 -o cp cp.c -lpthread  (Linux/macOS)
 *        cl /O2 cp.c                    (Windows)
 */

#include "lib/platform.c"
#include "lib/progress.c"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Limits ───────────────────────────────────────────────────────── */

#define MAX_FILES    65536
#define MAX_THREADS  16

/* ── File entry for the work queue ────────────────────────────────── */

typedef struct {
    char src[PATH_MAX_LEN];
    char dst[PATH_MAX_LEN];
    long long size;
} file_entry_t;

static file_entry_t g_files[MAX_FILES];
static int          g_file_count = 0;
static int          g_next_file  = 0; /* atomic-ish index for workers */
static long long    g_total_bytes = 0;
static int          g_errors      = 0;
static int          g_recursive   = 0;
static int          g_force       = 0;
static int          g_verbose     = 0;
static int          g_internal    = 0; /* set via TOOLS_INTERNAL env var */

static prog_bar_t   g_bar;
static plat_mutex_t g_idx_mtx;
static plat_mutex_t g_err_mtx;

/* ── Path utilities ───────────────────────────────────────────────── */

static void normalize_path(char *dst, const char *src, size_t len) {
    if (src[0] == '~' && (src[1] == '/' || src[1] == '\\' || src[1] == '\0')) {
        snprintf(dst, len, "%s%s", plat_home_dir(), src + 1);
    } else {
        strncpy(dst, src, len - 1);
        dst[len-1] = '\0';
    }
    /* Normalize separators */
    for (char *p = dst; *p; p++) {
#ifdef _WIN32
        if (*p == '/') *p = '\\';
#else
        if (*p == '\\') *p = '/';
#endif
    }
    /* Remove trailing separator (unless root) */
    size_t l = strlen(dst);
    if (l > 1 && (dst[l-1] == '/' || dst[l-1] == '\\'))
        dst[l-1] = '\0';
}

static const char *basename_of(const char *path) {
    const char *s = strrchr(path, '/');
    const char *b = strrchr(path, '\\');
    if (b > s) s = b;
    return s ? s + 1 : path;
}

static void dirname_of(char *dst, const char *path, size_t len) {
    strncpy(dst, path, len - 1);
    dst[len-1] = '\0';
    char *s = strrchr(dst, '/');
    char *b = strrchr(dst, '\\');
    if (b > s) s = b;
    if (s) *s = '\0';
    else dst[0] = '.', dst[1] = '\0';
}

static void join_path(char *dst, const char *dir, const char *name, size_t len) {
    snprintf(dst, len, "%s%c%s", dir, PATH_SEP, name);
}

/* ── Pattern checking ─────────────────────────────────────────────── */

static int has_glob(const char *s) {
    for (; *s; s++) {
        if (*s == '*' || *s == '?') return 1;
    }
    return 0;
}

/* ── Collect files recursively ────────────────────────────────────── */

static void collect_dir(const char *src_dir, const char *dst_dir);

static void add_file(const char *src, const char *dst) {
    if (g_file_count >= MAX_FILES) {
        fprintf(stderr, "cp: too many files (max %d)\n", MAX_FILES);
        return;
    }
    long long sz = plat_file_size(src);
    if (sz < 0) sz = 0;

    strncpy(g_files[g_file_count].src, src, PATH_MAX_LEN-1);
    strncpy(g_files[g_file_count].dst, dst, PATH_MAX_LEN-1);
    g_files[g_file_count].size = sz;
    g_total_bytes += sz;
    g_file_count++;
}

static void collect_dir(const char *src_dir, const char *dst_dir) {
    plat_dir_t dir;
    plat_dirent_t ent;

    if (plat_dir_open(&dir, src_dir) != 0) {
        fprintf(stderr, "cp: cannot open directory '%s'\n", src_dir);
        return;
    }

    while (plat_dir_next(&dir, &ent) > 0) {
        char src_path[PATH_MAX_LEN], dst_path[PATH_MAX_LEN];
        join_path(src_path, src_dir, ent.name, PATH_MAX_LEN);
        join_path(dst_path, dst_dir, ent.name, PATH_MAX_LEN);

        /* Re-check is_dir with full path for platforms where d_type is unreliable */
        if (plat_is_dir(src_path)) {
            if (g_recursive) {
                collect_dir(src_path, dst_path);
            }
        } else {
            add_file(src_path, dst_path);
        }
    }

    plat_dir_close(&dir);
}

/* Expand a source pattern into file entries */
static void expand_source(const char *raw_src, const char *dst_base, int dst_is_dir) {
    char src[PATH_MAX_LEN];
    normalize_path(src, raw_src, PATH_MAX_LEN);

    /* Check for dir/* pattern (all contents of directory) */
    size_t len = strlen(src);
    if (len >= 2 && src[len-1] == '*' && (src[len-2] == '/' || src[len-2] == '\\')) {
        /* dir/* -> copy contents of dir into dst */
        src[len-2] = '\0';
        if (!plat_is_dir(src)) {
            fprintf(stderr, "cp: '%s' is not a directory\n", src);
            g_errors++;
            return;
        }
        collect_dir(src, dst_base);
        return;
    }

    /* Check for glob patterns in the basename */
    const char *base = basename_of(src);
    if (has_glob(base)) {
        char dir[PATH_MAX_LEN];
        dirname_of(dir, src, PATH_MAX_LEN);

        plat_dir_t d;
        plat_dirent_t ent;
        if (plat_dir_open(&d, dir) != 0) {
            fprintf(stderr, "cp: cannot open directory '%s'\n", dir);
            g_errors++;
            return;
        }

        while (plat_dir_next(&d, &ent) > 0) {
            if (!plat_glob_match(base, ent.name)) continue;

            char src_path[PATH_MAX_LEN], dst_path[PATH_MAX_LEN];
            join_path(src_path, dir, ent.name, PATH_MAX_LEN);

            if (dst_is_dir)
                join_path(dst_path, dst_base, ent.name, PATH_MAX_LEN);
            else
                strncpy(dst_path, dst_base, PATH_MAX_LEN-1);

            if (plat_is_dir(src_path)) {
                if (g_recursive) {
                    collect_dir(src_path, dst_path);
                } else {
                    fprintf(stderr, "cp: omitting directory '%s' (use -r)\n", src_path);
                }
            } else {
                add_file(src_path, dst_path);
            }
        }
        plat_dir_close(&d);
        return;
    }

    /* Plain path: file or directory */
    if (plat_is_dir(src)) {
        if (!g_recursive) {
            fprintf(stderr, "cp: omitting directory '%s' (use -r)\n", src);
            g_errors++;
            return;
        }
        char dst_dir[PATH_MAX_LEN];
        if (dst_is_dir)
            join_path(dst_dir, dst_base, basename_of(src), PATH_MAX_LEN);
        else
            strncpy(dst_dir, dst_base, PATH_MAX_LEN-1);
        collect_dir(src, dst_dir);
    } else if (plat_is_file(src)) {
        char dst_path[PATH_MAX_LEN];
        if (dst_is_dir)
            join_path(dst_path, dst_base, basename_of(src), PATH_MAX_LEN);
        else
            strncpy(dst_path, dst_base, PATH_MAX_LEN-1);
        add_file(src, dst_path);
    } else {
        fprintf(stderr, "cp: cannot stat '%s': No such file or directory\n", src);
        g_errors++;
    }
}

/* ── Worker thread ────────────────────────────────────────────────── */

static PLAT_THREAD_FUNC(cp_worker, arg) {
    (void)arg;
    while (1) {
        plat_mutex_lock(&g_idx_mtx);
        int idx = g_next_file++;
        plat_mutex_unlock(&g_idx_mtx);

        if (idx >= g_file_count) break;

        file_entry_t *f = &g_files[idx];

        /* Ensure destination directory exists */
        char dir[PATH_MAX_LEN];
        dirname_of(dir, f->dst, PATH_MAX_LEN);
        plat_mkdir_p(dir);

        /* Skip if source and destination are the same */
        if (strcmp(f->src, f->dst) == 0) {
            fprintf(stderr, "cp: '%s' and '%s' are the same file\n", f->src, f->dst);
            plat_mutex_lock(&g_err_mtx);
            g_errors++;
            plat_mutex_unlock(&g_err_mtx);
            prog_advance(&g_bar, 1, f->size);
            continue;
        }

        /* Check if destination exists and -f not set */
        if (!g_force && plat_is_file(f->dst)) {
            /* Default: overwrite (like standard cp) */
        }

        if (g_file_count > 1 && !g_internal) {
            /* Print relative path */
            prog_print_path(&g_bar, f->src);
        }

        int r = plat_copy_file(f->src, f->dst);
        if (r != 0) {
            fprintf(stderr, "cp: error copying '%s' -> '%s'\n", f->src, f->dst);
            plat_mutex_lock(&g_err_mtx);
            g_errors++;
            plat_mutex_unlock(&g_err_mtx);
        }

        prog_advance(&g_bar, 1, f->size);
    }
    return 0;
}

/* ── Usage ────────────────────────────────────────────────────────── */

static void usage(void) {
    fprintf(stderr,
        "Usage: cp [options] <source...> <destination>\n"
        "\n"
        "Options:\n"
        "  -r, -R, --recursive   Copy directories recursively\n"
        "  -f, --force           Force overwrite\n"
        "  -v, --verbose         Verbose output (print every file)\n"
        "  -h, --help            Show this help\n"
        "\n"
        "Path patterns:\n"
        "  ~            Home directory\n"
        "  dir/*        All files and folders under dir\n"
        "  *.ext        Files matching extension\n"
        "  *something*  Wildcard matching\n"
        "\n"
        "Environment:\n"
        "  TOOLS_INTERNAL=1   Suppress progress bar (for use by other programs)\n"
    );
}

/* ── Main ─────────────────────────────────────────────────────────── */

int main(int argc, char **argv) {
    if (argc < 3) { usage(); return 1; }

    /* Check TOOLS_INTERNAL env var */
    const char *internal_env = getenv("TOOLS_INTERNAL");
    if (internal_env && strcmp(internal_env, "1") == 0)
        g_internal = 1;

    /* Parse arguments */
    int src_count = 0;
    char *sources[4096];
    char *destination = NULL;

    for (int i = 1; i < argc; i++) {
        if (argv[i][0] == '-' && argv[i][1] != '\0') {
            if (strcmp(argv[i], "-r") == 0 || strcmp(argv[i], "-R") == 0 || strcmp(argv[i], "--recursive") == 0) {
                g_recursive = 1;
            } else if (strcmp(argv[i], "-f") == 0 || strcmp(argv[i], "--force") == 0) {
                g_force = 1;
            } else if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--verbose") == 0) {
                g_verbose = 1;
            } else if (strcmp(argv[i], "-rf") == 0 || strcmp(argv[i], "-fr") == 0) {
                g_recursive = 1; g_force = 1;
            } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
                usage(); return 0;
            } else {
                fprintf(stderr, "cp: unknown option '%s'\n", argv[i]);
                return 1;
            }
        } else {
            if (src_count < 4096)
                sources[src_count++] = argv[i];
        }
    }

    if (src_count < 2) { usage(); return 1; }

    /* Last positional arg is destination */
    destination = sources[--src_count];

    char dst[PATH_MAX_LEN];
    normalize_path(dst, destination, PATH_MAX_LEN);

    int dst_is_dir = plat_is_dir(dst);

    /* If multiple sources, destination must be a directory */
    if (src_count > 1 && !dst_is_dir) {
        /* Try to create it */
        if (plat_mkdir_p(dst) == 0)
            dst_is_dir = 1;
        else {
            fprintf(stderr, "cp: target '%s' is not a directory\n", dst);
            return 1;
        }
    }

    /* Expand sources */
    for (int i = 0; i < src_count; i++) {
        expand_source(sources[i], dst, dst_is_dir);
    }

    if (g_file_count == 0) {
        fprintf(stderr, "cp: no files to copy\n");
        return g_errors ? 1 : 0;
    }

    /* Set up progress bar */
    plat_mutex_init(&g_idx_mtx);
    plat_mutex_init(&g_err_mtx);

    if (!g_internal) {
        prog_init(&g_bar);
        g_bar.print_paths = (g_file_count > 1 || g_verbose) ? 1 : 0;
        prog_add_field(&g_bar, PROG_FIELD_PERCENT);
        prog_add_field(&g_bar, PROG_FIELD_BAR);
        prog_add_field(&g_bar, PROG_FIELD_COUNT);
        prog_add_field(&g_bar, PROG_FIELD_DATA_RATE);
        prog_add_field(&g_bar, PROG_FIELD_FILE_RATE);
        prog_add_field(&g_bar, PROG_FIELD_ETA);
        prog_start(&g_bar, g_file_count, g_total_bytes);
    }

    /* Spawn worker threads */
    int n_threads = plat_cpu_count();
    if (n_threads > MAX_THREADS) n_threads = MAX_THREADS;
    if (n_threads > g_file_count) n_threads = g_file_count;
    if (n_threads < 1) n_threads = 1;

    if (n_threads == 1) {
        /* Single-threaded fast path */
        cp_worker(NULL);
    } else {
        plat_thread_t threads[MAX_THREADS];
        for (int i = 0; i < n_threads; i++)
            plat_thread_create(&threads[i], cp_worker, NULL);
        for (int i = 0; i < n_threads; i++)
            plat_thread_join(threads[i]);
    }

    if (!g_internal) {
        prog_finish(&g_bar);
    }

    plat_mutex_destroy(&g_idx_mtx);
    plat_mutex_destroy(&g_err_mtx);

    return g_errors ? 1 : 0;
}