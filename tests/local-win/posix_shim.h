/* Socket declarations for the Windows host mirror.
 *
 * The shipping esphome_l2 tests are written against POSIX sockets and CI
 * compiles them against the real headers. This machine has no <sys/socket.h>,
 * so this header declares the subset those tests and
 * firmware/components/esphome_l2 use; win_socket_loopback.c implements it over
 * an in-process byte pipe.
 *
 * It is deliberately self-contained: it must not pull in <winsock.h>, whose
 * declarations of socket(), bind(), connect() and select() conflict with these
 * signatures.
 *
 * Test-support code only. Nothing here is compiled into firmware. */
#pragma once
#include <stddef.h>
#include <stdint.h>

typedef unsigned int socklen_t;

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

/* The platform already defines struct timeval in <_timeval.h>; only the type
 * the firmware names is added here. */
#ifndef _TIMEVAL_DEFINED
#define _TIMEVAL_DEFINED
struct timeval {
    long tv_sec;
    long tv_usec;
};
#endif
typedef long suseconds_t;

typedef struct {
    unsigned long fds_bits[4];
} fd_set;

int socket(int, int, int);
int bind(int, const struct sockaddr *, socklen_t);
int listen(int, int);
int accept(int, struct sockaddr *, socklen_t *);
int connect(int, const struct sockaddr *, socklen_t);
int shutdown(int, int);
int close(int);
int setsockopt(int, int, int, const void *, socklen_t);
int getsockopt(int, int, int, void *, socklen_t *);
int getsockname(int, struct sockaddr *, socklen_t *);
long recv(int, void *, size_t, int);
long send(int, const void *, size_t, int);
int select(int, fd_set *, fd_set *, fd_set *, struct timeval *);
int fcntl(int, int, ...);

struct addrinfo {
    int ai_family;
    int ai_socktype;
    int ai_protocol;
    socklen_t ai_addrlen;
    struct sockaddr *ai_addr;
    struct addrinfo *ai_next;
};
int getaddrinfo(const char *, const char *, const struct addrinfo *, struct addrinfo **);
void freeaddrinfo(struct addrinfo *);

unsigned int shim_htonl(unsigned int);
unsigned short shim_htons(unsigned short);

#define FD_ZERO(set) \
    ((set)->fds_bits[0] = 0u, (set)->fds_bits[1] = 0u, (set)->fds_bits[2] = 0u, (set)->fds_bits[3] = 0u)
#define FD_SET(fd, set) ((set)->fds_bits[(unsigned)(fd) / 64u] |= (1ul << ((unsigned)(fd) % 64u)))
#define FD_CLR(fd, set) ((set)->fds_bits[(unsigned)(fd) / 64u] &= ~(1ul << ((unsigned)(fd) % 64u)))
#define FD_ISSET(fd, set) (((set)->fds_bits[(unsigned)(fd) / 64u] >> ((unsigned)(fd) % 64u)) & 1ul)

#define htonl(x) shim_htonl(x)
#define htons(x) shim_htons(x)
#define ntohs(x) shim_htons(x)

#define SOL_SOCKET 1
#define SO_REUSEADDR 2
#define SO_RCVTIMEO 3
#define SO_SNDTIMEO 4
#define SO_ERROR 5
#define AF_INET 2
#define AF_UNSPEC 0
#define SOCK_STREAM 1
#define SHUT_RDWR 2
#define INADDR_LOOPBACK 0x7f000001u
#define INADDR_ANY 0u

#define F_GETFL 3
#define F_SETFL 4
#define O_NONBLOCK 04000
