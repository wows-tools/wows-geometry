/*
 * dirent.h shim for Windows (MSVC / UCRT).
 * On non-Windows platforms this forwards to the real system header via
 * #include_next so the compat directory does not shadow it.
 * On Windows, implements opendir/readdir/closedir via Win32 FindFirstFile.
 */
#ifndef COMPAT_DIRENT_H
#define COMPAT_DIRENT_H

#ifndef _WIN32
/* Let the real system dirent.h handle everything on non-Windows. */
#include_next <dirent.h>
#else

#include <windows.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

struct dirent {
    char d_name[MAX_PATH];
};

typedef struct {
    HANDLE          handle;
    WIN32_FIND_DATAA find_data;
    int             first;
    struct dirent   entry;
} DIR;

static inline DIR *opendir(const char *path) {
    char pattern[MAX_PATH];
    DIR *dir;

    if (!path || strlen(path) + 3 > MAX_PATH) {
        errno = ENAMETOOLONG;
        return NULL;
    }
    dir = (DIR *)malloc(sizeof(DIR));
    if (!dir) return NULL;

    snprintf(pattern, sizeof(pattern), "%s\\*", path);
    dir->handle = FindFirstFileA(pattern, &dir->find_data);
    if (dir->handle == INVALID_HANDLE_VALUE) {
        free(dir);
        errno = ENOENT;
        return NULL;
    }
    dir->first = 1;
    return dir;
}

static inline struct dirent *readdir(DIR *dir) {
    if (!dir) return NULL;
    if (dir->first) {
        dir->first = 0;
    } else {
        if (!FindNextFileA(dir->handle, &dir->find_data))
            return NULL;
    }
    strncpy(dir->entry.d_name, dir->find_data.cFileName, MAX_PATH - 1);
    dir->entry.d_name[MAX_PATH - 1] = '\0';
    return &dir->entry;
}

static inline int closedir(DIR *dir) {
    if (!dir) return -1;
    FindClose(dir->handle);
    free(dir);
    return 0;
}

#endif /* _WIN32 */
#endif /* COMPAT_DIRENT_H */
