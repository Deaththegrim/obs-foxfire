/* Foxfire -- the handful of places MSVC and POSIX disagree, in one file.
 *
 * Scope is deliberately tiny. Most of what looks non-portable in this codebase is not:
 * <pthread.h> is fine because OBS's Windows dependency set ships one (util/threading.h
 * includes it unconditionally, right under an #ifndef _MSC_VER, so it is plainly expected
 * to be compiled by MSVC), and <sys/stat.h> and <fcntl.h> exist there too. What genuinely
 * has no MSVC equivalent under the same name is collected here so no caller has to carry
 * an #ifdef of its own -- three shims, one place, rather than three spellings that drift.
 *
 * Every one of these was added because the Windows build FAILED on it. Do not add a shim
 * here speculatively: a guard for a problem that does not exist is a lie about the platform
 * that the next reader has to disprove.
 */
#pragma once

#include <time.h>
#include <stdbool.h>
#include <string.h>

/* MSVC has no __attribute__. libobs solves this exactly this way in util/base.h, then
   #undefs its macro, so we cannot borrow it -- the same three lines, under our own name.
   It is worth keeping rather than dropping the attribute everywhere: the format check is
   what catches a %s handed an int, which on a logging path is a crash in the field and
   nothing at all in a test. */
#if !defined(_MSC_VER)
#define FF_PRINTF(fmt_index, first_arg) __attribute__((__format__(__printf__, fmt_index, first_arg)))
#else
#define FF_PRINTF(fmt_index, first_arg)
#endif

/* MSVC's strtok_s takes (str, delim, &ctx) and behaves exactly as POSIX strtok_r -- it is
   NOT the four-argument C11 Annex K function of the same name, which Microsoft documents as
   a deliberate divergence. So this is a rename and nothing more. */
#if defined(_MSC_VER)
#define ff_strtok_r strtok_s
#else
#define ff_strtok_r strtok_r
#endif

/* localtime_s is NOT localtime_r with a different name: the arguments are reversed and it
   returns errno_t (0 on success) rather than the struct. Wrapping it in a bool keeps that
   asymmetry from reaching any caller. */
static inline bool ff_localtime(const time_t *t, struct tm *out)
{
#if defined(_MSC_VER)
	return localtime_s(out, t) == 0;
#else
	return localtime_r(t, out) != NULL;
#endif
}
