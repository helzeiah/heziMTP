/* libusb config.h for macOS (Darwin) — arm64 + x86_64, macOS 12+ */
#pragma once

/* clock_gettime available since macOS 10.12 */
#define HAVE_CLOCK_GETTIME 1

/* pthread_condattr_setclock(CLOCK_MONOTONIC) is NOT available on macOS.
   Must be undefined (not 0) so #ifdef checks are false. */
#undef HAVE_PTHREAD_CONDATTR_SETCLOCK

/* timerfd is Linux-only — must be undefined, not 0 */
#undef HAVE_TIMERFD

/* Standard POSIX headers present on macOS */
#define HAVE_SYS_TIME_H 1
#define HAVE_DLFCN_H 1
#define HAVE_INTTYPES_H 1
#define HAVE_MEMORY_H 1
#define HAVE_STDINT_H 1
#define HAVE_STDLIB_H 1
#define HAVE_STRING_H 1
#define HAVE_STRINGS_H 1
#define HAVE_SYS_STAT_H 1
#define HAVE_SYS_TYPES_H 1
#define HAVE_UNISTD_H 1
#define STDC_HEADERS 1

#define PLATFORM_POSIX 1

/* Visibility / printf format attributes */
#define DEFAULT_VISIBILITY           __attribute__((visibility("default")))
#define PRINTF_FORMAT(a, b)          __attribute__((__format__(__printf__, a, b)))

/* 64-bit */
#define SIZEOF_VOID_P 8

/* Logging enabled (not debug) */
#define ENABLE_LOGGING 1
#undef  ENABLE_DEBUG_LOGGING
