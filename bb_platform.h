/* bb_platform.h -- the five things the engine needs from an operating system.
 *
 * The engine is deliberately device-independent (see engine.h) but it is not,
 * and cannot be, OS-independent: it has to name a directory, put a file in it,
 * replace an older file with a newer one without ever showing anyone a
 * half-written session, and read a monotonic clock from the audio thread.
 * Those four things are the ONLY places the instrument touches the platform,
 * and each of them is spelled differently on Windows than it is on the unices.
 *
 * Rather than sprinkle `#if defined(_WIN32)` through 2700 lines of engine.c,
 * everything platform-shaped lives behind these five calls. engine.c stays
 * readable; bb_platform.c is the one file where the differences are allowed to
 * show.
 *
 * TWO RULES apply to every function here.
 *
 *   1. Paths are UTF-8, in and out, on every platform. This is not pedantry.
 *      Windows' narrow CRT (fopen, mkdir, remove, rename) interprets a `char*`
 *      path in the process's ANSI codepage, which on a typical machine is
 *      Windows-1252 -- so a user called "Bj\u00f6rk" or "\u4e2d\u6751" gets a
 *      MORGUE that cannot open its own session file, with no error anyone can
 *      act on. Everything here widens to UTF-16 and calls the `_w` variant, so
 *      the only encoding the rest of the program ever has to think about is
 *      UTF-8.
 *
 *   2. bb_now_us() is called from bb_engine_render(), so it obeys the hard
 *      realtime rule: it does not allocate, does not lock, does not block.
 *      The other four are called from the UI thread only.
 */
#ifndef BB_PLATFORM_H
#define BB_PLATFORM_H

#include <stdio.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The separator to use when the program BUILDS a path. Both platforms accept
 * '/' when they READ one -- the Win32 API has taken forward slashes since
 * NT -- but a path that mixes them ("C:\Users\tommy/session.conf") is ugly in
 * a log, ugly in an error dialog, and confuses anyone who pastes it into a
 * shell. Joining with the native separator costs nothing and keeps every path
 * the user ever sees looking like it belongs to the machine it is on. */
#if defined(_WIN32)
#  define BB_PATH_SEP "\\"
#else
#  define BB_PATH_SEP "/"
#endif

/* Create `utf8_path` and every missing directory above it. Returns 0 if the
 * whole chain exists afterwards, non-zero if some level could not be made.
 * A level that already exists is not an error.
 *
 * On Windows this accepts BOTH '/' and '\' as separators (paths arrive from
 * several directions -- the GUI, a config file, a drag-and-drop -- and they do
 * not agree), and it skips the drive-letter prefix, because "C:" is not a
 * directory and asking to create it is an error, not a no-op. */
int bb_mkdirs(const char *utf8_path);

/* Rename `utf8_tmp` onto `utf8_dst`, replacing `utf8_dst` if it exists.
 * Returns 0 on success.
 *
 * This exists because POSIX rename() and Windows rename() are NOT the same
 * function. POSIX rename() atomically replaces an existing destination;
 * Windows' fails with EEXIST. Every write-to-temp-then-rename in this program
 * therefore worked exactly once on Windows -- the first save, when there was
 * nothing to replace -- and silently failed forever after. */
int bb_replace_atomic(const char *utf8_tmp, const char *utf8_dst);

/* Microseconds from an unspecified but monotonic origin: never runs backwards,
 * never jumps when someone changes the wall clock or a leap second lands.
 * Only differences between two readings are meaningful.
 *
 * Safe to call from the audio thread. */
uint64_t bb_now_us(void);

/* fopen() with a UTF-8 path. `mode` is an ordinary stdio mode string.
 * Returns NULL on failure, exactly like fopen(). */
FILE *bb_fopen(const char *utf8_path, const char *mode);

/* remove() with a UTF-8 path. Returns 0 on success. */
int bb_remove(const char *utf8_path);

#ifdef __cplusplus
}
#endif

#endif /* BB_PLATFORM_H */
