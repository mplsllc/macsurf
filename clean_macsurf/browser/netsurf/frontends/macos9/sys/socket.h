/*
 * MacSurf stub -- sys/socket.h
 * Minimal C89-compatible stub for CodeWarrior 8 compilation.
 * Mac OS 9 networking uses Open Transport, not BSD sockets.
 */

#ifndef MACOS9_SYS_SOCKET_H
#define MACOS9_SYS_SOCKET_H

#ifndef AF_INET
#define AF_INET  2
#endif
#ifndef AF_INET6
#define AF_INET6 30
#endif
#ifndef SOCK_STREAM
#define SOCK_STREAM 1
#endif
#ifndef SOCK_DGRAM
#define SOCK_DGRAM  2
#endif

typedef unsigned int socklen_t;

#endif /* MACOS9_SYS_SOCKET_H */
