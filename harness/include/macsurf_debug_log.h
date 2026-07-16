#ifndef MACSURF_DEBUG_LOG_H
#define MACSURF_DEBUG_LOG_H
void macsurf_debug_log_init(void);
void macsurf_debug_log_close(void);
void macsurf_debug_log_write(const char *s);
void macsurf_debug_log_writef(const char *fmt, ...);
#endif
