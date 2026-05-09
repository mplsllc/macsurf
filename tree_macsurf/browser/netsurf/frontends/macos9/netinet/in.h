/*
 * MacSurf stub -- netinet/in.h
 * Minimal C89-compatible stub for CodeWarrior 8 compilation.
 * Mac OS 9 networking uses Open Transport, not BSD sockets.
 */

#ifndef MACOS9_NETINET_IN_H
#define MACOS9_NETINET_IN_H

struct in_addr {
	unsigned long s_addr;
};

struct sockaddr_in {
	unsigned char  sin_len;
	unsigned char  sin_family;
	unsigned short sin_port;
	struct in_addr sin_addr;
	char           sin_zero[8];
};

#endif /* MACOS9_NETINET_IN_H */
