#ifndef MACSURF_SYS_SELECT_H
#define MACSURF_SYS_SELECT_H

typedef struct {
    int fds_bits[1];
} fd_set;

#define FD_SET(n, p)   ((void)0)
#define FD_CLR(n, p)   ((void)0)
#define FD_ISSET(n, p) (0)
#define FD_ZERO(p)     ((void)0)

#endif
