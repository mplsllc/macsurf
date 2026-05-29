/*
 * mlink_log.c — file-backed diagnostic log for macLink
 *
 * Same pattern as MacSurf's macsurf_debug_log.c: writes to a text
 * file on the Desktop, flushed after every line so it survives
 * crashes. Each entry is one CR-terminated line.
 *
 * Format spec set (must stay in sync with mlink_log.h):
 *   %d   — int (signed)
 *   %ld  — long (signed)
 *   %p   — pointer (hex)
 *   %s   — null-terminated C string
 *   %%   — literal '%'
 * Anything else prints literally without consuming an argument.
 *
 * Hard cap on emitted line length: 255 bytes (one Str255 buffer plus
 * defensive margin).
 *
 * CW8 C89 — no inline, no //, no for-scope decls.
 */
#include <stddef.h>
#include <stdarg.h>
#include <string.h>
#include <Files.h>
#include <Folders.h>
#include <Errors.h>

#include "mlink_log.h"

/* ------------------------------------------------------------------ */
/* Internal state                                                     */
/* ------------------------------------------------------------------ */

static short g_log_ref       = 0;   /* HFS file refnum; 0 = closed   */
static short g_log_vRefNum   = 0;
static long  g_log_dirID     = 0;
static int   g_log_open      = 0;
static const char k_log_name[] = "macLink Debug.log";

/* ------------------------------------------------------------------ */
/* Helpers                                                            */
/* ------------------------------------------------------------------ */

static void mlink__cstr_to_p255(const char *src, unsigned char *dst)
{
    long n;
    n = 0;
    if (src != NULL) {
        while (src[n] != 0 && n < 255) {
            n++;
        }
    }
    dst[0] = (unsigned char)n;
    if (n > 0) {
        memcpy(dst + 1, src, (size_t)n);
    }
}

static int mlink__append_str(char *buf, int *pos, int cap, const char *s)
{
    int n;
    if (s == NULL) s = "(null)";
    n = (int)strlen(s);
    if (*pos + n >= cap) {
        n = cap - *pos - 1;
        if (n < 0) n = 0;
    }
    if (n > 0) {
        memcpy(buf + *pos, s, (size_t)n);
        *pos += n;
    }
    return n;
}

static int mlink__append_int(char *buf, int *pos, int cap, long v)
{
    char tmp[24];
    int  t;
    int  neg;
    int  written;
    neg = 0;
    if (v < 0) {
        neg = 1;
        v = -v;
    }
    t = 0;
    if (v == 0) {
        tmp[t++] = '0';
    } else {
        while (v > 0 && t < (int)sizeof tmp) {
            tmp[t++] = (char)('0' + (int)(v % 10));
            v /= 10;
        }
    }
    written = 0;
    if (neg && *pos + 1 < cap) {
        buf[(*pos)++] = '-';
        written++;
    }
    while (t > 0 && *pos + 1 < cap) {
        t--;
        buf[(*pos)++] = tmp[t];
        written++;
    }
    return written;
}

static int mlink__append_hex(char *buf, int *pos, int cap, unsigned long v)
{
    static const char hex_digits[] = "0123456789ABCDEF";
    char tmp[16];
    int  t;
    int  written;
    t = 0;
    if (v == 0) {
        tmp[t++] = '0';
    } else {
        while (v > 0 && t < (int)sizeof tmp) {
            tmp[t++] = hex_digits[v & 0xF];
            v >>= 4;
        }
    }
    written = 0;
    if (*pos + 2 < cap) {
        buf[(*pos)++] = '0';
        buf[(*pos)++] = 'x';
        written += 2;
    }
    while (t > 0 && *pos + 1 < cap) {
        t--;
        buf[(*pos)++] = tmp[t];
        written++;
    }
    return written;
}

/* ------------------------------------------------------------------ */
/* Init / shutdown                                                    */
/* ------------------------------------------------------------------ */

int mlink_log_init(void)
{
    FSSpec      spec;
    OSErr       err;
    short       v;
    long        d;
    unsigned char p_name[64];

    if (g_log_open) return 0;

    err = FindFolder(kOnSystemDisk, kDesktopFolderType,
                     kCreateFolder, &v, &d);
    if (err != noErr) return -1;

    mlink__cstr_to_p255(k_log_name, p_name);

    err = FSMakeFSSpec(v, d, p_name, &spec);
    if (err == fnfErr) {
        err = FSpCreate(&spec, 'CWIE', 'TEXT', smSystemScript);
        if (err != noErr) return -1;
    } else if (err != noErr) {
        return -1;
    }

    err = FSpOpenDF(&spec, fsRdWrPerm, &g_log_ref);
    if (err != noErr) return -1;

    /* Append: seek to EOF before the first write */
    err = SetFPos(g_log_ref, fsFromLEOF, 0);
    if (err != noErr) {
        FSClose(g_log_ref);
        g_log_ref = 0;
        return -1;
    }

    g_log_vRefNum = v;
    g_log_dirID   = d;
    g_log_open    = 1;

    mlink_log("=== macLink startup ===");
    return 0;
}

void mlink_log_close(void)
{
    if (!g_log_open) return;
    FSClose(g_log_ref);
    g_log_ref  = 0;
    g_log_open = 0;
}

/* ------------------------------------------------------------------ */
/* Public write                                                       */
/* ------------------------------------------------------------------ */

void mlink_log(const char *s)
{
    char  line[300];
    int   pos;
    int   slen;
    long  to_write;

    if (!g_log_open) return;
    if (s == NULL) return;

    pos = 0;
    line[pos++] = '[';
    line[pos++] = 'm';
    line[pos++] = 'l';
    line[pos++] = 'i';
    line[pos++] = 'n';
    line[pos++] = 'k';
    line[pos++] = ']';
    line[pos++] = ' ';

    slen = (int)strlen(s);
    if (pos + slen >= (int)sizeof line - 2) {
        slen = (int)sizeof line - 2 - pos;
        if (slen < 0) slen = 0;
    }
    if (slen > 0) {
        memcpy(line + pos, s, (size_t)slen);
        pos += slen;
    }
    line[pos++] = '\r';

    to_write = (long)pos;
    FSWrite(g_log_ref, &to_write, line);
    FlushVol(NULL, g_log_vRefNum);
}

void mlink_logf(const char *fmt, ...)
{
    char     line[300];
    int      pos;
    int      cap;
    va_list  ap;
    const char *p;
    long     to_write;

    if (!g_log_open) return;
    if (fmt == NULL) return;

    pos = 0;
    cap = (int)sizeof line - 2;  /* reserve room for CR + safety byte */

    line[pos++] = '[';
    line[pos++] = 'm';
    line[pos++] = 'l';
    line[pos++] = 'i';
    line[pos++] = 'n';
    line[pos++] = 'k';
    line[pos++] = ']';
    line[pos++] = ' ';

    va_start(ap, fmt);
    p = fmt;
    while (*p != 0 && pos < cap) {
        if (*p != '%') {
            line[pos++] = *p++;
            continue;
        }
        p++;
        if (*p == 0) break;
        if (*p == '%') {
            line[pos++] = '%';
            p++;
        } else if (*p == 'd') {
            (void)mlink__append_int(line, &pos, cap, (long)va_arg(ap, int));
            p++;
        } else if (*p == 'l' && *(p+1) == 'd') {
            (void)mlink__append_int(line, &pos, cap, va_arg(ap, long));
            p += 2;
        } else if (*p == 'p') {
            (void)mlink__append_hex(line, &pos, cap,
                                    (unsigned long)va_arg(ap, void *));
            p++;
        } else if (*p == 's') {
            (void)mlink__append_str(line, &pos, cap, va_arg(ap, const char *));
            p++;
        } else {
            /* Unknown specifier — emit literally, do not consume arg */
            line[pos++] = '%';
            if (pos < cap) line[pos++] = *p;
            p++;
        }
    }
    va_end(ap);

    line[pos++] = '\r';

    to_write = (long)pos;
    FSWrite(g_log_ref, &to_write, line);
    FlushVol(NULL, g_log_vRefNum);
}
