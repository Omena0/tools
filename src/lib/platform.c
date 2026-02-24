/*
 * platform.c - Cross-platform abstraction layer
 * Include this file directly in your .c files (single-header style).
 */

#ifndef PLATFORM_C
#define PLATFORM_C

#ifndef _WIN32
  #define _GNU_SOURCE
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
  #define WIN32_LEAN_AND_MEAN
  #include <windows.h>
  #include <direct.h>
  #include <io.h>
  #include <sys/stat.h>
  #include <sys/types.h>

  #define PLAT_WINDOWS 1
  #define PLAT_UNIX    0
  #define PATH_SEP     '\\'
  #define PATH_SEP_STR "\\"
  #define PATH_MAX_LEN MAX_PATH

  typedef struct _stat64 plat_stat_t;
  #define plat_stat(p, s) _stat64(p, s)
  #define plat_mkdir(p)   _mkdir(p)

#else
  #include <unistd.h>
  #include <dirent.h>
  #include <sys/stat.h>
  #include <sys/types.h>
  #include <errno.h>
  #include <fnmatch.h>
  #include <limits.h>
  #include <pthread.h>
  #include <time.h>
  #include <sys/ioctl.h>

  #define PLAT_WINDOWS 0
  #define PLAT_UNIX    1
  #define PATH_SEP     '/'
  #define PATH_SEP_STR "/"
  #define PATH_MAX_LEN PATH_MAX

  typedef struct stat plat_stat_t;
  #define plat_stat(p, s) stat(p, s)
  #define plat_mkdir(p)   mkdir(p, 0755)

#endif

/* ── Thread abstraction ───────────────────────────────────────────── */

#ifdef _WIN32

typedef HANDLE plat_thread_t;
typedef CRITICAL_SECTION plat_mutex_t;

static inline int plat_thread_create(plat_thread_t *t, DWORD (WINAPI *fn)(void*), void *arg) {
    *t = CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)fn, arg, 0, NULL);
    return *t == NULL ? -1 : 0;
}
static inline void plat_thread_join(plat_thread_t t) {
    WaitForSingleObject(t, INFINITE);
    CloseHandle(t);
}
static inline void plat_mutex_init(plat_mutex_t *m) { InitializeCriticalSection(m); }
static inline void plat_mutex_lock(plat_mutex_t *m) { EnterCriticalSection(m); }
static inline void plat_mutex_unlock(plat_mutex_t *m) { LeaveCriticalSection(m); }
static inline void plat_mutex_destroy(plat_mutex_t *m) { DeleteCriticalSection(m); }

#define PLAT_THREAD_FUNC(name, arg) DWORD WINAPI name(void *arg)

#else

typedef pthread_t plat_thread_t;
typedef pthread_mutex_t plat_mutex_t;

static inline int plat_thread_create(plat_thread_t *t, void *(*fn)(void*), void *arg) {
    return pthread_create(t, NULL, fn, arg);
}
static inline void plat_thread_join(plat_thread_t t) {
    pthread_join(t, NULL);
}
static inline void plat_mutex_init(plat_mutex_t *m) { pthread_mutex_init(m, NULL); }
static inline void plat_mutex_lock(plat_mutex_t *m) { pthread_mutex_lock(m); }
static inline void plat_mutex_unlock(plat_mutex_t *m) { pthread_mutex_unlock(m); }
static inline void plat_mutex_destroy(plat_mutex_t *m) { pthread_mutex_destroy(m); }

#define PLAT_THREAD_FUNC(name, arg) void *name(void *arg)

#endif

/* ── Terminal helpers ─────────────────────────────────────────────── */

static int plat_terminal_width(void) {
#ifdef _WIN32
    CONSOLE_SCREEN_BUFFER_INFO csbi;
    if (GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &csbi))
        return csbi.srWindow.Right - csbi.srWindow.Left + 1;
    return 80;
#else
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0)
        return ws.ws_col;
    return 80;
#endif
}

/* ── Time helpers ─────────────────────────────────────────────────── */

/* Returns current time in seconds (monotonic, high resolution) */
static double plat_time(void) {
#ifdef _WIN32
    static LARGE_INTEGER freq = {0};
    LARGE_INTEGER counter;
    if (freq.QuadPart == 0) QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&counter);
    return (double)counter.QuadPart / (double)freq.QuadPart;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
#endif
}

static void plat_sleep_ms(int ms) {
#ifdef _WIN32
    Sleep(ms);
#else
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
#endif
}

/* ── Filesystem helpers ───────────────────────────────────────────── */

static const char *plat_home_dir(void) {
#ifdef _WIN32
    static char buf[PATH_MAX_LEN];
    const char *h = getenv("USERPROFILE");
    if (h) { strncpy(buf, h, sizeof(buf)-1); return buf; }
    const char *drive = getenv("HOMEDRIVE");
    const char *path = getenv("HOMEPATH");
    if (drive && path) { snprintf(buf, sizeof(buf), "%s%s", drive, path); return buf; }
    return "C:\\";
#else
    const char *h = getenv("HOME");
    return h ? h : "/";
#endif
}

static int plat_is_dir(const char *path) {
    plat_stat_t st;
    if (plat_stat(path, &st) != 0) return 0;
#ifdef _WIN32
    return (st.st_mode & _S_IFDIR) != 0;
#else
    return S_ISDIR(st.st_mode);
#endif
}

static int plat_is_file(const char *path) {
    plat_stat_t st;
    if (plat_stat(path, &st) != 0) return 0;
#ifdef _WIN32
    return (st.st_mode & _S_IFREG) != 0;
#else
    return S_ISREG(st.st_mode);
#endif
}

static long long plat_file_size(const char *path) {
    plat_stat_t st;
    if (plat_stat(path, &st) != 0) return -1;
    return (long long)st.st_size;
}

static int plat_path_exists(const char *path) {
    plat_stat_t st;
    return plat_stat(path, &st) == 0;
}

static int plat_mkdir_p(const char *path) {
    char tmp[PATH_MAX_LEN];
    size_t len;
    strncpy(tmp, path, sizeof(tmp)-1);
    tmp[sizeof(tmp)-1] = '\0';
    len = strlen(tmp);
    if (len > 0 && (tmp[len-1] == '/' || tmp[len-1] == '\\'))
        tmp[len-1] = '\0';
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/' || *p == '\\') {
            *p = '\0';
            if (!plat_path_exists(tmp))
                plat_mkdir(tmp);
            *p = PATH_SEP;
        }
    }
    if (!plat_path_exists(tmp))
        return plat_mkdir(tmp);
    return 0;
}

/* Copy a single file. Returns 0 on success. */
static int plat_copy_file(const char *src, const char *dst) {
#ifdef _WIN32
    return CopyFileA(src, dst, FALSE) ? 0 : -1;
#else
    FILE *in = fopen(src, "rb");
    if (!in) return -1;
    FILE *out = fopen(dst, "wb");
    if (!out) { fclose(in); return -1; }

    char buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
        if (fwrite(buf, 1, n, out) != n) {
            fclose(in); fclose(out);
            return -1;
        }
    }
    /* Preserve permissions */
    plat_stat_t st;
    if (plat_stat(src, &st) == 0)
        chmod(dst, st.st_mode);

    fclose(in);
    fclose(out);
    return 0;
#endif
}

/* Move / rename a file. Falls back to copy+delete across filesystems. */
static int plat_move_file(const char *src, const char *dst) {
#ifdef _WIN32
    if (MoveFileExA(src, dst, MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED))
        return 0;
    return -1;
#else
    if (rename(src, dst) == 0) return 0;
    if (errno == EXDEV) {
        if (plat_copy_file(src, dst) == 0) {
            unlink(src);
            return 0;
        }
    }
    return -1;
#endif
}

/* Remove a file */
static int plat_remove_file(const char *path) {
#ifdef _WIN32
    return DeleteFileA(path) ? 0 : -1;
#else
    return unlink(path);
#endif
}

/* ── Directory iteration ──────────────────────────────────────────── */

typedef struct {
    char name[PATH_MAX_LEN];
    int  is_dir;
} plat_dirent_t;

typedef struct {
#ifdef _WIN32
    HANDLE           handle;
    WIN32_FIND_DATAA fdata;
    int              first;
#else
    DIR *dir;
#endif
} plat_dir_t;

static int plat_dir_open(plat_dir_t *d, const char *path) {
#ifdef _WIN32
    char pat[PATH_MAX_LEN];
    snprintf(pat, sizeof(pat), "%s\\*", path);
    d->handle = FindFirstFileA(pat, &d->fdata);
    d->first = 1;
    return d->handle == INVALID_HANDLE_VALUE ? -1 : 0;
#else
    d->dir = opendir(path);
    return d->dir ? 0 : -1;
#endif
}

/* Returns 1 if entry read, 0 at end, -1 on error */
static int plat_dir_next(plat_dir_t *d, plat_dirent_t *ent) {
#ifdef _WIN32
    while (1) {
        if (!d->first) {
            if (!FindNextFileA(d->handle, &d->fdata)) return 0;
        }
        d->first = 0;
        if (strcmp(d->fdata.cFileName, ".") == 0 || strcmp(d->fdata.cFileName, "..") == 0)
            continue;
        strncpy(ent->name, d->fdata.cFileName, PATH_MAX_LEN-1);
        ent->is_dir = (d->fdata.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        return 1;
    }
#else
    struct dirent *e;
    while ((e = readdir(d->dir)) != NULL) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
            continue;
        strncpy(ent->name, e->d_name, PATH_MAX_LEN-1);
        ent->is_dir = (e->d_type == DT_DIR);
        if (e->d_type == DT_UNKNOWN) {
            ent->is_dir = 0; /* conservative fallback */
        }
        return 1;
    }
    return 0;
#endif
}

static void plat_dir_close(plat_dir_t *d) {
#ifdef _WIN32
    FindClose(d->handle);
#else
    closedir(d->dir);
#endif
}

/* ── Glob / pattern matching ──────────────────────────────────────── */

/* Simple glob match: supports * and ? wildcards */
static int plat_glob_match(const char *pattern, const char *str) {
    while (*pattern) {
        if (*pattern == '*') {
            pattern++;
            if (*pattern == '\0') return 1;
            for (; *str; str++) {
                if (plat_glob_match(pattern, str)) return 1;
            }
            return plat_glob_match(pattern, str);
        } else if (*pattern == '?') {
            if (*str == '\0') return 0;
            pattern++;
            str++;
        } else {
            if (*str != *pattern) return 0;
            pattern++;
            str++;
        }
    }
    return *str == '\0';
}

/* ── CPU core count ───────────────────────────────────────────────── */

static int plat_cpu_count(void) {
#ifdef _WIN32
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    return (int)si.dwNumberOfProcessors;
#else
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return n > 0 ? (int)n : 1;
#endif
}

#endif /* PLATFORM_C */