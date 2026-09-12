#pragma once
/* <netdb.h> does not exist for the Windows host mirror. The tests only need
 * getaddrinfo/freeaddrinfo to resolve the loopback host and port they bound. */
#include <sys/socket.h>

struct addrinfo {
    int ai_family;
    int ai_socktype;
    int ai_protocol;
    socklen_t ai_addrlen;
    struct sockaddr *ai_addr;
    struct addrinfo *ai_next;
};

int shim_getaddrinfo(const char *node, const char *service, const struct addrinfo *hints,
                     struct addrinfo **res);
void shim_freeaddrinfo(struct addrinfo *res);

#define getaddrinfo shim_getaddrinfo
#define freeaddrinfo shim_freeaddrinfo
