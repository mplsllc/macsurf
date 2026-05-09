/*
 * mac_inet.h — BSD socket type stubs for Mac OS 9 / NetSurf
 *
 * Provides the subset of sys/socket.h, netinet/in.h, and arpa/inet.h
 * types and constants that NetSurf core headers reference. Actual
 * networking goes through Open Transport in the frontend fetch layer.
 */

#ifndef MACOS9_SHIMS_MAC_INET_H
#define MACOS9_SHIMS_MAC_INET_H

#include <stdint.h>
#include <errno.h>

#ifndef EAFNOSUPPORT
#define EAFNOSUPPORT 47
#endif

/* Address families */
#ifndef AF_INET
#define AF_INET 2
#endif

#ifndef AF_INET6
#define AF_INET6 30
#endif

/* Socket types */
#ifndef SOCK_STREAM
#define SOCK_STREAM 1
#endif

#ifndef SOCK_DGRAM
#define SOCK_DGRAM 2
#endif

/* in_addr_t */
typedef uint32_t in_addr_t;

/* struct in_addr */
struct in_addr {
	in_addr_t s_addr;
};

/* struct sockaddr_in */
struct sockaddr_in {
	uint8_t        sin_len;
	uint8_t        sin_family;
	uint16_t       sin_port;
	struct in_addr sin_addr;
	char           sin_zero[8];
};

/* struct sockaddr — generic */
struct sockaddr {
	uint8_t  sa_len;
	uint8_t  sa_family;
	char     sa_data[14];
};

typedef uint32_t socklen_t;

/*
 * Byte-order macros.
 * PowerPC is big-endian — network byte order matches host order.
 * Provide correct byte-swap for little-endian cross-compilation.
 */
#if defined(__BIG_ENDIAN__) || defined(__ppc__) || defined(__POWERPC__)

#define htons(x) ((uint16_t)(x))
#define ntohs(x) ((uint16_t)(x))
#define htonl(x) ((uint32_t)(x))
#define ntohl(x) ((uint32_t)(x))

#else

static uint16_t htons(uint16_t x) {
	return (uint16_t)((x >> 8) | (x << 8));
}
static uint16_t ntohs(uint16_t x) {
	return (uint16_t)((x >> 8) | (x << 8));
}
static uint32_t htonl(uint32_t x) {
	return ((x >> 24) & 0x000000FF) |
	       ((x >>  8) & 0x0000FF00) |
	       ((x <<  8) & 0x00FF0000) |
	       ((x << 24) & 0xFF000000);
}
static uint32_t ntohl(uint32_t x) {
	return htonl(x);
}

#endif

/*
 * fd_set stub — Mac OS 9 uses Open Transport notifiers, not select().
 * This typedef exists solely so fetch.h function signatures parse.
 */
#ifndef FD_SETSIZE
#define FD_SETSIZE 64
#endif

typedef struct {
	unsigned long fds_bits[FD_SETSIZE / (8 * sizeof(unsigned long))];
} fd_set;

#define FD_ZERO(set)      do { unsigned _i; for (_i = 0; _i < sizeof((set)->fds_bits)/sizeof((set)->fds_bits[0]); _i++) (set)->fds_bits[_i] = 0; } while (0)
#define FD_SET(fd, set)   ((set)->fds_bits[(fd) / (8 * sizeof(unsigned long))] |=  (1UL << ((fd) % (8 * sizeof(unsigned long)))))
#define FD_CLR(fd, set)   ((set)->fds_bits[(fd) / (8 * sizeof(unsigned long))] &= ~(1UL << ((fd) % (8 * sizeof(unsigned long)))))
#define FD_ISSET(fd, set) ((set)->fds_bits[(fd) / (8 * sizeof(unsigned long))] &   (1UL << ((fd) % (8 * sizeof(unsigned long)))))

/*
 * socket() stub — MacSurf overrides gui_fetch_table.socket_open with
 * an Open Transport implementation, so this default is never called.
 * Exists only so gui_factory.c's gui_default_socket_open() compiles.
 */
static int socket(int domain, int type, int protocol)
{
	(void)domain; (void)type; (void)protocol;
	return -1;
}

/*
 * inet_aton is NOT provided here — NetSurf's utils/utils.c supplies
 * its own fallback when HAVE_INETATON is undefined.
 */

#endif /* MACOS9_SHIMS_MAC_INET_H */
