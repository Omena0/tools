/*
 * mv.c - Cross-platform file move/rename with progress bar and glob support
 *
 * Usage: mv [options] <source...> <destination>
 *
 * Features:
 *   - ~ expands to home directory
 *   - Glob patterns: *.txt, dir/*, *something*
 *   - Recursive directory move
 *   - Multi-threaded parallel file operations
 *   - Progress bar with %, count, rate, ETA
 *   - Compatible as drop-in replacement for system mv
 *   - Falls back to copy+delete across filesystems
 *   - Set TOOLS_INTERNAL=1 env var for quiet mode (no progress bar)
 *
 * Build: gcc -O2 -o mv mv.c -lpthread  (Linux/macOS)
 *        cl /O2 mv.c                    (Windows)
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
static int          g_next_file  = 0;
static long long    g_total_bytes = 0;
static int          g_errors      = 0;
static int          g_force       = 0;
static int          g_verbose     = 0;
static int          g_internal    = 0;

/* Track source directories to remove after moving */
#define MAX_DIRS 8192
static char   g_src_dirs[MAX_DIRS][PATH_MAX_LEN];
static int    g_src_dir_count = 0;

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
    for (char *p = dst; *p; p++) {
#ifdef _WIN32
        if (*p == '/') *p = '\\';
#else
        if (*p == '\\') *p = '/';
#endif
    }
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

static int has_glob(const char *s) {
    for (; *s; s++) {
        if (*s == '*' || *s == '?') return 1;
    }
    return 0;
}

/* Record a source directory for later removal */
static void record_src_dir(const char *dir) {
    if (g_src_dir_count >= MAX_DIRS) return;
    strncpy(g_src_dirs[g_src_dir_count], dir, PATH_MAX_LEN - 1);
    g_src_dir_count++;
}

/* ── Collect files recursively ────────────────────────────────────── */

static void collect_dir(const char *src_dir, const char *dst_dir);

static void add_file(const char *src, const char *dst) {
    if (g_file_count >= MAX_FILES) {
        fprintf(stderr, "mv: too many files (max %d)\n", MAX_FILES);
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

    record_src_dir(src_dir);

    if (plat_dir_open(&dir, src_dir) != 0) {
        fprintf(stderr, "mv: cannot open directory '%s'\n", src_dir);
        return;
    }

    while (plat_dir_next(&dir, &ent) > 0) {
        char src_path[PATH_MAX_LEN], dst_path[PATH_MAX_LEN];
        join_path(src_path, src_dir, ent.name, PATH_MAX_LEN);
        join_path(dst_path, dst_dir, ent.name, PATH_MAX_LEN);

        if (plat_is_dir(src_path)) {
            collect_dir(src_path, dst_path);
        } else {
            add_file(src_path, dst_path);
        }
    }

    plat_dir_close(&dir);
}

static void expand_source(const char *raw_src, const char *dst_base, int dst_is_dir) {
    char src[PATH_MAX_LEN];
    normalize_path(src, raw_src, PATH_MAX_LEN);

    /* dir/* pattern */
    size_t len = strlen(src);
    if (len >= 2 && src[len-1] == '*' && (src[len-2] == '/' || src[len-2] == '\\')) {
        src[len-2] = '\0';
        if (!plat_is_dir(src)) {
            fprintf(stderr, "mv: '%s' is not a directory\n", src);
            g_errors++;
            return;
        }
        record_src_dir(src);
        collect_dir(src, dst_base);
        return;
    }

    /* Glob in basename */
    const char *base = basename_of(src);
    if (has_glob(base)) {
        char dir[PATH_MAX_LEN];
        dirname_of(dir, src, PATH_MAX_LEN);

        plat_dir_t d;
        plat_dirent_t ent;
        if (plat_dir_open(&d, dir) != 0) {
            fprintf(stderr, "mv: cannot open directory '%s'\n", dir);
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
                collect_dir(src_path, dst_path);
            } else {
                add_file(src_path, dst_path);
            }
        }
        plat_dir_close(&d);
        return;
    }

    /* Try same-filesystem rename for a whole directory first */
    if (plat_is_dir(src)) {
        char dst_dir[PATH_MAX_LEN];
        if (dst_is_dir)
            join_path(dst_dir, dst_base, basename_of(src), PATH_MAX_LEN);
        else
            strncpy(dst_dir, dst_base, PATH_MAX_LEN-1);

        /* Attempt direct rename (fast path, no file-by-file move needed) */
        if (rename(src, dst_dir) == 0) return;

        /* Fallback: file-by-file move */
        collect_dir(src, dst_dir);
    } else if (plat_is_file(src)) {
        char dst_path[PATH_MAX_LEN];
        if (dst_is_dir)
            join_path(dst_path, dst_base, basename_of(src), PATH_MAX_LEN);
        else
            strncpy(dst_path, dst_base, PATH_MAX_LEN-1);
        add_file(src, dst_path);
    } else {
        fprintf(stderr, "mv: cannot stat '%s': No such file or directory\n", src);
        g_errors++;
    }
}

/* ── Worker thread ────────────────────────────────────────────────── */

static PLAT_THREAD_FUNC(mv_worker, arg) {
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

        if (strcmp(f->src, f->dst) == 0) {
            fprintf(stderr, "mv: '%s' and '%s' are the same file\n", f->src, f->dst);
            plat_mutex_lock(&g_err_mtx);
            g_errors++;
            plat_mutex_unlock(&g_err_mtx);
            prog_advance(&g_bar, 1, f->size);
            continue;
        }

        if (g_file_count > 1 && !g_internal) {
            prog_print_path(&g_bar, f->src);
        }

        int r = plat_move_file(f->src, f->dst);
        if (r != 0) {
            fprintf(stderr, "mv: error moving '%s' -> '%s'\n", f->src, f->dst);
            plat_mutex_lock(&g_err_mtx);
            g_errors++;
            plat_mutex_unlock(&g_err_mtx);
        }

        prog_advance(&g_bar, 1, f->size);
    }
    return 0;
}

/* Remove empty source directories (deepest first) */
static void cleanup_src_dirs(void) {
    /* Sort by path length descending so children are removed before parents */
    for (int i = 0; i < g_src_dir_count - 1; i++) {
        for (int j = i + 1; j < g_src_dir_count; j++) {
            if (strlen(g_src_dirs[i]) < strlen(g_src_dirs[j])) {
                char tmp[PATH_MAX_LEN];
                memcpy(tmp, g_src_dirs[i], PATH_MAX_LEN);
                memcpy(g_src_dirs[i], g_src_dirs[j], PATH_MAX_LEN);
                memcpy(g_src_dirs[j], tmp, PATH_MAX_LEN);
            }
        }
    }
    for (int i = 0; i < g_src_dir_count; i++) {
#ifdef _WIN32
        RemoveDirectoryA(g_src_dirs[i]);
#else
        rmdir(g_src_dirs[i]);
#endif
    }
}

/* ── Usage ────────────────────────────────────────────────────────── */

static void usage(void) {
    fprintf(stderr,
        "Usage: mv [options] <source...> <destination>\n"
        "\n"
        "Options:\n"
        "  -f, --force     Force overwrite\n"
        "  -v, --verbose   Verbose output (print every file)\n"
        "  -h, --help      Show this help\n"
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

    const char *internal_env = getenv("TOOLS_INTERNAL");
    if (internal_env && strcmp(internal_env, "1") == 0)
        g_internal = 1;

    int src_count = 0;
    char *sources[4096];
    char *destination = NULL;

    for (int i = 1; i < argc; i++) {
        if (argv[i][0] == '-' && argv[i][1] != '\0') {
            if (strcmp(argv[i], "-f") == 0 || strcmp(argv[i], "--force") == 0) {
                g_force = 1;
            } else if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--verbose") == 0) {
                g_verbose = 1;
            } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
                usage(); return 0;
            } else {
                fprintf(stderr, "mv: unknown option '%s'\n", argv[i]);
                return 1;
            }
        } else {
            if (src_count < 4096)
                sources[src_count++] = argv[i];
        }
    }

    if (src_count < 2) { usage(); return 1; }

    destination = sources[--src_count];

    char dst[PATH_MAX_LEN];
    normalize_path(dst, destination, PATH_MAX_LEN);

    int dst_is_dir = plat_is_dir(dst);

    if (src_count > 1 && !dst_is_dir) {
        if (plat_mkdir_p(dst) == 0)
            dst_is_dir = 1;
        else {
            fprintf(stderr, "mv: target '%s' is not a directory\n", dst);
            return 1;
        }
    }

    /* For single file rename, try fast path */
    if (src_count == 1 && !has_glob(sources[0])) {
        char src[PATH_MAX_LEN];
        normalize_path(src, sources[0], PATH_MAX_LEN);

        /* Single file or dir rename */
        if (plat_is_file(src) || plat_is_dir(src)) {
            char final_dst[PATH_MAX_LEN];
            if (dst_is_dir)
                join_path(final_dst, dst, basename_of(src), PATH_MAX_LEN);
            else
                strncpy(final_dst, dst, PATH_MAX_LEN-1);

            if (rename(src, final_dst) == 0) return 0;
            /* Fall through to full pipeline if rename failed */
        }
    }

    for (int i = 0; i < src_count; i++) {
        expand_source(sources[i], dst, dst_is_dir);
    }

    if (g_file_count == 0) {
        fprintf(stderr, "mv: no files to move\n");
        return g_errors ? 1 : 0;
    }

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

    int n_threads = plat_cpu_count();
    if (n_threads > MAX_THREADS) n_threads = MAX_THREADS;
    if (n_threads > g_file_count) n_threads = g_file_count;
    if (n_threads < 1) n_threads = 1;

    if (n_threads == 1) {
        mv_worker(NULL);
    } else {
        plat_thread_t threads[MAX_THREADS];
        for (int i = 0; i < n_threads; i++)
            plat_thread_create(&threads[i], mv_worker, NULL);
        for (int i = 0; i < n_threads; i++)
            plat_thread_join(threads[i]);
    }

    if (!g_internal) {
        prog_finish(&g_bar);
    }

    /* Clean up empty source directories */
    if (g_src_dir_count > 0)
        cleanup_src_dirs();

    plat_mutex_destroy(&g_idx_mtx);
    plat_mutex_destroy(&g_err_mtx);

    return g_errors ? 1 : 0;
}