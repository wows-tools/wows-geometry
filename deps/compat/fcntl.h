/*
 * fcntl.h shim for Windows (MSVC / UCRT / MinGW).
 * On non-Windows platforms this forwards to the real system header via
 * #include_next.
 * On Windows with MSVC/UCRT, <fcntl.h> uses _O_* names; this header provides
 * the standard O_* names and maps open() to _open().
 * On MinGW, we use #include_next so the real system fcntl.h (which defines
 * both _O_* and O_*) is included directly, avoiding circular include issues.
 */
#ifndef COMPAT_FCNTL_H
#define COMPAT_FCNTL_H

#if !defined(_WIN32) || defined(__MINGW32__) || defined(__MINGW64__)
#include_next <fcntl.h>
#else

#include <io.h>       /* _open, _close */
#include <sys/stat.h> /* _S_IREAD, _S_IWRITE */

/* UCRT exposes _O_* in <fcntl.h>; our shim replaces that header, so define them
 * here (values match MSVC/UCRT fcntl.h). */
#ifndef _O_RDONLY
#define _O_RDONLY 0x0000
#endif
#ifndef _O_WRONLY
#define _O_WRONLY 0x0001
#endif
#ifndef _O_RDWR
#define _O_RDWR 0x0002
#endif
#ifndef _O_APPEND
#define _O_APPEND 0x0008
#endif
#ifndef _O_CREAT
#define _O_CREAT 0x0100
#endif
#ifndef _O_TRUNC
#define _O_TRUNC 0x0200
#endif
#ifndef _O_EXCL
#define _O_EXCL 0x0400
#endif
#ifndef _O_BINARY
#define _O_BINARY 0x8000
#endif
#ifndef _O_TEXT
#define _O_TEXT 0x4000
#endif

#ifndef O_RDONLY
#define O_RDONLY _O_RDONLY
#endif
#ifndef O_WRONLY
#define O_WRONLY _O_WRONLY
#endif
#ifndef O_RDWR
#define O_RDWR _O_RDWR
#endif
#ifndef O_APPEND
#define O_APPEND _O_APPEND
#endif
#ifndef O_CREAT
#define O_CREAT _O_CREAT
#endif
#ifndef O_TRUNC
#define O_TRUNC _O_TRUNC
#endif
#ifndef O_EXCL
#define O_EXCL _O_EXCL
#endif
#ifndef O_BINARY
#define O_BINARY _O_BINARY
#endif
#ifndef O_TEXT
#define O_TEXT _O_TEXT
#endif

#define open  _open

#endif /* _WIN32 and not MinGW */
#endif /* COMPAT_FCNTL_H */
