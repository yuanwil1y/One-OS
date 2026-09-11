#pragma once

/*
 * Portable bounded string copy for One-OS application code.
 *
 * Why this exists:
 *   ESP-IDF and BSD provide strlcpy() in <string.h>, but glibc only gained it in
 *   2.38 and a strict -std=c11 host build does not declare it. The host tests
 *   compile the same application sources as the firmware, so a direct strlcpy()
 *   call would either fail to compile on the host or silently differ from the
 *   firmware build.
 *
 * app_strlcpy() is therefore defined once here and used by application code.
 * It is not a wrapper that renames an existing API for style: it exists only to
 * make the two builds agree, and it matches strlcpy() semantics exactly.
 *
 * This header is platform independent (no ESP-IDF/FreeRTOS headers).
 */

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Copy at most `dst_size - 1` bytes of `src` into `dst` and always terminate.
 * Returns strlen(src), so a return value >= dst_size means truncation.
 * `dst_size` must be greater than zero.
 */
size_t app_strlcpy(char *dst, const char *src, size_t dst_size);

#ifdef __cplusplus
}
#endif
