#pragma once
/* The tests include <arpa/inet.h> on every platform. On Windows there is no
 * such header, so the mirror redirects it to the loopback shim's socket
 * interface; the real header is a superset of what the tests use. */
#include <sys/socket.h>
