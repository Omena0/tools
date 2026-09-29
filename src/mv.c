/*
 * mv.c - Cross-platform file move/rename with progress bar and glob support
 *
 * Usage: mv [options] <source...> <destination>
 *
 * Features:
 *   - ~ expands to home directory
 *   - Glob patterns: *.txt, dir / *, *something*
 *   - Recursive directory move
 *   - Multi-threaded parallel file operations (-j N)
 *   - Progress bar with %, count, rate, ETA
 *   - Falls back to copy+delete across filesystems
 *   - Compatible as drop-in replacement for system mv
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

/* ── Constants ──────────────────────────────────────────────────────── */

#define TOOLS_VERSION "1.0.0"
#define MAX_FILES     65536
#define MAX_THREADS   64
#define MAX_SOURCES   4096
#define MAX_DIRS      8192

/* ── Options ────────────────────────────────────────────────────────── */

typedef struct {
    int recursive;        /* -r -R --recursive (no-op, mv always recurses) */
    int force;            /* -f --force                 */
    int no_clobber;       /* -n --no-clobber            */
    int interactive;      /* -i --interactive           */
    int update_only;      /* -u --update                */
    int verbose;          /* -v --verbose               */
    int jobs;             /* -j --jobs=N                */
    int no_progress;      /* --no-progress              */
} mv_opts_t;

/* ── File entry for the work queue ────────────────────────────────── */

typedef struct {
    char       src[PATH_MAX_LEN];
    char       dst[PATH_MAX_LEN];
    long long  size;
} file_entry_t;

/* ── Globals ────────────────────────────────────────────────────────── */

static mv_opts_t  g_opts;
static int        g_internal = 0;

static file_entry_t  g_files[MAX_FILES];
static int           g_file_count = 0;
static int           g_next_file  = 0;
static long long     g_total_bytes = 0;
static int           g_errors = 0;

static prog_bar_t     g_bar;
static plat_mutex_t   g_idx_mtx;
static plat_mutex_t   g_err_mtx;
static plat_mutex_t   g_io_mtx;

/* ── Source directories to remove after move ────────────────────────── */

static char  g_src_dirs[MAX_DIRS][PATH_MAX_LEN];
static int   g_src_dir_count = 0;

static void record_src_dir(const char *dir) {
    if (g_src_dir_count >= MAX_DIRS) return;
    snprintf(g_src_dirs[g_src_dir_count], PATH_MAX_LEN, "%s", dir);
    g_src_dir_count++;
}

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
    else { dst[0] = '.'; dst[1] = '\0'; }
}

static void join_path(char *dst, const char *dir, const char *name, size_t len) {
    snprintf(dst, len, "%s%c%s", dir, PATH_SEP, name);
}

/* ── Glob ────────────────────────────────────────────────────────── */

static int has_glob(const char *s) {
    for (; *s; s++) {
        if (*s == '*' || *s == '?') return 1;
    }
    return 0;
}

/* ── Path length guard ────────────────────────────────────────────── */

static void check_path_len(const char *path) {
    if (plat_path_too_long(path)) {
        fprintf(stderr, "mv: name too long: '%s'\n", path);
        exit(1);
    }
}

/* ── Interactive prompt ───────────────────────────────────────────── */

static int prompt_overwrite(const char *dst) {
    if (!plat_is_stdin_tty())
        return 1; /* no tty — assume yes */
    fprintf(stderr, "mv: overwrite '%s'? ", dst);
    fflush(stderr);
    char resp[32];
    if (fgets(resp, sizeof(resp), stdin))
        return (resp[0] == 'y' || resp[0] == 'Y');
    return 0;
}

/* Print a message above the progress bar (or to stderr if bar is off) */
static void print_msg(const char *msg) {
    if (!g_opts.no_progress && !g_internal) {
        prog_print_path(&g_bar, msg);
    } else {
        plat_mutex_lock(&g_io_mtx);
        fprintf(stderr, "%s\n", msg);
        fflush(stderr);
        plat_mutex_unlock(&g_io_mtx);
    }
}

/* ── Collect files recursively ────────────────────────────────────── */

static void collect_dir(const char *src_dir, const char *dst_dir);

static void add_file(const char *src, const char *dst) {
    check_path_len(src);
    check_path_len(dst);

    if (g_file_count >= MAX_FILES)
        return; /* silently stop adding */

    long long sz = plat_file_size(src);
    if (sz < 0) sz = 0;

    strncpy(g_files[g_file_count].src, src, PATH_MAX_LEN-1);
    g_files[g_file_count].src[PATH_MAX_LEN-1] = '\0';
    strncpy(g_files[g_file_count].dst, dst, PATH_MAX_LEN-1);
    g_files[g_file_count].dst[PATH_MAX_LEN-1] = '\0';
    g_files[g_file_count].size = sz;
    g_total_bytes += sz;
    g_file_count++;
}

static int is_dot_or_dotdot(const char *name) {
    return (strcmp(name, ".") == 0 || strcmp(name, "..") == 0);
}

static void collect_dir(const char *src_dir, const char *dst_dir) {
    check_path_len(src_dir);
    check_path_len(dst_dir);

    record_src_dir(src_dir);

    plat_dir_t dir;
    plat_dirent_t ent;

    if (plat_dir_open(&dir, src_dir) != 0) {
        fprintf(stderr, "mv: cannot open directory '%s'\n", src_dir);
        return;
    }

    while (plat_dir_next(&dir, &ent) > 0) {
        if (is_dot_or_dotdot(ent.name)) continue;

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
    check_path_len(src);

    /* dir / * pattern */
    size_t len = strlen(src);
    if (len >= 2 && src[len-1] == '*' && (src[len-2] == '/' || src[len-2] == '\\')) {
        src[len-2] = '\0';
        check_path_len(src);
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
        check_path_len(dir);

        plat_dir_t d;
        plat_dirent_t ent;
        if (plat_dir_open(&d, dir) != 0) {
            fprintf(stderr, "mv: cannot open directory '%s'\n", dir);
            g_errors++;
            return;
        }

        while (plat_dir_next(&d, &ent) > 0) {
            if (is_dot_or_dotdot(ent.name)) continue;
            if (!plat_glob_match(base, ent.name)) continue;

            char src_path[PATH_MAX_LEN], dst_path[PATH_MAX_LEN];
            join_path(src_path, dir, ent.name, PATH_MAX_LEN);
            check_path_len(src_path);

            if (dst_is_dir)
                join_path(dst_path, dst_base, ent.name, PATH_MAX_LEN);
            else
                strncpy(dst_path, dst_base, PATH_MAX_LEN-1);
            dst_path[PATH_MAX_LEN-1] = '\0';
            check_path_len(dst_path);

            if (plat_is_dir(src_path)) {
                collect_dir(src_path, dst_path);
            } else {
                add_file(src_path, dst_path);
            }
        }
        plat_dir_close(&d);
        return;
    }

    /* Plain path — file or directory */
    if (plat_is_dir(src)) {
        char dst_dir[PATH_MAX_LEN];
        if (dst_is_dir)
            join_path(dst_dir, dst_base, basename_of(src), PATH_MAX_LEN);
        else
            strncpy(dst_dir, dst_base, PATH_MAX_LEN-1);
        dst_dir[PATH_MAX_LEN-1] = '\0';
        check_path_len(dst_dir);

        /* Attempt direct rename (fast path) */
        if (rename(src, dst_dir) == 0) return;

        /* Fallback: file-by-file move */
        collect_dir(src, dst_dir);
    } else if (plat_is_file(src)) {
        char dst_path[PATH_MAX_LEN];
        if (dst_is_dir)
            join_path(dst_path, dst_base, basename_of(src), PATH_MAX_LEN);
        else
            strncpy(dst_path, dst_base, PATH_MAX_LEN-1);
        dst_path[PATH_MAX_LEN-1] = '\0';
        check_path_len(dst_path);
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

        /* Same file check */
        if (strcmp(f->src, f->dst) == 0) {
            fprintf(stderr, "mv: '%s' and '%s' are the same file\n", f->src, f->dst);
            plat_mutex_lock(&g_err_mtx);
            g_errors++;
            plat_mutex_unlock(&g_err_mtx);
            if (!g_opts.no_progress && !g_internal)
                prog_advance(&g_bar, 1, f->size);
            continue;
        }

        /* Determine if destination exists */
        int dst_exists = plat_path_exists(f->dst);

        /* Overwrite rules — precedence: -n > -i > -f/default */
        if (dst_exists) {
            if (g_opts.no_clobber) {
                if (g_opts.verbose) {
                    char msg[PATH_MAX_LEN + 32];
                    snprintf(msg, sizeof(msg), "not overwriting '%s'", f->dst);
                    print_msg(msg);
                }
                if (!g_opts.no_progress && !g_internal)
                    prog_advance(&g_bar, 1, 0);
                continue;
            }
            if (g_opts.interactive) {
                plat_mutex_lock(&g_io_mtx);
                int answer = prompt_overwrite(f->dst);
                plat_mutex_unlock(&g_io_mtx);
                if (!answer) {
                    if (g_opts.verbose) {
                        char msg[PATH_MAX_LEN + 32];
                        snprintf(msg, sizeof(msg), "not overwriting '%s'", f->dst);
                        print_msg(msg);
                    }
                    if (!g_opts.no_progress && !g_internal)
                        prog_advance(&g_bar, 1, 0);
                    continue;
                }
            }

            /* Remove existing destination */
            if (g_opts.force) {
                plat_make_writable(f->dst);
            }
            plat_remove_file(f->dst);
        }

        /* Update-only check (-u) */
        if (g_opts.update_only) {
            if (!plat_file_newer(f->src, f->dst)) {
                if (g_opts.verbose) {
                    char msg[PATH_MAX_LEN + 32];
                    snprintf(msg, sizeof(msg), "'%s' is up to date.", f->dst);
                    print_msg(msg);
                }
                if (!g_opts.no_progress && !g_internal)
                    prog_advance(&g_bar, 1, 0);
                continue;
            }
        }

        /* Verbose output */
        if (g_opts.verbose) {
            char msg[PATH_MAX_LEN * 2 + 32];
            snprintf(msg, sizeof(msg), "'%s' -> '%s'", f->src, f->dst);
            print_msg(msg);
        }

        /* Print path in progress bar (when not verbose) */
        if (!g_opts.no_progress && !g_internal && g_file_count > 1 && !g_opts.verbose) {
            prog_print_path(&g_bar, f->src);
        }

        int r = plat_move_file(f->src, f->dst);
        if (r != 0) {
            fprintf(stderr, "mv: error moving '%s' -> '%s'\n", f->src, f->dst);
            plat_mutex_lock(&g_err_mtx);
            g_errors++;
            plat_mutex_unlock(&g_err_mtx);
        }

        if (!g_opts.no_progress && !g_internal)
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
        rmdir(g_src_dirs[i]);
    }
}

/* ── Usage & version ──────────────────────────────────────────────── */

static void usage(void) {
    fprintf(stderr,
        "Usage: mv [OPTION]... SOURCE... DEST\n"
        "\n"
        "Move (rename) files.\n"
        "\n"
        "Options:\n"
        "  -f, --force          Do not prompt before overwriting\n"
        "  -i, --interactive      Prompt before overwriting\n"
        "  -n, --no-clobber       Do not overwrite an existing file\n"
        "  -u, --update           Move only when source is newer\n"
        "  -v, --verbose          Explain what is being done\n"
        "  -r, -R, --recursive    Recurse into directories (always on for mv)\n"
        "  -j, --jobs=N           Number of parallel jobs (default: CPU count)\n"
        "      --no-progress      Turn off progress meter\n"
        "      --help             Show this help and exit\n"
        "      --version          Output version information and exit\n"
        "  --                     End of options\n"
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

static void version(void) {
    fprintf(stderr, "mv (tools) %s\n", TOOLS_VERSION);
    fprintf(stderr, "Copyright (C) 2026 Omena0. All rights reserved.\n");
    fprintf(stderr, "License: BSD-3-Clause\n");
}

/* ── Argument parser ──────────────────────────────────────────────── */

static int parse_long_opt(const char *name, const char *value, mv_opts_t *o) {
    if (strcmp(name, "force") == 0)        { o->force = 1; }
    else if (strcmp(name, "no-clobber") == 0) { o->no_clobber = 1; }
    else if (strcmp(name, "interactive") == 0) { o->interactive = 1; }
    else if (strcmp(name, "update") == 0)  { o->update_only = 1; }
    else if (strcmp(name, "verbose") == 0) { o->verbose = 1; }
    else if (strcmp(name, "recursive") == 0) { o->recursive = 1; }
    else if (strcmp(name, "jobs") == 0) {
        if (!value) return -1;
        o->jobs = atoi(value);
        if (o->jobs < 1) o->jobs = 1;
    }
    else if (strcmp(name, "no-progress") == 0) { o->no_progress = 1; }
    else if (strcmp(name, "help") == 0)     { usage(); exit(0); }
    else if (strcmp(name, "version") == 0)  { version(); exit(0); }
    else {
        fprintf(stderr, "mv: unrecognized option '--%s'\n", name);
        return -2;
    }
    return 0;
}

static int parse_short_opt(char c, mv_opts_t *o) {
    switch (c) {
        case 'f': o->force = 1; break;
        case 'n': o->no_clobber = 1; break;
        case 'i': o->interactive = 1; break;
        case 'u': o->update_only = 1; break;
        case 'v': o->verbose = 1; break;
        case 'r': case 'R': o->recursive = 1; break;
        default:
            fprintf(stderr, "mv: invalid option -- '%c'\n", c);
            return -2;
    }
    return 0;
}

static int parse_args(int argc, char **argv, mv_opts_t *o, char *sources[], int *src_count) {
    int end_of_opts = 0;

    for (int i = 1; i < argc; i++) {
        if (!end_of_opts && argv[i][0] == '-' && argv[i][1] != '\0') {
            if (strcmp(argv[i], "--") == 0) {
                end_of_opts = 1;
                continue;
            }

            if (argv[i][1] == '-') {
                /* Long option */
                const char *name = argv[i] + 2;
                const char *eq = strchr(name, '=');
                char opt[64];
                const char *val = NULL;

                if (eq) {
                    size_t n = eq - name;
                    if (n >= sizeof(opt)) n = sizeof(opt) - 1;
                    memcpy(opt, name, n);
                    opt[n] = '\0';
                    val = eq + 1;
                } else {
                    strncpy(opt, name, sizeof(opt) - 1);
                    opt[sizeof(opt) - 1] = '\0';
                }

                if (!val && strcmp(opt, "jobs") == 0) {
                    if (i + 1 < argc) {
                        val = argv[++i];
                    } else {
                        fprintf(stderr, "mv: --jobs requires an argument\n");
                        return 1;
                    }
                }

                int r = parse_long_opt(opt, val, o);
                if (r == -2) return 1;
                if (r == -1) {
                    fprintf(stderr, "mv: --%s requires an argument\n", opt);
                    return 1;
                }
            } else {
                /* Short option(s) — may be combined like -fv, -vi */
                const char *p = argv[i] + 1;
                while (*p) {
                    char c = *p++;
                    if (c == 'j') {
                        /* -j with attached value: -j4 */
                        if (*p) {
                            o->jobs = atoi(p);
                            if (o->jobs < 1) o->jobs = 1;
                            break;
                        } else {
                            if (i + 1 < argc) {
                                o->jobs = atoi(argv[++i]);
                                if (o->jobs < 1) o->jobs = 1;
                            } else {
                                fprintf(stderr, "mv: -j requires an argument\n");
                                return 1;
                            }
                            break;
                        }
                    }
                    int r = parse_short_opt(c, o);
                    if (r == -2) return 1;
                }
            }
        } else if (argv[i][0] == '-' && argv[i][1] == '\0') {
            if (*src_count < MAX_SOURCES)
                sources[(*src_count)++] = argv[i];
        } else {
            if (*src_count < MAX_SOURCES)
                sources[(*src_count)++] = argv[i];
            end_of_opts = 1;
        }
    }

    return 0;
}

/* ── Main ─────────────────────────────────────────────────────────── */

int main(int argc, char **argv) {
    const char *internal_env = getenv("TOOLS_INTERNAL");
    if (internal_env && strcmp(internal_env, "1") == 0)
        g_internal = 1;

    /* Defaults */
    g_opts.jobs = plat_cpu_count();

    char *sources[MAX_SOURCES];
    int src_count = 0;

    if (parse_args(argc, argv, &g_opts, sources, &src_count) != 0)
        return 1;

    if (src_count < 2) {
        usage();
        return 1;
    }

    /* Last positional arg is destination */
    char *destination = sources[--src_count];

    char dst[PATH_MAX_LEN];
    normalize_path(dst, destination, PATH_MAX_LEN);
    check_path_len(dst);

    int dst_is_dir = plat_is_dir(dst);

    /* Destination with trailing slash means "directory" */
    if (!dst_is_dir && strlen(destination) > 0) {
        char last = destination[strlen(destination) - 1];
        if (last == '/' || last == '\\') {
            dst_is_dir = 1;
            if (plat_mkdir_p(dst) != 0) {
                fprintf(stderr, "mv: cannot create directory '%s'\n", dst);
                return 1;
            }
        }
    }

    if (src_count > 1 && !dst_is_dir) {
        if (plat_mkdir_p(dst) == 0)
            dst_is_dir = 1;
        else {
            fprintf(stderr, "mv: target '%s' is not a directory\n", dst);
            return 1;
        }
    }

    /* Fast path: single file/dir rename */
    if (src_count == 1 && !has_glob(sources[0])) {
        char src[PATH_MAX_LEN];
        normalize_path(src, sources[0], PATH_MAX_LEN);
        check_path_len(src);

        if (plat_is_file(src) || plat_is_dir(src)) {
            char final_dst[PATH_MAX_LEN];
            if (dst_is_dir)
                join_path(final_dst, dst, basename_of(src), PATH_MAX_LEN);
            else
                strncpy(final_dst, dst, PATH_MAX_LEN-1);
            final_dst[PATH_MAX_LEN-1] = '\0';
            check_path_len(final_dst);

            /* Handle overwrite rules for the fast path */
            if (plat_path_exists(final_dst)) {
                if (g_opts.no_clobber) {
                    if (g_opts.verbose)
                        fprintf(stderr, "not overwriting '%s'\n", final_dst);
                    return 0;
                }
                if (g_opts.interactive) {
                    if (!prompt_overwrite(final_dst)) {
                        if (g_opts.verbose)
                            fprintf(stderr, "not overwriting '%s'\n", final_dst);
                        return 0;
                    }
                }
                if (g_opts.force)
                    plat_make_writable(final_dst);
                plat_remove_file(final_dst);
            }

            if (rename(src, final_dst) == 0) {
                if (g_opts.verbose)
                    fprintf(stderr, "'%s' -> '%s'\n", src, final_dst);
                if (plat_is_dir(src)) {
                    /* Source dir was renamed, nothing to clean up */
                }
                return 0;
            }
            /* Fall through to full pipeline if rename failed (e.g. cross-device) */
        }
    }

    /* Full pipeline */
    for (int i = 0; i < src_count; i++) {
        expand_source(sources[i], dst, dst_is_dir);
    }

    if (g_file_count == 0) {
        fprintf(stderr, "mv: no files to move\n");
        return g_errors ? 1 : 0;
    }

    plat_mutex_init(&g_idx_mtx);
    plat_mutex_init(&g_err_mtx);
    plat_mutex_init(&g_io_mtx);

    if (!g_opts.no_progress && !g_internal) {
        prog_init(&g_bar);
        g_bar.print_paths = 1; /* enable prog_print_path for verbose & path display */
        prog_add_field(&g_bar, PROG_FIELD_PERCENT);
        prog_add_field(&g_bar, PROG_FIELD_BAR);
        prog_add_field(&g_bar, PROG_FIELD_COUNT);
        prog_add_field(&g_bar, PROG_FIELD_DATA_RATE);
        prog_add_field(&g_bar, PROG_FIELD_FILE_RATE);
        prog_add_field(&g_bar, PROG_FIELD_ETA);
        prog_start(&g_bar, g_file_count, g_total_bytes);
    }

    int n_threads = g_opts.jobs;
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

    if (!g_opts.no_progress && !g_internal) {
        prog_finish(&g_bar);
    }

    if (g_src_dir_count > 0)
        cleanup_src_dirs();

    plat_mutex_destroy(&g_idx_mtx);
    plat_mutex_destroy(&g_err_mtx);
    plat_mutex_destroy(&g_io_mtx);

    return g_errors ? 1 : 0;
}
