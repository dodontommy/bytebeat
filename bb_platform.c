/* bb_platform.c -- the one file allowed to know which operating system this
 * is. See bb_platform.h for what each function promises; this file is about
 * HOW, and about the several places where the obvious implementation is
 * quietly wrong.
 */

/* getenv, fopen and friends are on MSVC's "consider the _s variant" list.
 * They are not unsafe as used here, and the _s variants do not exist anywhere
 * else, so silence the advice rather than fork the code. Must be defined
 * before any CRT header is pulled in. */
#if defined(_WIN32) && !defined(_CRT_SECURE_NO_WARNINGS)
#  define _CRT_SECURE_NO_WARNINGS 1
#endif

#include "bb_platform.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)

#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN 1
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX 1
#  endif
#  include <windows.h>
#  include <direct.h>   /* _wmkdir  */
#  include <wchar.h>

/* ------------------------------------------------------------------------ */
/*  UTF-8 -> UTF-16                                                          */
/*                                                                           */
/*  Every path that crosses into Win32 goes through here. The result is       */
/*  heap-allocated and the caller frees it; that is fine because none of the  */
/*  callers are on the audio thread (bb_now_us, which is, never widens        */
/*  anything). A stack buffer with a heap fallback was tempting and was        */
/*  rejected: paths in this program can be ARR_PATH_MAX long, nested under a  */
/*  user-chosen root, and a truncated path is a data-loss bug rather than a   */
/*  cosmetic one.                                                             */
/* ------------------------------------------------------------------------ */
static wchar_t *bb_widen(const char *utf8)
{
    if (!utf8) return NULL;

    /* -1 for the length asks MultiByteToWideChar to include the terminator in
     * both the measurement and the conversion, so `n` counts the NUL too. */
    int n = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, NULL, 0);
    if (n <= 0) return NULL;

    wchar_t *w = (wchar_t *)malloc((size_t)n * sizeof *w);
    if (!w) return NULL;

    if (MultiByteToWideChar(CP_UTF8, 0, utf8, -1, w, n) != n) {
        free(w);
        return NULL;
    }
    return w;
}

static int bb_is_sep(wchar_t c) { return c == L'\\' || c == L'/'; }

/* ---- clock --------------------------------------------------------------- */

uint64_t bb_now_us(void)
{
    /* QueryPerformanceFrequency is fixed for the life of the process -- it has
     * been documented as such since Windows XP -- so read it once and keep it.
     * The unsynchronised access to `freq` is a benign race: two threads racing
     * to initialise it both write the same value, and a thread that observes
     * the zero simply asks the kernel again.
     *
     * QueryPerformanceCounter itself is a user-mode read of an invariant TSC
     * on any machine this program will ever run on -- no syscall, no lock, no
     * allocation -- which is what lets bb_engine_render() call it. */
    static LARGE_INTEGER freq;
    if (freq.QuadPart == 0) {
        if (!QueryPerformanceFrequency(&freq)) return 0;
    }
    if (freq.QuadPart <= 0) return 0;

    LARGE_INTEGER now;
    if (!QueryPerformanceCounter(&now)) return 0;

    /* Split the conversion instead of writing (ticks * 1000000) / freq. The
     * counter is nanosecond-resolution on modern hardware and counts from
     * boot; multiplying it by a million overflows 64 bits after roughly five
     * hours of uptime, at which point the CPU meter would start reporting
     * nonsense. Whole seconds first, remainder second: exact, and it cannot
     * overflow until the machine has been up for close to 600,000 years. */
    uint64_t t = (uint64_t)now.QuadPart;
    uint64_t f = (uint64_t)freq.QuadPart;
    return (t / f) * 1000000ull + ((t % f) * 1000000ull) / f;
}

/* ---- directories --------------------------------------------------------- */

int bb_mkdirs(const char *utf8_path)
{
    wchar_t *w = bb_widen(utf8_path);
    if (!w) return -1;

    size_t n = wcslen(w);
    if (n == 0) { free(w); return -1; }

    /* Work out where the first CREATABLE component starts. Everything before
     * it is a root of some kind, and asking to create a root fails.
     *
     *   "C:\Users\x"        -> skip "C:" and the separator after it
     *   "\\server\share\x"  -> skip the server and the share; neither is a
     *                          directory we could make even in principle
     *   "\x" / "x"          -> nothing to skip
     */
    size_t start = 0;
    if (n >= 2 && w[1] == L':') {
        start = 2;
    } else if (n >= 2 && bb_is_sep(w[0]) && bb_is_sep(w[1])) {
        size_t i = 2;
        int crossed = 0;                      /* separators seen after "\\" */
        while (i < n && crossed < 2) {
            if (bb_is_sep(w[i])) crossed++;
            if (crossed < 2) i++;
        }
        start = i;
    }
    while (start < n && bb_is_sep(w[start])) start++;

    /* Now walk the components, terminating the string at each separator in
     * turn and creating the prefix. Empty components ("a\\\\b", or a trailing
     * separator) are skipped rather than treated as an error, because callers
     * build these paths by concatenation and a doubled separator means the
     * same directory to every Windows API. */
    int rc = 0;
    size_t seg = start;
    for (size_t i = start; i <= n; i++) {
        if (i < n && !bb_is_sep(w[i])) continue;
        if (i > seg) {
            wchar_t save = w[i];
            w[i] = L'\0';
            if (_wmkdir(w) != 0 && errno != EEXIST) rc = -1;
            w[i] = save;
        }
        seg = i + 1;
    }

    free(w);
    return rc;
}

/* ---- atomic replace ------------------------------------------------------ */

int bb_replace_atomic(const char *utf8_tmp, const char *utf8_dst)
{
    wchar_t *wt = bb_widen(utf8_tmp);
    wchar_t *wd = bb_widen(utf8_dst);
    int rc = -1;

    if (wt && wd) {
        /* MOVEFILE_REPLACE_EXISTING is the whole reason this function exists:
         * without it Win32 refuses when the destination is there, exactly like
         * the CRT's rename().
         *
         * MOVEFILE_WRITE_THROUGH makes MoveFileExW wait until the directory
         * change is on the medium before returning. The caller has already
         * pushed the file's CONTENTS out (see bb_config_save), so between the
         * two of them the "either the old session or the new one, never a
         * mixture" promise actually survives a power cut. */
        rc = MoveFileExW(wt, wd,
                         MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)
             ? 0 : -1;
    }

    free(wt);
    free(wd);
    return rc;
}

/* ---- files --------------------------------------------------------------- */

FILE *bb_fopen(const char *utf8_path, const char *mode)
{
    if (!utf8_path || !mode) return NULL;

    wchar_t *wp = bb_widen(utf8_path);
    /* The mode string needs widening too -- _wfopen takes wide arguments for
     * both. It is always plain ASCII, but running it through the same helper
     * is shorter than open-coding a copy and cannot disagree with it. */
    wchar_t *wm = bb_widen(mode);

    FILE *f = (wp && wm) ? _wfopen(wp, wm) : NULL;

    free(wp);
    free(wm);
    return f;
}

int bb_remove(const char *utf8_path)
{
    wchar_t *w = bb_widen(utf8_path);
    if (!w) return -1;
    int rc = _wremove(w);
    free(w);
    return rc;
}

#else /* ---------------------------------------------------------------- */

#  include <sys/stat.h>
#  include <sys/types.h>
#  include <time.h>
#  include <stdio.h>

/* ---- clock --------------------------------------------------------------- */

uint64_t bb_now_us(void)
{
    /* CLOCK_MONOTONIC, not CLOCK_REALTIME: the CPU meter measures an interval,
     * and CLOCK_REALTIME can step sideways when NTP disciplines it, which
     * would show up as an impossible render time or a negative one. */
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)(ts.tv_nsec / 1000);
}

/* ---- directories --------------------------------------------------------- */

int bb_mkdirs(const char *utf8_path)
{
    if (!utf8_path || !*utf8_path) return -1;

    /* A private copy so we can punch temporary NULs into it. strdup rather
     * than a fixed buffer: this is the UI thread, and silently truncating a
     * path is how a session ends up written somewhere nobody looks. */
    size_t n = strlen(utf8_path);
    char *p = (char *)malloc(n + 1);
    if (!p) return -1;
    memcpy(p, utf8_path, n + 1);

    /* A leading '/' is the root; it exists by definition and mkdir("/") would
     * fail with EEXIST anyway, so start past it. */
    size_t seg = 0;
    while (seg < n && p[seg] == '/') seg++;

    int rc = 0;
    for (size_t i = seg; i <= n; i++) {
        if (i < n && p[i] != '/') continue;
        if (i > seg) {
            char save = p[i];
            p[i] = '\0';
            if (mkdir(p, 0755) != 0 && errno != EEXIST) rc = -1;
            p[i] = save;
        }
        seg = i + 1;
    }

    free(p);
    return rc;
}

/* ---- atomic replace ------------------------------------------------------ */

int bb_replace_atomic(const char *utf8_tmp, const char *utf8_dst)
{
    /* POSIX rename() already replaces an existing destination atomically.
     * This is the whole implementation on this side of the #if; the function
     * exists for the benefit of the other side. */
    return rename(utf8_tmp, utf8_dst) == 0 ? 0 : -1;
}

/* ---- files --------------------------------------------------------------- */

FILE *bb_fopen(const char *utf8_path, const char *mode)
{
    /* Unix filenames are bytes and the locale is UTF-8 everywhere that
     * matters, so a UTF-8 path IS the path. Nothing to translate. */
    if (!utf8_path || !mode) return NULL;
    return fopen(utf8_path, mode);
}

int bb_remove(const char *utf8_path)
{
    if (!utf8_path) return -1;
    return remove(utf8_path);
}

#endif /* _WIN32 */
