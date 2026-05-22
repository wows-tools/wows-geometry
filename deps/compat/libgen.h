/*
 * libgen.h shim for Windows (MSVC / UCRT).
 * On non-Windows platforms this forwards to the real system header via
 * #include_next so the compat directory does not shadow it.
 * On Windows, the bundled sources include libgen.h but do not call
 * basename/dirname, so an empty stub suffices.
 */
#ifndef COMPAT_LIBGEN_H
#define COMPAT_LIBGEN_H

#ifndef _WIN32
#include_next <libgen.h>
#else
#include <string.h>
#endif

#endif /* COMPAT_LIBGEN_H */
