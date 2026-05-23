/*
 * endian.h shim for Windows (MSVC / UCRT).
 * On non-Windows platforms this forwards to the real system header via
 * #include_next so the compat directory does not shadow it.
 * On Windows, the le*toh / be*toh / htole* / htobe* macros are provided
 * directly (Windows is always little-endian; big-endian uses _byteswap_*).
 */
#ifndef COMPAT_ENDIAN_H
#define COMPAT_ENDIAN_H

#ifndef _WIN32
#include_next <endian.h>
#else

#include <stdlib.h> /* _byteswap_ushort / _byteswap_ulong / _byteswap_uint64 */

#define htole16(x)  ((uint16_t)(x))
#define le16toh(x)  ((uint16_t)(x))
#define htole32(x)  ((uint32_t)(x))
#define le32toh(x)  ((uint32_t)(x))
#define htole64(x)  ((uint64_t)(x))
#define le64toh(x)  ((uint64_t)(x))

#define be16toh(x)  _byteswap_ushort((uint16_t)(x))
#define htobe16(x)  _byteswap_ushort((uint16_t)(x))
#define be32toh(x)  _byteswap_ulong((uint32_t)(x))
#define htobe32(x)  _byteswap_ulong((uint32_t)(x))
#define be64toh(x)  _byteswap_uint64((uint64_t)(x))
#define htobe64(x)  _byteswap_uint64((uint64_t)(x))

#endif /* _WIN32 */
#endif /* COMPAT_ENDIAN_H */
