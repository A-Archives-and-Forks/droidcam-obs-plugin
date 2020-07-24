/*
Copyright (C) 2020 github.com/aramg

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <errno.h>
#include <string.h>
#include "plugin.h"
#include "plugin_properties.h"
#include "net.h"

#ifdef _WIN32
  #pragma comment(lib,"ws2_32.lib")
  typedef int socklen_t;
#else
# include <arpa/inet.h>
# include <fcntl.h>
# include <unistd.h>
  typedef struct sockaddr_in SOCKADDR_IN;
  typedef struct sockaddr SOCKADDR;
  typedef struct in_addr IN_ADDR;
#endif

static bool set_nonblock(socket_t sock, int nonblock) {
#ifdef _WIN32
    u_long nb = nonblock;
    return (NO_ERROR == ioctlsocket(sock, FIONBIO, &nb));
#else
    int flags = fcntl(sock, F_GETFL, NULL);
    if (flags < 0) {
        elog("fcntl(): %s", strerror(errno));
        return false;
    }

    if (nonblock)
        flags |= O_NONBLOCK;
    else
        flags &= ~O_NONBLOCK;

    if (fcntl(sock, F_SETFL, flags) < 0) {
        elog("fcntl(): %s", strerror(errno));
        return false;
    }

    return true;
#endif
}

socket_t
net_connect_and_ping(const char* ip, uint16_t port) {
    int len;
    char buf[8];
    const char *ping_req = PING_REQ;

    socket_t sock = net_connect(ip, port);
    if (sock == INVALID_SOCKET) {
        return INVALID_SOCKET;
    }
    if ((len = net_send_all(sock, ping_req, sizeof(PING_REQ)-1)) <= 0) {
        elog("send(ping) failed");
        net_close(sock);
        return INVALID_SOCKET;
    }

    if ((len = net_recv(sock, buf, sizeof(buf))) <= 0) {
        elog("recv(ping) failed");
        net_close(sock);
        return INVALID_SOCKET;
    }

    if (len != 4 || memcmp(buf, "pong", 4) != 0) {
        elog("recv invalid data: %.*s", len, buf);
        net_close(sock);
        return INVALID_SOCKET;
    }
    return sock;
}

socket_t
net_connect(const char* ip, uint16_t port) {
    dlog("connect %s:%d", ip, port);

    int len = 65536;
    socket_t sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock == INVALID_SOCKET) {
        elog("socket(): %s", strerror(errno));
        return INVALID_SOCKET;
    }

    SOCKADDR_IN sin;
    sin.sin_family = AF_INET;
    sin.sin_addr.s_addr = inet_addr(ip);
    sin.sin_port = htons(port);

    struct timeval timeout;
    timeout.tv_sec = 2;
    timeout.tv_usec = 0;

    fd_set set;
    FD_ZERO(&set);
    FD_SET(sock, &set);

    if (!set_nonblock(sock, 1)) {
ERROR_OUT:
        net_close(sock);
        return INVALID_SOCKET;
    }

    connect(sock, (SOCKADDR *) &sin, sizeof(sin));
#if _WIN32
    if (WSAGetLastError() != WSAEWOULDBLOCK)
        goto ERROR_OUT;
#else
    if (!(errno == EAGAIN || errno == EWOULDBLOCK || errno == EINPROGRESS)) {
        elog("connect(): %s", strerror(errno));
        goto ERROR_OUT;
    }
#endif

    if (select(sock+1, NULL, &set, NULL, &timeout) <= 0) {
        elog("connect timeout/failed: %s", strerror(errno));
        goto ERROR_OUT;
    }

    if (!set_nonblock(sock, 0))
        goto ERROR_OUT;

    setsockopt(sock, SOL_SOCKET, SO_RCVBUF, (char *) &len, sizeof(int));
    return sock;
}

ssize_t
net_recv(socket_t sock, void *buf, size_t len) {
#if _WIN32
    return recv(sock, (char*)buf, len, 0);
#else
    return recv(sock, buf, len, 0);
#endif
}

ssize_t
net_recv_peek(socket_t sock) {
    char buf[4];
    return recv(sock, buf, 1, MSG_PEEK);
}

ssize_t
net_recv_all(socket_t sock, void *buf, size_t len) {
#if _WIN32
    return recv(sock, (char*)buf, len, MSG_WAITALL);
#else
    return recv(sock, buf, len, MSG_WAITALL);
#endif
}

ssize_t
net_send(socket_t sock, const void *buf, size_t len) {
#if _WIN32
    return send(sock, (const char*)buf, len, 0);
#else
    return send(sock, buf, len, 0);
#endif

}

ssize_t
net_send_all(socket_t sock, const void *buf, size_t len) {
    ssize_t w = 0;
    while (len > 0) {
        #if _WIN32
        w = send(sock, (const char*)buf, len, 0);
        #else
        w = send(sock, buf, len, 0);
        #endif
        if (w == -1) {
            return -1;
        }
        len -= w;
        buf = (char *) buf + w;
    }
    return w;
}

bool
net_close(socket_t sock)
{
    shutdown(sock, SHUT_RDWR);
#ifdef _WIN32
    return !closesocket(sock);
#else
    return !close(sock);
#endif
}

bool
net_init(void) {
#ifdef _WIN32
    WSADATA wsa;
    int res = WSAStartup(MAKEWORD(2, 2), &wsa) < 0;
    if (res < 0) {
        elog("WSAStartup failed with error %d", res);
        return false;
    }
#endif
    return true;
}

void
net_cleanup(void) {
#ifdef _WIN32
    WSACleanup();
#endif
}
