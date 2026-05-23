/*
 * libgen.h shim for Windows (MSVC / UCRT / MinGW).
 * On non-Windows platforms this forwards to the real system header via
 * #include_next so the compat directory does not shadow it.
 * On Windows, dirname/basename are not available — provide inline versions.
 */
#ifndef COMPAT_LIBGEN_H
#define COMPAT_LIBGEN_H

#ifndef _WIN32
#include_next <libgen.h>
#else
#include <string.h>
#include <stdlib.h>

static inline char *dirname(char *path) {
    static char buf[4096];
    if (!path || !*path)
        return (char *)".";
    size_t len = strlen(path);
    /* strip trailing slashes */
    while (len > 1 && (path[len - 1] == '/' || path[len - 1] == '\\'))
        len--;
    /* find last separator */
    size_t i = len;
    while (i > 0 && path[i - 1] != '/' && path[i - 1] != '\\')
        i--;
    if (i == 0)
        return (char *)".";
    /* strip trailing slashes of the parent */
    while (i > 1 && (path[i - 1] == '/' || path[i - 1] == '\\'))
        i--;
    if (i >= sizeof(buf))
        i = sizeof(buf) - 1;
    memcpy(buf, path, i);
    buf[i] = '\0';
    return buf;
}

static inline char *basename(char *path) {
    if (!path || !*path)
        return (char *)".";
    size_t len = strlen(path);
    while (len > 1 && (path[len - 1] == '/' || path[len - 1] == '\\'))
        len--;
    size_t i = len;
    while (i > 0 && path[i - 1] != '/' && path[i - 1] != '\\')
        i--;
    return path + i;
}
#endif

#endif /* COMPAT_LIBGEN_H */
