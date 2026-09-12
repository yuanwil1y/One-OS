/* In-process socket loopback for the Windows host mirror.
 *
 * See posix_shim.h for why this exists. It implements the calls the esphome_l2
 * tests make over a bounded in-process pipe, so those tests can be run and
 * debugged on this machine instead of only in CI.
 *
 * It is a debugging aid, not a TCP stack: IPv4 loopback only, no DNS beyond the
 * numeric host, no half-close, and no flow control beyond sleeping when a
 * buffer is full. A test that passes here still has to pass in CI.
 *
 * Test-support code only. Nothing here is compiled into firmware. */

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "posix_shim.h"

#ifdef _WIN32
void __stdcall Sleep(unsigned long milliseconds);
unsigned long __stdcall GetCurrentThreadId(void);
#endif

#define SHIM_MAX_FD 32
#define SHIM_BUF_BYTES 65536u

typedef struct {
    int kind; /* 0 unused, 1 listener, 2 connection */
    int peer;
    int accepted;
    int closed_by_peer;
    unsigned short port;
    unsigned char buf[SHIM_BUF_BYTES];
    size_t head, tail;
} shim_sock_t;

static shim_sock_t g_socks[SHIM_MAX_FD];
static unsigned short g_next_port = 40000u;

static int trace_on(void)
{
    static int cached = -1;
    if (cached < 0) {
        const char *v = getenv("WIN_SHIM_TRACE");
        cached = (v != NULL && v[0] == '1') ? 1 : 0;
    }
    return cached;
}

static void trace(const char *what, int fd, long n)
{
    if (trace_on()) {
        fprintf(stderr, "shim %-10s fd=%d n=%ld thr=%lu\n", what, fd, n, GetCurrentThreadId());
        fflush(stderr);
    }
}

static shim_sock_t *at(int fd)
{
    if (fd < 0 || fd >= SHIM_MAX_FD) {
        return NULL;
    }
    return &g_socks[fd];
}

unsigned int shim_htonl(unsigned int x)
{
    return ((x & 0x000000ffu) << 24) | ((x & 0x0000ff00u) << 8) | ((x & 0x00ff0000u) >> 8) |
           ((x & 0xff000000u) >> 24);
}

unsigned short shim_htons(unsigned short x)
{
    return (unsigned short)(((x & 0x00ffu) << 8) | ((x & 0xff00u) >> 8));
}

int fcntl(int fd, int cmd, ...)
{
    (void)fd;
    (void)cmd;
    return 0; /* the loopback never blocks differently */
}

int socket(int domain, int type, int protocol)
{
    (void)domain;
    (void)type;
    (void)protocol;
    for (int fd = 0; fd < SHIM_MAX_FD; fd++) {
        if (g_socks[fd].kind == 0) {
            memset(&g_socks[fd], 0, sizeof(g_socks[fd]));
            g_socks[fd].kind = 2;
            g_socks[fd].peer = -1;
            trace("socket", fd, 0);
            return fd;
        }
    }
    errno = EMFILE;
    return -1;
}

int bind(int fd, const struct sockaddr *addr, socklen_t len)
{
    shim_sock_t *s = at(fd);
    (void)len;
    if (s == NULL || addr == NULL) {
        errno = EBADF;
        return -1;
    }
    s->port = ((const struct sockaddr_in *)addr)->sin_port;
    if (s->port == 0u) {
        s->port = shim_htons(g_next_port++);
    }
    trace("bind", fd, (long)s->port);
    return 0;
}

int listen(int fd, int backlog)
{
    shim_sock_t *s = at(fd);
    (void)backlog;
    if (s == NULL) {
        errno = EBADF;
        return -1;
    }
    s->kind = 1;
    return 0;
}

int accept(int fd, struct sockaddr *addr, socklen_t *len)
{
    shim_sock_t *l = at(fd);
    (void)addr;
    (void)len;
    if (l == NULL || l->kind != 1) {
        errno = EBADF;
        return -1;
    }
    for (;;) {
        for (int other = 0; other < SHIM_MAX_FD; other++) {
            shim_sock_t *c = &g_socks[other];
            if (c->kind == 2 && c->peer == fd && c->accepted == 0 && c->closed_by_peer == 0) {
                /* The accepted end needs its own descriptor: sharing the
                 * connecting socket's number would make both sides read the
                 * same buffer and steal each other's bytes. */
                for (int mine = 0; mine < SHIM_MAX_FD; mine++) {
                    if (mine == other || g_socks[mine].kind != 0) {
                        continue;
                    }
                    c->accepted = 1;
                    memset(&g_socks[mine], 0, sizeof(g_socks[mine]));
                    g_socks[mine].kind = 2;
                    g_socks[mine].peer = other;
                    g_socks[mine].accepted = 1;
                    c->peer = mine;
                    trace("accept", mine, (long)fd);
                    return mine;
                }
                errno = EMFILE;
                return -1;
            }
        }
        Sleep(1);
    }
}

int connect(int fd, const struct sockaddr *addr, socklen_t len)
{
    shim_sock_t *c = at(fd);
    (void)len;
    if (c == NULL || addr == NULL) {
        errno = EBADF;
        return -1;
    }
    {
        unsigned short port = ((const struct sockaddr_in *)addr)->sin_port;
        for (int l = 0; l < SHIM_MAX_FD; l++) {
            if (g_socks[l].kind == 1 && g_socks[l].port == port) {
                c->peer = l;
                trace("connect", fd, (long)l);
                return 0;
            }
        }
    }
    errno = ECONNREFUSED;
    return -1;
}

int setsockopt(int fd, int level, int option, const void *value, socklen_t len)
{
    (void)level;
    (void)option;
    (void)value;
    (void)len;
    return at(fd) == NULL ? -1 : 0;
}

int getsockopt(int fd, int level, int option, void *value, socklen_t *len)
{
    (void)level;
    (void)option;
    if (at(fd) == NULL) {
        return -1;
    }
    if (value != NULL && len != NULL && *len >= sizeof(int)) {
        *(int *)value = 0;
        *len = sizeof(int);
    }
    return 0;
}

int getsockname(int fd, struct sockaddr *addr, socklen_t *len)
{
    shim_sock_t *s = at(fd);
    if (s == NULL || addr == NULL || len == NULL || *len < sizeof(struct sockaddr_in)) {
        errno = EBADF;
        return -1;
    }
    {
        struct sockaddr_in *in = (struct sockaddr_in *)addr;
        memset(in, 0, sizeof(*in));
        in->sin_family = AF_INET;
        in->sin_port = s->port;
        in->sin_addr = shim_htonl(INADDR_LOOPBACK);
    }
    *len = sizeof(struct sockaddr_in);
    return 0;
}

int shutdown(int fd, int how)
{
    shim_sock_t *s = at(fd);
    (void)how;
    if (s == NULL) {
        errno = EBADF;
        return -1;
    }
    if (s->peer >= 0 && s->peer < SHIM_MAX_FD) {
        g_socks[s->peer].closed_by_peer = 1;
    }
    return 0;
}

int close(int fd)
{
    shim_sock_t *s = at(fd);
    if (s == NULL) {
        errno = EBADF;
        return -1;
    }
    if (s->peer >= 0 && s->peer < SHIM_MAX_FD) {
        g_socks[s->peer].closed_by_peer = 1;
        g_socks[s->peer].peer = -1;
    }
    memset(s, 0, sizeof(*s));
    return 0;
}

long recv(int fd, void *buf, size_t len, int flags)
{
    /* POSIX recv: return up to len bytes, block until at least one is
     * available, return 0 once the peer has closed and the buffer is drained.
     * Returning 0 while bytes are still pending would make a caller treat a
     * full buffer as a closed connection. */
    shim_sock_t *s = at(fd);
    (void)flags;
    if (s == NULL || buf == NULL) {
        errno = EBADF;
        return -1;
    }
    for (;;) {
        if (s->head != s->tail) {
            size_t have = s->tail - s->head;
            size_t take = len < have ? len : have;
            memcpy(buf, s->buf + s->head, take);
            s->head += take;
            if (s->head == s->tail) {
                /* Empty: both back to zero, the only state in which send refills
                 * the buffer. */
                s->head = s->tail = 0u;
            }
            trace("recv", fd, (long)take);
            return (long)take;
        }
        if (s->closed_by_peer) {
            trace("recv-eof", fd, 0);
            return 0;
        }
        Sleep(1);
    }
}

long send(int fd, const void *buf, size_t len, int flags)
{
    shim_sock_t *s = at(fd);
    (void)flags;
    if (s == NULL || buf == NULL) {
        errno = EBADF;
        return -1;
    }
    for (;;) {
        shim_sock_t *p = (s->peer >= 0 && s->peer < SHIM_MAX_FD) ? &g_socks[s->peer] : NULL;
        if (p == NULL || p->closed_by_peer) {
            errno = EPIPE;
            return -1;
        }
        {
            size_t free_bytes = SHIM_BUF_BYTES - p->tail;
            if (free_bytes != 0u) {
                size_t take = len < free_bytes ? len : free_bytes;
                memcpy(p->buf + p->tail, buf, take);
                p->tail += take;
                trace("send", fd, (long)take);
                return (long)take;
            }
        }
        Sleep(1);
    }
}

int select(int nfds, fd_set *rd, fd_set *wr, fd_set *ex, struct timeval *tv)
{
    /* A connection is established the moment connect() pairs the sockets, so
     * readiness is immediate. Only the descriptors the caller asked about are
     * examined, so an uninitialised descriptor number cannot make this spin. */
    (void)tv;
    (void)ex;
    int ready = 0;
    for (int fd = 0; fd < nfds && fd < SHIM_MAX_FD; fd++) {
        bool want_read = (rd != NULL) && FD_ISSET(fd, rd);
        bool want_write = (wr != NULL) && FD_ISSET(fd, wr);
        if (!want_read && !want_write) {
            continue;
        }
        shim_sock_t *s = &g_socks[fd];
        bool readable = (s->kind != 0) && (s->head != s->tail || s->closed_by_peer);
        bool writable = (s->kind != 0) && (s->peer >= 0);
        if ((want_read && readable) || (want_write && writable)) {
            if (rd != NULL) {
                FD_ZERO(rd);
                if (want_read && readable) {
                    FD_SET(fd, rd);
                }
            }
            if (wr != NULL) {
                FD_ZERO(wr);
                if (want_write && writable) {
                    FD_SET(fd, wr);
                }
            }
            ready = 1;
            break;
        }
        if (want_read && rd != NULL) {
            FD_CLR(fd, rd);
        }
        if (want_write && wr != NULL) {
            FD_CLR(fd, wr);
        }
    }
    return ready;
}

int getaddrinfo(const char *node, const char *service, const struct addrinfo *hints,
                struct addrinfo **res)
{
    static struct addrinfo ai;
    static struct sockaddr_in sa;
    (void)node;
    (void)hints;
    if (service == NULL || res == NULL) {
        return -1;
    }
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = shim_htons((unsigned short)atoi(service));
    sa.sin_addr = shim_htonl(INADDR_LOOPBACK);
    memset(&ai, 0, sizeof(ai));
    ai.ai_family = AF_INET;
    ai.ai_socktype = SOCK_STREAM;
    ai.ai_addrlen = (socklen_t)sizeof(sa);
    ai.ai_addr = (struct sockaddr *)&sa;
    *res = &ai;
    return 0;
}

void freeaddrinfo(struct addrinfo *res)
{
    (void)res;
}
