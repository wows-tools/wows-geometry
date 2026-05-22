/*
 * unistd.h shim for Windows (MSVC / UCRT).
 * On non-Windows platforms this forwards to the real system header via
 * #include_next so the compat directory does not shadow it.
 * On Windows, maps the few POSIX identifiers used by the bundled sources
 * to their MSVC equivalents.
 */
#ifndef COMPAT_UNISTD_H
#define COMPAT_UNISTD_H

#ifndef _WIN32
#include_next <unistd.h>
#else

#include <io.h>
#include <process.h>
#include <direct.h>

#define close   _close
#define read    _read
#define write   _write
#define unlink  _unlink
#define getcwd  _getcwd
#define chdir   _chdir
#define getpid  _getpid

#ifndef _SSIZE_T_DEFINED
#define _SSIZE_T_DEFINED
#include <basetsd.h>
typedef SSIZE_T ssize_t;
#endif

#endif /* _WIN32 */
#endif /* COMPAT_UNISTD_H */
