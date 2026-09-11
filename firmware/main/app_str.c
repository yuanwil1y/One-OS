/*
 * Portable bounded string copy. See app_str.h for why this exists.
 *
 * Platform independent: compiled into the firmware and the host tests.
 */

#include "app_str.h"

size_t app_strlcpy(char *dst, const char *src, size_t dst_size)
{
    size_t src_len = 0u;
    size_t copy_len;

    if (dst == NULL || dst_size == 0u) {
        return 0u;
    }
    if (src == NULL) {
        dst[0] = '\0';
        return 0u;
    }

    while (src[src_len] != '\0') {
        ++src_len;
    }

    copy_len = src_len;
    if (copy_len > dst_size - 1u) {
        copy_len = dst_size - 1u;
    }
    for (size_t i = 0u; i < copy_len; ++i) {
        dst[i] = src[i];
    }
    dst[copy_len] = '\0';

    return src_len;
}
