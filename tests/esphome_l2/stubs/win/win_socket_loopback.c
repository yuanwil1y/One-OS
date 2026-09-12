/* In-process socket loopback for the Windows host mirror. See the header for
 * what this is and why it exists. Test-support code only. */
#include <sys/socket.h>
/* The Win32 sleep, declared here rather than by including <windows.h>, which
 * would pull in <winsock.h> and collide with the redirects above. */
void __stdcall Sleep(unsigned long milliseconds);
unsigned long __stdcall GetCurrentThreadId(void);

/* fcntl.h is redirected here; the shim never blocks differently. */
int shim_fcntl(int fd, int cmd, ...)
{
    (void)fd;
    (void)cmd;
    return 0;
}

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SHIM_MAX_FD 32
#define SHIM_BUF_BYTES 65536u

typedef struct {
    int kind; /* 0 unused, 1 listener, 2 connection */
    int peer; /* connection paired with this one, -1 for a listener */
    unsigned short port;
    unsigned char buf[SHIM_BUF_BYTES];
    size_t head, tail;
    int closed_by_peer;
    int accepted;
} shim_sock_t;

/* Set WIN_SHIM_TRACE=1 to follow every call. The socket tests otherwise hang
 * with no output when the harness is wrong, which is exactly what this
 * environment cannot afford. */
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
        fprintf(stderr, "shim %-12s fd=%d n=%ld\n", what, fd, n);
        fflush(stderr);
    }
}
static shim_sock_t g_socks[SHIM_MAX_FD];
static unsigned short g_next_port = 40000u;

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

int shim_socket(int domain, int type, int protocol)
{
    (void)domain;
    (void)type;
    (void)protocol;
    for (int fd = 0; fd < SHIM_MAX_FD; fd++) {
        if (g_socks[fd].kind == 0) {
            memset(&g_socks[fd], 0, sizeof(g_socks[fd]));
            g_socks[fd].kind = 2; /* a connection until it listens */
            g_socks[fd].peer = -1;
            trace("socket", fd, 0);
            return fd;
        }
    }
    errno = EMFILE;
    return -1;
}

int shim_bind(int fd, const struct sockaddr *addr, socklen_t len)
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

int shim_listen(int fd, int backlog)
{
    shim_sock_t *s = at(fd);
    (void)backlog;
    if (s == NULL) {
        errno = EBADF;
        return -1;
    }
    s->kind = 1;
    trace("listen", fd, 0);
    return 0;
}

int shim_accept(int fd, struct sockaddr *addr, socklen_t *len)
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
                /* The accepted end needs its OWN descriptor. Returning the
                 * connecting socket's number would make both sides share one
                 * buffer and steal each other's bytes. */
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
                    return mine;
                }
                errno = EMFILE;
                return -1;
            }
        }
        Sleep(1);
    }
}

int shim_connect(int fd, const struct sockaddr *addr, socklen_t len)
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

int shim_setsockopt(int fd, int level, int option, const void *value, socklen_t len)
{
    (void)level;
    (void)option;
    (void)value;
    (void)len;
    return at(fd) == NULL ? -1 : 0;
}

int shim_getsockopt(int fd, int level, int option, void *value, socklen_t *len)
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

int shim_getsockname(int fd, struct sockaddr *addr, socklen_t *len)
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

int shim_shutdown(int fd, int how)
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

int shim_sock_close(int fd)
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

long shim_recv(int fd, void *buf, size_t len, int flags)
{
    shim_sock_t *s = at(fd);
    (void)flags;
    if (s == NULL || buf == NULL) {
        errno = EBADF;
        return -1;
    }
    {   /* A blocking read in the loopback harness can only mean the two sides
         * disagree about the protocol. Hanging forever hides that, so it becomes
         * a loud abort with the descriptor and the byte count. */
        for (;;) {
        if (s->head != s->tail) {
            size_t have = s->tail - s->head;
            size_t take = len < have ? len : have;
            memcpy(buf, s->buf + s->head, take);
            s->head += take;
            if (s->head == s->tail) {
                s->head = s->tail = 0u;
            }
            trace("recv", fd, (long)take);
            return (long)take;
        }
            if (s->closed_by_peer) {
                return 0; /* the peer closed */
            }
            Sleep(1);
        }
    }
}

long shim_send(int fd, const void *buf, size_t len, int flags)
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

int shim_select(int nfds, fd_set *rd, fd_set *wr, fd_set *ex, struct timeval *tv)
{
    (void)tv;
    (void)ex;
    for (;;) {
        int ready = 0;
        for (int fd = 0; fd < nfds && fd < SHIM_MAX_FD; fd++) {
            if (rd != NULL && FD_ISSET(fd, rd)) {
                shim_sock_t *s = &g_socks[fd];
                if (s->head != s->tail || s->closed_by_peer) {
                    fd_set only;
                    FD_ZERO(&only);
                    FD_SET(fd, &only);
                    *rd = only;
                    ready = 1;
                    break;
                }
            }
        }
        if (ready) {
            if (wr != NULL) {
                FD_ZERO(wr);
            }
            return 1;
        }
        for (int fd = 0; fd < nfds && fd < SHIM_MAX_FD; fd++) {
            if (wr != NULL && FD_ISSET(fd, wr)) {
                /* A connected socket is always writable in this shim. */
                fd_set only;
                FD_ZERO(&only);
                FD_SET(fd, &only);
                *wr = only;
                if (rd != NULL) {
                    FD_ZERO(rd);
                }
                return 1;
            }
        }
        Sleep(1);
    }
}

/* getaddrinfo for the numeric loopback host and port the tests use. */
static struct addrinfo g_ai;
static struct sockaddr_in g_ai_addr;

int shim_getaddrinfo(const char *node, const char *service, const struct addrinfo *hints,
                     struct addrinfo **res)
{
    (void)hints;
    if (node == NULL || service == NULL || res == NULL) {
        return -1;
    }
    memset(&g_ai_addr, 0, sizeof(g_ai_addr));
    g_ai_addr.sin_family = AF_INET;
    g_ai_addr.sin_port = shim_htons((unsigned short)atoi(service));
    g_ai_addr.sin_addr = shim_htonl(INADDR_LOOPBACK);
    memset(&g_ai, 0, sizeof(g_ai));
    g_ai.ai_family = AF_INET;
    g_ai.ai_socktype = SOCK_STREAM;
    g_ai.ai_addrlen = (socklen_t)sizeof(g_ai_addr);
    g_ai.ai_addr = (struct sockaddr *)&g_ai_addr;
    *res = &g_ai;
    return 0;
}

void shim_freeaddrinfo(struct addrinfo *res)
{
    (void)res;
}