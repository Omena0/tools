/*
 * cp.c - Cross-platform file copy with progress bar and glob support
 *
 * Usage: cp [options] <source...> <destination>
 *
 * Features:
 *   - ~ expands to home directory
 *   - Glob patterns: *.txt, dir / *, *something*
 *   - Recursive directory copy
 *   - Multi-threaded parallel file operations (-j N)
 *   - Progress bar with %, count, rate, ETA
 *   - Reflink (copy-on-write) when supported (--reflink=auto)
 *   - Sparse file support (--sparse=auto)
 *   - Hard links (-l) and symbolic links (-s)
 *   - File backup (-b) and --parents
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

/* ── Constants ──────────────────────────────────────────────────────── */

#define TOOLS_VERSION "1.0.0"
#define MAX_FILES     65536
#define MAX_THREADS   64
#define MAX_SOURCES   4096

#define REFLINK_AUTO    0
#define REFLINK_ALWAYS  1
#define REFLINK_NEVER   2

#define SPARSE_AUTO    0
#define SPARSE_ALWAYS  1
#define SPARSE_NEVER   2

/* ── Options ────────────────────────────────────────────────────────── */

typedef struct {
    int recursive;        /* -r -R --recursive          */
    int force;            /* -f --force                 */
    int no_clobber;       /* -n --no-clobber            */
    int interactive;      /* -i --interactive           */
    int update_only;      /* -u --update                */
    int verbose;          /* -v --verbose               */
    int archive;          /* -a --archive               */
    int preserve;         /* -p --preserve              */
    int no_dereference;   /* -P --no-dereference        */
    int dereference;      /* -L --dereference           */
    int follow_cmdline;   /* -H                         */
    int hard_link;        /* -l --link                  */
    int sym_link;         /* -s --symbolic-link         */
    int jobs;             /* -j --jobs=N                */
    int reflink;          /* --reflink=auto|always|never */
    int sparse;           /* --sparse=auto|always|never */
    int backup;           /* -b --backup                */
    int parents;          /* --parents                  */
    int no_progress;      /* --no-progress              */
} cp_opts_t;

/* ── File entry for the work queue ────────────────────────────────── */

typedef struct {
    char       src[PATH_MAX_LEN];
    char       dst[PATH_MAX_LEN];
    long long  size;
} file_entry_t;

/* ── Globals ────────────────────────────────────────────────────────── */

static cp_opts_t  g_opts;
static int        g_internal       = 0; /* set via TOOLS_INTERNAL env var */

static file_entry_t  g_files[MAX_FILES];
static int           g_file_count = 0;
static int           g_next_file  = 0;
static long long     g_total_bytes = 0;
static int           g_errors = 0;

static prog_bar_t     g_bar;
static plat_mutex_t   g_idx_mtx;
static plat_mutex_t   g_err_mtx;
static plat_mutex_t   g_io_mtx;   /* for verbose / prompt serialisation */

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
        fprintf(stderr, "cp: name too long: '%s'\n", path);
        exit(1);
    }
}

/* ── Interactive prompt ───────────────────────────────────────────── */

static int prompt_overwrite(const char *dst) {
    if (!plat_is_stdin_tty())
        return 1; /* no tty — assume yes */
    fprintf(stderr, "cp: overwrite '%s'? ", dst);
    fflush(stderr);
    char resp[32];
    if (fgets(resp, sizeof(resp), stdin))
        return (resp[0] == 'y' || resp[0] == 'Y');
    return 0;
}

/* ── Backup ───────────────────────────────────────────────────────── */

static void make_backup(const char *dst) {
    char tmp[PATH_MAX_LEN];
    snprintf(tmp, sizeof(tmp), "%s~", dst);
    rename(dst, tmp);
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

/* ── Single-file copy logic ───────────────────────────────────────── */

static int do_copy_operation(const char *src, const char *dst) {
    int r;

    /* Hard link (-l) */
    if (g_opts.hard_link) {
        r = plat_make_hardlink(src, dst);
        if (r != 0)
            fprintf(stderr, "cp: failed to hard link '%s' -> '%s'\n", src, dst);
        return r;
    }

    /* Symbolic link (-s) */
    if (g_opts.sym_link) {
        r = plat_make_symlink(src, dst);
        if (r != 0)
            fprintf(stderr, "cp: failed to create symbolic link '%s' -> '%s'\n", dst, src);
        return r;
    }

    /* With -P, if source is a symlink, copy the symlink itself */
    if (g_opts.no_dereference && plat_is_symlink(src)) {
        char target[PATH_MAX_LEN];
        if (plat_read_symlink_target(src, target, sizeof(target)) == 0) {
            r = plat_make_symlink(target, dst);
            if (r != 0)
                fprintf(stderr, "cp: failed to create symbolic link '%s' -> '%s'\n", dst, target);
        } else {
            fprintf(stderr, "cp: cannot read symlink '%s'\n", src);
            r = -1;
        }
        if (r == 0 && g_opts.preserve)
            plat_preserve_timestamps(src, dst);
        return r;
    }

    /* Reflink (--reflink) */
    if (g_opts.reflink != REFLINK_NEVER) {
        r = plat_copy_file_reflink(src, dst);
        if (r == 0) {
            if (g_opts.preserve)
                plat_preserve_timestamps(src, dst);
            return 0;
        }
        if (g_opts.reflink == REFLINK_ALWAYS) {
            fprintf(stderr, "cp: reflink failed for '%s' -> '%s'\n", src, dst);
            return r;
        }
        /* auto: fall through to regular/sparse copy */
    }

    /* Sparse copy (--sparse) */
    if (g_opts.sparse != SPARSE_NEVER) {
        r = plat_copy_file_sparse(src, dst);
        if (r == 0) {
            if (g_opts.preserve)
                plat_preserve_timestamps(src, dst);
            return 0;
        }
        /* fall through to regular copy */
    }

    /* Regular copy */
    r = plat_copy_file(src, dst);
    if (r == 0 && g_opts.preserve)
        plat_preserve_timestamps(src, dst);
    return r;
}

/* Check entry type without following symlinks (for -P) */
static int entry_is_dir(const char *path) {
    if (g_opts.no_dereference && plat_is_symlink(path))
        return 0;
    return plat_is_dir(path);
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

/* Determine if a directory entry should be skipped (e.g. . and ..) */
static int is_dot_or_dotdot(const char *name) {
    return (strcmp(name, ".") == 0 || strcmp(name, "..") == 0);
}

static void collect_dir(const char *src_dir, const char *dst_dir) {
    check_path_len(src_dir);
    check_path_len(dst_dir);

    plat_dir_t dir;
    plat_dirent_t ent;

    if (plat_dir_open(&dir, src_dir) != 0) {
        fprintf(stderr, "cp: cannot open directory '%s'\n", src_dir);
        return;
    }

    while (plat_dir_next(&dir, &ent) > 0) {
        if (is_dot_or_dotdot(ent.name)) continue;

        char src_path[PATH_MAX_LEN], dst_path[PATH_MAX_LEN];
        join_path(src_path, src_dir, ent.name, PATH_MAX_LEN);
        join_path(dst_path, dst_dir, ent.name, PATH_MAX_LEN);

        if (entry_is_dir(src_path)) {
            if (g_opts.recursive || g_opts.archive) {
                collect_dir(src_path, dst_path);
            } else {
                fprintf(stderr, "cp: omitting directory '%s'\n", src_path);
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
    check_path_len(src);

    /* Check for --parents with absolute paths */
    if (g_opts.parents && src[0] == '/') {
        fprintf(stderr, "cp: cannot combine --parents with absolute paths: '%s'\n", src);
        g_errors++;
        return;
    }

    /* dir / * pattern - copy contents of dir */
    size_t len = strlen(src);
    if (len >= 2 && src[len-1] == '*' && (src[len-2] == '/' || src[len-2] == '\\')) {
        src[len-2] = '\0'; /* strip "/" */
        check_path_len(src);

        if (!plat_is_dir(src)) {
            fprintf(stderr, "cp: '%s' is not a directory\n", src);
            g_errors++;
            return;
        }

        char eff_dst[PATH_MAX_LEN];
        if (g_opts.parents)
            join_path(eff_dst, dst_base, src, PATH_MAX_LEN);
        else
            strncpy(eff_dst, dst_base, PATH_MAX_LEN-1);
        eff_dst[PATH_MAX_LEN-1] = '\0';
        check_path_len(eff_dst);

        collect_dir(src, eff_dst);
        return;
    }

    /* Glob in the basename */
    const char *base = basename_of(src);
    if (has_glob(base)) {
        char dir[PATH_MAX_LEN];
        dirname_of(dir, src, PATH_MAX_LEN);
        check_path_len(dir);

        plat_dir_t d;
        plat_dirent_t ent;
        if (plat_dir_open(&d, dir) != 0) {
            fprintf(stderr, "cp: cannot open directory '%s'\n", dir);
            g_errors++;
            return;
        }

        while (plat_dir_next(&d, &ent) > 0) {
            if (is_dot_or_dotdot(ent.name)) continue;
            if (!plat_glob_match(base, ent.name)) continue;

            char src_path[PATH_MAX_LEN], dst_path[PATH_MAX_LEN];
            join_path(src_path, dir, ent.name, PATH_MAX_LEN);
            check_path_len(src_path);

            if (g_opts.parents) {
                join_path(dst_path, dst_base, dir, PATH_MAX_LEN);
                /* append ent.name */
                {
                    char tmp[PATH_MAX_LEN];
                    join_path(tmp, dst_path, ent.name, PATH_MAX_LEN);
                    strncpy(dst_path, tmp, PATH_MAX_LEN-1);
                    dst_path[PATH_MAX_LEN-1] = '\0';
                }
            } else if (dst_is_dir) {
                join_path(dst_path, dst_base, ent.name, PATH_MAX_LEN);
            } else {
                strncpy(dst_path, dst_base, PATH_MAX_LEN-1);
                dst_path[PATH_MAX_LEN-1] = '\0';
            }
            check_path_len(dst_path);

            if (plat_is_dir(src_path)) {
                if (g_opts.recursive || g_opts.archive) {
                    collect_dir(src_path, dst_path);
                } else {
                    fprintf(stderr, "cp: omitting directory '%s'\n", src_path);
                }
            } else {
                add_file(src_path, dst_path);
            }
        }
        plat_dir_close(&d);
        return;
    }

    /* Plain path — file or directory */
    if (plat_is_dir(src)) {
        if (!(g_opts.recursive || g_opts.archive)) {
            fprintf(stderr, "cp: omitting directory '%s' (use -r or -R)\n", src);
            g_errors++;
            return;
        }
        char dst_dir[PATH_MAX_LEN];
        if (g_opts.parents)
            join_path(dst_dir, dst_base, src, PATH_MAX_LEN);
        else if (dst_is_dir)
            join_path(dst_dir, dst_base, basename_of(src), PATH_MAX_LEN);
        else
            strncpy(dst_dir, dst_base, PATH_MAX_LEN-1);
        dst_dir[PATH_MAX_LEN-1] = '\0';
        check_path_len(dst_dir);

        collect_dir(src, dst_dir);
    } else if (plat_is_file(src)) {
        char dst_path[PATH_MAX_LEN];
        if (g_opts.parents)
            join_path(dst_path, dst_base, src, PATH_MAX_LEN);
        else if (dst_is_dir)
            join_path(dst_path, dst_base, basename_of(src), PATH_MAX_LEN);
        else
            strncpy(dst_path, dst_base, PATH_MAX_LEN-1);
        dst_path[PATH_MAX_LEN-1] = '\0';
        check_path_len(dst_path);

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

        /* Same file check */
        if (strcmp(f->src, f->dst) == 0) {
            fprintf(stderr, "cp: '%s' and '%s' are the same file\n", f->src, f->dst);
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
                    snprintf(msg, sizeof(msg), "'%s' not overwritten", f->dst);
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
                        snprintf(msg, sizeof(msg), "'%s' not overwritten", f->dst);
                        print_msg(msg);
                    }
                    if (!g_opts.no_progress && !g_internal)
                        prog_advance(&g_bar, 1, 0);
                    continue;
                }
            }

            /* Backup (-b) — must happen before removal */
            if (g_opts.backup) {
                make_backup(f->dst);
                dst_exists = 0;
            }

            /* Remove existing destination (if not already moved by backup) */
            if (dst_exists) {
                if (g_opts.force) {
                    plat_make_writable(f->dst);
                }
                plat_remove_file(f->dst);
            }
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
            if (g_opts.hard_link)
                snprintf(msg, sizeof(msg), "%s -> %s", f->src, f->dst);
            else if (g_opts.sym_link)
                snprintf(msg, sizeof(msg), "%s -> %s", f->dst, f->src);
            else
                snprintf(msg, sizeof(msg), "'%s' -> '%s'", f->src, f->dst);
            print_msg(msg);
        }

        /* Print path in progress bar (when not verbose) */
        if (!g_opts.no_progress && !g_internal && g_file_count > 1 && !g_opts.verbose) {
            prog_print_path(&g_bar, f->src);
        }

        /* Perform the copy */
        int r = do_copy_operation(f->src, f->dst);
        if (r != 0) {
            fprintf(stderr, "cp: error copying '%s' -> '%s'\n", f->src, f->dst);
            plat_mutex_lock(&g_err_mtx);
            g_errors++;
            plat_mutex_unlock(&g_err_mtx);
        }

        if (!g_opts.no_progress && !g_internal)
            prog_advance(&g_bar, 1, f->size);
    }
    return 0;
}

/* ── Usage & version ──────────────────────────────────────────────── */

static void usage(void) {
    fprintf(stderr,
        "Usage: cp [OPTION]... [-t] DIRECTORY SOURCE...\n"
        "  or:  cp [OPTION]... -T DIRECTORY SOURCE...\n"
        "  or:  cp [OPTION]... SOURCE... DEST\n"
        "  or:  cp [OPTION]... SOURCE DEST\n"
        "\n"
        "Copy (concatenate) files to another file or to a directory.\n"
        "\n"
        "Options:\n"
        "  -a, --archive                 Same as -dR --preserve=all\n"
        "  -b, --backup                  Make a backup of each existing dest file\n"
        "  -d, --no-dereference           Preserve links, same as --no-dereference\n"
        "      --dense                    (ignored, sparse is auto)\n"
        "  -f, --force                    Remove destination if it exists; force overwrite\n"
        "  -H, -L, --dereference         Follow symbolic links\n"
        "  -P, --no-dereference           Don't follow symbolic links\n"
        "  -i, --interactive              Prompt before overwriting\n"
        "  -l, --link                    Hard link instead of copying\n"
        "  -n, --no-clobber               Do not overwrite an existing file\n"
        "  -p, --preserve                 Preserve mode, ownership (if possible),\n"
        "                                 and timestamps\n"
        "      --no-progress               Turn off progress meter\n"
        "      --parents                  Preserve source path structure\n"
        "      --reflink[=WHEN]           Copy-on-write: auto (default), always, never\n"
        "      --sparse[=WHEN]            Sparse: auto (default), always, never\n"
        "  -R, -r, --recursive            Copy directories recursively\n"
        "  -s, --symbolic-link            Make symbolic links instead of copying\n"
        "      --strip                    (unsupported)\n"
        "  -u, --update                   Copy only when source is newer\n"
        "      --usage                    Show usage\n"
        "  -v, --verbose                  Explain what is being done\n"
        "  -j, --jobs=N                   Number of parallel jobs (default: CPU count)\n"
        "      --help                     Show this help and exit\n"
        "      --version                  Output version information and exit\n"
        "  --                             End of options\n"
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
    fprintf(stderr, "cp (tools) %s\n", TOOLS_VERSION);
    fprintf(stderr, "Copyright (C) 2026 Omena0. All rights reserved.\n");
    fprintf(stderr, "License: BSD-3-Clause\n");
}

/* ── Argument parser ──────────────────────────────────────────────── */

static int parse_long_opt(const char *name, const char *value, cp_opts_t *o) {
    if (strcmp(name, "recursive") == 0)       { o->recursive = 1; }
    else if (strcmp(name, "force") == 0)        { o->force = 1; }
    else if (strcmp(name, "no-clobber") == 0)   { o->no_clobber = 1; }
    else if (strcmp(name, "interactive") == 0)  { o->interactive = 1; }
    else if (strcmp(name, "update") == 0)       { o->update_only = 1; }
    else if (strcmp(name, "verbose") == 0)      { o->verbose = 1; }
    else if (strcmp(name, "archive") == 0) {
        o->archive = 1; o->recursive = 1; o->preserve = 1; o->no_dereference = 1;
    }
    else if (strcmp(name, "preserve") == 0)     { o->preserve = 1; }
    else if (strcmp(name, "no-dereference") == 0) { o->no_dereference = 1; o->dereference = 0; }
    else if (strcmp(name, "dereference") == 0)  { o->dereference = 1; o->no_dereference = 0; }
    else if (strcmp(name, "link") == 0)         { o->hard_link = 1; }
    else if (strcmp(name, "symbolic-link") == 0) { o->sym_link = 1; }
    else if (strcmp(name, "jobs") == 0) {
        if (!value) return -1;
        o->jobs = atoi(value);
        if (o->jobs < 1) o->jobs = 1;
    }
    else if (strcmp(name, "reflink") == 0) {
        const char *m = value ? value : "auto";
        if (strcmp(m, "auto") == 0)       o->reflink = REFLINK_AUTO;
        else if (strcmp(m, "always") == 0) o->reflink = REFLINK_ALWAYS;
        else if (strcmp(m, "never") == 0)  o->reflink = REFLINK_NEVER;
        else { fprintf(stderr, "cp: invalid reflink value '%s'\n", m); return -2; }
    }
    else if (strcmp(name, "sparse") == 0) {
        const char *m = value ? value : "auto";
        if (strcmp(m, "auto") == 0)        o->sparse = SPARSE_AUTO;
        else if (strcmp(m, "always") == 0)  o->sparse = SPARSE_ALWAYS;
        else if (strcmp(m, "never") == 0)   o->sparse = SPARSE_NEVER;
        else { fprintf(stderr, "cp: invalid sparse value '%s'\n", m); return -2; }
    }
    else if (strcmp(name, "backup") == 0)   { o->backup = 1; }
    else if (strcmp(name, "parents") == 0)  { o->parents = 1; }
    else if (strcmp(name, "no-progress") == 0) { o->no_progress = 1; }
    else if (strcmp(name, "help") == 0)     { usage(); exit(0); }
    else if (strcmp(name, "version") == 0)  { version(); exit(0); }
    else if (strcmp(name, "usage") == 0)    { usage(); exit(0); }
    else if (strcmp(name, "dense") == 0)    { /* ignored */ }
    else {
        fprintf(stderr, "cp: unrecognized option '--%s'\n", name);
        return -2;
    }
    return 0;
}

/* Parse a short option character. Returns 1 if consumed a value from argv, 0 otherwise. */
static int parse_short_opt(char c, int *pi, int argc, char **argv, cp_opts_t *o) {
    switch (c) {
        case 'r': case 'R': o->recursive = 1; break;
        case 'f': o->force = 1; break;
        case 'n': o->no_clobber = 1; break;
        case 'i': o->interactive = 1; break;
        case 'u': o->update_only = 1; break;
        case 'v': o->verbose = 1; break;
        case 'a':
            o->archive = 1; o->recursive = 1; o->preserve = 1; o->no_dereference = 1;
            break;
        case 'p': o->preserve = 1; break;
        case 'P': o->no_dereference = 1; o->dereference = 0; break;
        case 'L': o->dereference = 1; o->no_dereference = 0; break;
        case 'H': o->follow_cmdline = 1; break;
        case 'l': o->hard_link = 1; break;
        case 's': o->sym_link = 1; break;
        case 'b': o->backup = 1; break;
        case 'j': {
            /* -j N, -jN */
            const char *val = NULL;
            /* Check if value is attached: -j4 */
            /* We're called with the rest of the arg after 'j' */
            /* Actually, we handle -j4 in the caller; here we just need the next arg */
            if (*pi + 1 < argc) {
                val = argv[*pi + 1];
                (*pi)++;
            } else {
                fprintf(stderr, "cp: -j requires an argument\n");
                return -2;
            }
            o->jobs = atoi(val);
            if (o->jobs < 1) o->jobs = 1;
            break;
        }
        case 'd':
            o->no_dereference = 1; o->preserve = 1;
            break;
        default:
            fprintf(stderr, "cp: invalid option -- '%c'\n", c);
            return -2;
    }
    return 0;
}

/* Parse all command-line arguments. Returns a list of positional args. */
static int parse_args(int argc, char **argv, cp_opts_t *o, char *sources[], int *src_count) {
    int end_of_opts = 0;

    for (int i = 1; i < argc; i++) {
        if (!end_of_opts && argv[i][0] == '-' && argv[i][1] != '\0') {
            /* Option argument */
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

                /* Options that take a value and might need next arg */
                if (!val && (strcmp(opt, "jobs") == 0)) {
                    if (i + 1 < argc) {
                        val = argv[++i];
                    } else {
                        fprintf(stderr, "cp: --jobs requires an argument\n");
                        return 1;
                    }
                }

                int r = parse_long_opt(opt, val, o);
                if (r == -2) return 1;
                if (r == -1) {
                    fprintf(stderr, "cp: --%s requires an argument\n", opt);
                    return 1;
                }
            } else {
                /* Short option(s) — may be combined like -rf, -rvs */
                const char *p = argv[i] + 1;
                while (*p) {
                    char c = *p;
                    if (c == 'j') {
                        /* -j with attached value: -j4 */
                        p++;
                        if (*p) {
                            o->jobs = atoi(p);
                            if (o->jobs < 1) o->jobs = 1;
                            break; /* rest of arg is the value */
                        } else {
                            /* -j with space-separated value */
                            int consumed = parse_short_opt('j', &i, argc, argv, o);
                            if (consumed == -2) return 1;
                            p = ""; /* exit the while loop */
                        }
                    } else {
                        int consumed = parse_short_opt(c, &i, argc, argv, o);
                        if (consumed == -2) return 1;
                        /* c options like -j that consume next arg would set i */
                        p++;
                    }
                }
            }
        } else if (argv[i][0] == '-' && argv[i][1] == '\0') {
            /* "-" — treat as a regular file (stdin) */
            if (*src_count < MAX_SOURCES)
                sources[(*src_count)++] = argv[i];
        } else {
            /* Positional argument */
            if (*src_count < MAX_SOURCES)
                sources[(*src_count)++] = argv[i];
            /* POSIX: stop parsing at first non-option. But GNU permutes.
               For simplicity, we stop parsing options at first non-option. */
            end_of_opts = 1;
        }
    }

    return 0;
}

/* ── Main ─────────────────────────────────────────────────────────── */

int main(int argc, char **argv) {
    /* Check TOOLS_INTERNAL env var */
    const char *internal_env = getenv("TOOLS_INTERNAL");
    if (internal_env && strcmp(internal_env, "1") == 0)
        g_internal = 1;

    /* Defaults */
    g_opts.jobs     = plat_cpu_count();
    g_opts.reflink  = REFLINK_AUTO;  /* default: try reflink, fall back */
    g_opts.sparse   = SPARSE_AUTO;   /* default: sparse-aware */

    /* Parse arguments */
    char *sources[MAX_SOURCES];
    int src_count = 0;

    if (parse_args(argc, argv, &g_opts, sources, &src_count) != 0)
        return 1;

    if (src_count < 2) {
        usage();
        return 1;
    }

    /* Apply archive implications */
    if (g_opts.archive) {
        g_opts.recursive = 1;
        g_opts.preserve = 1;
        g_opts.no_dereference = 1;
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
                fprintf(stderr, "cp: cannot create directory '%s'\n", dst);
                return 1;
            }
        }
    }

    /* If multiple sources, destination must be a directory */
    if (src_count > 1 && !dst_is_dir) {
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

    /* Determine thread count */
    int n_threads = g_opts.jobs;
    if (n_threads > MAX_THREADS) n_threads = MAX_THREADS;
    if (n_threads > g_file_count) n_threads = g_file_count;
    if (n_threads < 1) n_threads = 1;

    if (n_threads == 1) {
        cp_worker(NULL);
    } else {
        plat_thread_t threads[MAX_THREADS];
        for (int i = 0; i < n_threads; i++)
            plat_thread_create(&threads[i], cp_worker, NULL);
        for (int i = 0; i < n_threads; i++)
            plat_thread_join(threads[i]);
    }

    if (!g_opts.no_progress && !g_internal) {
        prog_finish(&g_bar);
    }

    plat_mutex_destroy(&g_idx_mtx);
    plat_mutex_destroy(&g_err_mtx);
    plat_mutex_destroy(&g_io_mtx);

    return g_errors ? 1 : 0;
}
