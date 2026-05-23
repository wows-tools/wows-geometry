#ifdef _WIN32

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include "posix_compat.h"

/*
 * Bundled wows-depack sources (e.g. utils.c) set _POSIX_C_SOURCE and include
 * <sys/stat.h> without posix_compat.h.  MSVC then treats S_ISDIR/S_ISREG as
 * CRT functions — provide them here (main-project portability layer).
 */
int S_ISDIR(int mode) { return (((mode) & _S_IFMT) == _S_IFDIR); }

int S_ISREG(int mode) { return (((mode) & _S_IFMT) == _S_IFREG); }

char *realpath(const char *path, char *resolved) {
    return _fullpath(resolved, path, _MAX_PATH);
}

/* fmemopen — POSIX function missing from the UCRT runtime used by this CI.
 *
 * Read mode:  write buf into a tmpfile, rewind, return the FILE*.
 *
 * Write mode: zero buf so every byte past the last write is '\0' (matching
 *             the POSIX requirement that the buffer is null-terminated after
 *             the last write), then install buf as the FILE's I/O buffer via
 *             setvbuf(_IOFBF).  Writes go directly into buf; after fclose the
 *             buffer still contains the data because the C runtime does not
 *             zero a caller-supplied setvbuf buffer on close. */
FILE *fmemopen(void *buf, size_t size, const char *mode) {
    FILE *f = tmpfile();
    if (!f)
        return NULL;

    if (mode[0] == 'r') {
        if (fwrite(buf, 1, size, f) != size) {
            fclose(f);
            return NULL;
        }
        rewind(f);
    } else {
        memset(buf, 0, size);
        if (setvbuf(f, (char *)buf, _IOFBF, size) != 0) {
            fclose(f);
            return NULL;
        }
    }
    return f;
}

#endif /* _WIN32 */
