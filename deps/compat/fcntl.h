/*
 * fcntl.h shim for Windows (MSVC / UCRT).
 * On non-Windows platforms this forwards to the real system header via
 * #include_next.
 * On Windows, MSVC has <fcntl.h> but uses _O_* names; this header provides
 * the standard O_* names and maps open() to _open().
 * Note: we cannot use #include_next on MSVC, so constants are defined directly.
 */
#ifndef COMPAT_FCNTL_H
#define COMPAT_FCNTL_H

#ifndef _WIN32
#include_next <fcntl.h>
#else

#include <io.h>       /* _open, _read, _write, _close, _O_* */
#include <sys/stat.h> /* _S_IREAD, _S_IWRITE */

/* Standard O_* flags mapped to MSVC _O_* values */
#ifndef O_RDONLY
#define O_RDONLY  _O_RDONLY
#endif
#ifndef O_WRONLY
#define O_WRONLY  _O_WRONLY
#endif
#ifndef O_RDWR
#define O_RDWR    _O_RDWR
#endif
#ifndef O_APPEND
#define O_APPEND  _O_APPEND
#endif
#ifndef O_CREAT
#define O_CREAT   _O_CREAT
#endif
#ifndef O_TRUNC
#define O_TRUNC   _O_TRUNC
#endif
#ifndef O_EXCL
#define O_EXCL    _O_EXCL
#endif
#ifndef O_BINARY
#define O_BINARY  _O_BINARY
#endif
#ifndef O_TEXT
#define O_TEXT    _O_TEXT
#endif

#define open  _open

#endif /* _WIN32 */
#endif /* COMPAT_FCNTL_H */
