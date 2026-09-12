#pragma once
/* In-process socket loopback for the Windows host mirror.
 *
 * tools/local/run-host-tests.ps1 cannot compile the socket-based esphome_l2
 * tests because llvm-mingw has no <sys/socket.h> or <arpa/inet.h>. This shim
 * gives those tests a real run on Windows instead of "CI only": every socket
 * call the tests make is redirected, through the macro block at the bottom, to
 * an in-process byte pipe, so the client and the test's own server thread talk
 * to each other exactly as they would over loopback.
 *
 * It is a test harness, not a network stack: no DNS, no routing, IPv4 loopback
 * only, a bounded per-connection buffer, and no real timeouts (a blocked read
 * spins with a one-millisecond sleep, which is why the sleep is declared
 * below instead of including <windows.h>, whose <winsock.h> would collide with
 * every redirect).
 *
 * The POSIX names are defined here rather than taken from the platform,
 * because llvm-mingw has no <sys/socket.h> and its <winsock.h> is not what the
 * firmware code is written against.
 */
#include <stddef.h>
#include <stdint.h>
#include <sys/time.h>

#ifdef _WIN32
void __stdcall Sleep(unsigned long milliseconds);
#endif

typedef unsigned int socklen_t;
typedef unsigned int shim_socklen_t;

struct sockaddr {
    unsigned short sa_family;
    char sa_data[14];
};
struct sockaddr_in {
    unsigned short sin_family;
    unsigned short sin_port;
    unsigned int sin_addr;
    char pad[8];
};
struct in_addr {
    unsigned int s_addr;
};

typedef struct {
    uint64_t bits;
} fd_set;

int shim_socket(int domain, int type, int protocol);
int shim_bind(int fd, const struct sockaddr *addr, socklen_t len);
int shim_listen(int fd, int backlog);
int shim_accept(int fd, struct sockaddr *addr, socklen_t *len);
int shim_connect(int fd, const struct sockaddr *addr, socklen_t len);
int shim_setsockopt(int fd, int level, int option, const void *value, socklen_t len);
int shim_getsockopt(int fd, int level, int option, void *value, socklen_t *len);
int shim_getsockname(int fd, struct sockaddr *addr, socklen_t *len);
int shim_shutdown(int fd, int how);
int shim_sock_close(int fd);
long shim_recv(int fd, void *buf, size_t len, int flags);
long shim_send(int fd, const void *buf, size_t len, int flags);
int shim_select(int nfds, fd_set *rd, fd_set *wr, fd_set *ex, struct timeval *tv);
unsigned int shim_htonl(unsigned int x);
unsigned short shim_htons(unsigned short x);

#define AF_INET 2
#define AF_UNSPEC 0
#define SOCK_STREAM 1
#define SOL_SOCKET 1
#define SO_REUSEADDR 2
#define SO_RCVTIMEO 3
#define SO_SNDTIMEO 4
#define SO_ERROR 5
#define SHUT_RDWR 2
#define INADDR_LOOPBACK 0x7f000001u
#define INADDR_ANY 0u
#define htonl(x) shim_htonl(x)
#define htons(x) shim_htons(x)
#define ntohs(x) shim_htons(x)

#define FD_ZERO(set) ((set)->bits = 0u)
#define FD_SET(fd, set) ((set)->bits |= (1ull << (unsigned)(fd)))
#define FD_ISSET(fd, set) (((set)->bits >> (unsigned)(fd)) & 1ull)

/* Only the calls the firmware makes, and only the ones llvm-mingw does not
 * already declare with an incompatible signature. */
#define socket shim_socket
#define bind shim_bind
#define listen shim_listen
#define accept shim_accept
#define connect shim_connect
#define setsockopt shim_setsockopt
#define getsockopt shim_getsockopt
#define getsockname shim_getsockname
#define shutdown shim_shutdown
#define recv shim_recv
#define send shim_send
#define select shim_select
