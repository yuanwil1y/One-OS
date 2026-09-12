#pragma once

/*
 * Host-build stand-in for ESP-IDF's esp_timer.h.
 *
 * The adapter needs a monotonic millisecond clock. On the host the test owns that
 * clock, so a deadline can be crossed without sleeping: the test advances it and
 * the module under test sees time pass. Defining the clock here rather than
 * calling the real one is what makes every timeout path deterministic and keeps
 * the suite fast.
 */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Microseconds since an arbitrary epoch, monotonic. Implemented by the test. */
int64_t esp_timer_get_time(void);

#ifdef __cplusplus
}
#endif
