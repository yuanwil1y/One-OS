#pragma once
/* <fcntl.h> exists on Windows but has no fcntl() and none of the open flags the
 * firmware uses to put a socket in non-blocking mode. The loopback shim is
 * always "blocking", so the call is a no-op that reports success. */
#include_next <fcntl.h>

int shim_fcntl(int fd, int cmd, ...);

#define F_GETFL 3
#define F_SETFL 4
#define O_NONBLOCK 04000

#define fcntl shim_fcntl
