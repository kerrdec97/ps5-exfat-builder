/*
 * Build configuration of the libarchive sources the app compiles
 * (cmake/ArchiveLibs.cmake): the hand-written equivalent of the config.h that
 * libarchive's own build would generate, passed through PLATFORM_CONFIG_H.
 *
 * Only what the 7z, RAR and RAR5 readers (and, for the tests, the 7z writer)
 * need is switched on: liblzma, zlib and zstd; no crypto library (encrypted
 * archives are refused, never decrypted), no iconv, no ACL or extended
 * attribute support, no XML.
 *
 * The Windows values are those libarchive 3.8.11's CMake project detects with
 * MSVC 2022 for x64; the others follow libarchive's own hand-built Linux
 * configuration (contrib/android/config/linux_host.h), reduced to what
 * POSIX.1-2008 guarantees.
 */
#ifndef FW_LIBARCHIVE_CONFIG_H
#define FW_LIBARCHIVE_CONFIG_H

#define __LIBARCHIVE_CONFIG_H_INCLUDED 1

#define LIBARCHIVE_VERSION_NUMBER "3008011"
#define LIBARCHIVE_VERSION_STRING "3.8.11"
#define VERSION "3.8.11"

/* Compression libraries. */
#define HAVE_LIBLZMA 1
#define HAVE_LZMA_H 1
#define HAVE_LIBZ 1
#define HAVE_ZLIB_H 1
#define HAVE_LIBZSTD 1
#define HAVE_ZSTD_H 1

/* C99 everywhere. */
#define HAVE_CTYPE_H 1
#define HAVE_DECL_INT32_MAX 1
#define HAVE_DECL_INT32_MIN 1
#define HAVE_DECL_INT64_MAX 1
#define HAVE_DECL_INT64_MIN 1
#define HAVE_DECL_INTMAX_MAX 1
#define HAVE_DECL_INTMAX_MIN 1
#define HAVE_DECL_SIZE_MAX 1
#define HAVE_DECL_UINT32_MAX 1
#define HAVE_DECL_UINT64_MAX 1
#define HAVE_DECL_UINTMAX_MAX 1
#define HAVE_EILSEQ 1
#define HAVE_ERRNO_H 1
#define HAVE_FCNTL_H 1
#define HAVE_FSTAT 1
#define HAVE_GETPID 1
#define HAVE_INTMAX_T 1
#define HAVE_INTTYPES_H 1
#define HAVE_LIMITS_H 1
#define HAVE_LOCALE_H 1
#define HAVE_MBRTOWC 1
#define HAVE_MKDIR 1
#define HAVE_SETLOCALE 1
#define HAVE_SIGNAL_H 1
#define HAVE_STDARG_H 1
#define HAVE_STDINT_H 1
#define HAVE_STDLIB_H 1
#define HAVE_STRCHR 1
#define HAVE_STRDUP 1
#define HAVE_STRERROR 1
#define HAVE_STRFTIME 1
#define HAVE_STRING_H 1
#define HAVE_STRNLEN 1
#define HAVE_STRRCHR 1
#define HAVE_SYS_STAT_H 1
#define HAVE_SYS_TYPES_H 1
#define HAVE_TIME_H 1
#define HAVE_TZSET 1
#define HAVE_UINTMAX_T 1
#define HAVE_UTIME 1
#define HAVE_WCHAR_H 1
#define HAVE_WCHAR_T 1
#define HAVE_WCRTOMB 1
#define HAVE_WCSCMP 1
#define HAVE_WCSCPY 1
#define HAVE_WCSLEN 1
#define HAVE_WCTOMB 1
#define HAVE_WCTYPE_H 1
#define ICONV_CONST

#if defined(_WIN32) && !defined(__CYGWIN__)

#define HAVE_DIRECT_H 1
#define HAVE_INTSAFE_H 1
#define HAVE_IO_H 1
#define HAVE_PROCESS_H 1
#define HAVE_SYS_UTIME_H 1
#define HAVE_WINDOWS_H 1
#define HAVE_WINIOCTL_H 1
#define HAVE__CrtSetReportMode 1
#define HAVE_CTIME_S 1
#define HAVE__FSEEKI64 1
#define HAVE__GET_TIMEZONE 1
#define HAVE_GMTIME_S 1
#define HAVE_LOCALTIME_S 1
#define HAVE__MKGMTIME 1
#define SIZEOF_WCHAR_T 2
#ifndef NTDDI_VERSION
#define NTDDI_VERSION 0x0A000000
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#ifndef WINVER
#define WINVER 0x0A00
#endif
#include <stdint.h>
#define gid_t short
#define id_t short
#define mode_t unsigned short
#define pid_t int
#define ssize_t int64_t
#define uid_t short

#else /* POSIX */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif
#define HAVE_DECL_SSIZE_MAX 1
#define HAVE_FCNTL 1
#define HAVE_GETEUID 1
#define HAVE_GMTIME_R 1
#define HAVE_LANGINFO_H 1
#define HAVE_LOCALTIME_R 1
#define HAVE_LSTAT 1
#define HAVE_MEMMOVE 1
#define HAVE_MEMSET 1
#define HAVE_MKSTEMP 1
#define HAVE_NL_LANGINFO 1
#define HAVE_STRINGS_H 1
#define HAVE_SYS_TIME_H 1
#define HAVE_TIMEGM 1
#define HAVE_UNISTD_H 1
#define HAVE_UTIME_H 1
#define HAVE_WMEMCMP 1
#define HAVE_WMEMCPY 1
#define HAVE_WMEMMOVE 1
#define SIZEOF_WCHAR_T 4

#endif

/*
 * A crafted archive can declare tables of any size (a 7z header may announce
 * hundreds of millions of files in a few bytes). No single allocation of the
 * library may exceed this, so such an archive fails with "out of memory"
 * instead of taking the PC's memory. Real archives stay far below: the
 * largest tables of a 7z with two million files, a RAR dictionary (64 MiB at
 * most in this reader) and a PPMd model all fit.
 */
#include <errno.h>
#include <stdlib.h>
#define FW_LA_MAX_ALLOC ((size_t)256 * 1024 * 1024)
static inline void *fw_la_malloc(size_t n)
{
	if (n > FW_LA_MAX_ALLOC) {
		errno = ENOMEM;
		return NULL;
	}
	return malloc(n);
}
static inline void *fw_la_calloc(size_t n, size_t s)
{
	if (s != 0 && n > FW_LA_MAX_ALLOC / s) {
		errno = ENOMEM;
		return NULL;
	}
	return calloc(n, s);
}
static inline void *fw_la_realloc(void *p, size_t n)
{
	if (n > FW_LA_MAX_ALLOC) {
		errno = ENOMEM;
		return NULL;
	}
	return realloc(p, n);
}
/* Object-like: one call in the library has a preprocessor line in its arguments. */
#define malloc fw_la_malloc
#define calloc fw_la_calloc
#define realloc fw_la_realloc

#endif /* FW_LIBARCHIVE_CONFIG_H */
