#ifndef MACSURF_DEBUG_H
#define MACSURF_DEBUG_H
#include "macsurf_debug_log.h"
void macsurf_debug_set_title(const char *m);
void macsurf_debug_show_int(const char *l,long v);
void macsurf_debug_log_int(const char *l,long v);
void macsurf_debug_log_str(const char *l,const char *v);
void macsurf_debug_set_title_force(const char *m);
void macsurf_debug_log_int_force(const char *l,long v);
void macsurf_debug_probe_append(const char *m);
void macsurf_debug_probe_append_int(const char *l,long v);
void macsurf_debug_probe_reset(void);
#define MS_LOG(msg)          do { macsurf_debug_log_write(msg); } while(0)
#define MS_BREAK(msg)        do { macsurf_debug_log_write(msg); } while(0)
#define MS_ASSERT(cond, msg) do { if(!(cond)) macsurf_debug_log_write(msg); } while(0)
#endif
