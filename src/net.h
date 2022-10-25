// Copyright (C) 2021 DEV47APPS, github.com/dev47apps
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef _WIN32
  #include <winsock2.h>
  #define SHUT_RD SD_RECEIVE
  #define SHUT_WR SD_SEND
  #define SHUT_RDWR SD_BOTH
  #define poll WSAPoll
  #define WSAErrno errno=WSAGetLastError
  typedef SOCKET socket_t;
  typedef int ssize_t;
#else
  #define INVALID_SOCKET -1
  #define WSAErrno
  typedef int socket_t;
#endif

bool net_init(void);
void net_cleanup(void);
bool net_close(socket_t sock);
socket_t net_accept(socket_t sock);

socket_t
net_connect(struct addrinfo *addr, uint16_t port);

socket_t
net_connect(const char* host, uint16_t port);

socket_t
net_listen(const char* addr, uint16_t port);

int
net_listen_port(socket_t sock);

ssize_t
net_recv(socket_t sock, void *buf, size_t len);

ssize_t
net_recv_peek(socket_t sock);

ssize_t
net_recv_all(socket_t sock, void *buf, size_t len);

ssize_t
net_send(socket_t sock, const void *buf, size_t len);

ssize_t
net_send_all(socket_t sock, const void *buf, size_t len);

int
set_recv_timeout(socket_t sock, int tv_sec);

bool
set_nonblock(socket_t sock, int nonblock);
