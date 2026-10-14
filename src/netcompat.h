/*
 * Part of Astonia Server 3.5 (c) Daniel Brockhaus. Please read license.txt.
 *
 * Small socket compatibility layer so the same code builds on Linux and
 * on Windows (Winsock2).
 */

#ifndef NETCOMPAT_H
#define NETCOMPAT_H

#ifdef _WIN32

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#define sock_close(s)    closesocket(s)
#define sock_ioctl(s, c, a) ioctlsocket((s), (c), (a))
#define sock_errno       WSAGetLastError()
#define SOCK_EWOULDBLOCK WSAEWOULDBLOCK

// Winsock must be started before any socket call.
int net_startup(void);

#else

#include <unistd.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>

#define sock_close(s)    close(s)
#define sock_ioctl(s, c, a) ioctl((s), (c), (a))
#define sock_errno       errno
#define SOCK_EWOULDBLOCK EWOULDBLOCK

static inline int net_startup(void) {
    return 1;
}

#endif

#endif
