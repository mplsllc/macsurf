/*
 * ostls_log.c -- file-backed logger. See header.
 *
 * Resolves the Desktop folder via FindFolder + the standard
 * kOnSystemDisk / kDesktopFolderType pair. Creates / truncates
 * MacTLSTest.log there. Each write call does:
 *
 *   FSWrite(refnum, &len, buf)
 *   FSWrite(refnum, &cr_len, &cr_byte)
 *   GetFPos(...) / FSSetFork: not needed; FSWrite advances the mark
 *   FlushVol(NULL, vRefNum)
 *
 * The FlushVol is what makes the file survive an immediate crash
 * after a write -- HFS otherwise caches the write internally.
 *
 * Under Retro68 syntax check the Toolbox calls are stubbed so the
 * file parses; nothing is actually written under Linux.
 */

#include "ostls_log.h"

#include <stdarg.h>
#include <string.h>

#if defined(__MWERKS__) || defined(__RETRO68__)  /* Mac target, either toolchain */
#include <Files.h>
#include <Folders.h>
#include <Script.h>             /* smRoman */
#else
/* Non-CW8 stubs. The CW8 File Manager exposes file refnums as plain
 * 'short' (FSIORefNum is a newer Carbon typedef and isn't available
 * on CW8's Universal Interfaces), so we keep the type small here too.
 */
typedef long OSStatus;
typedef long FSDirID;
typedef struct {
    short vRefNum;
    long parID;
    unsigned char name[64];
} FSSpec;
#define noErr 0
#define kOnSystemDisk -32768
#define kDesktopFolderType 'desk'
#define fsRdWrPerm 3
#define smRoman 0
static OSStatus FindFolder(short v, long t, unsigned char create,
    short *out_v, long *out_d){(void)v;(void)t;(void)create;
    *out_v=0;*out_d=0;return noErr;}
static OSStatus FSMakeFSSpec(short v, long d, const unsigned char *n, FSSpec *s){
    (void)v;(void)d;(void)n;(void)s;return noErr;}
static OSStatus FSpDelete(const FSSpec *s){(void)s;return noErr;}
static OSStatus FSpCreate(const FSSpec *s, unsigned long c, unsigned long t,
    short script){(void)s;(void)c;(void)t;(void)script;return noErr;}
static OSStatus FSpOpenDF(const FSSpec *s, char perm, short *r){
    (void)s;(void)perm;*r=0;return noErr;}
static OSStatus FSWrite(short r, long *count, const void *buf){
    (void)r;(void)buf;return noErr;}
static OSStatus FSClose(short r){(void)r;return noErr;}
static OSStatus FlushVol(const unsigned char *name, short v){(void)name;(void)v;return noErr;}
#endif


/* ----------------------------------------------------------------- */
/* State                                                             */
/* ----------------------------------------------------------------- */

/*
 * File refnum is plain 'short' under classic File Manager and CW8
 * Universal Interfaces. FSIORefNum is the modern Carbon typedef that
 * isn't reachable from CW8's headers; using 'short' keeps both
 * toolchains parsing the same code.
 */
static short gLogRef = 0;
static short gLogVol = 0;
static int   gLogOpen = 0;


/* ----------------------------------------------------------------- */
/* Helpers                                                           */
/* ----------------------------------------------------------------- */

/*
 * Pascal-string copy from C, bounded by Str63 capacity. The destination
 * is a 64-byte buffer where byte 0 is length and bytes 1..63 are the
 * characters; we cap at 63 chars.
 */
static void
c_to_pstr_63(const char *src, unsigned char *dst)
{
    size_t n = strlen(src);
    if (n > 63) n = 63;
    dst[0] = (unsigned char)n;
    memcpy(&dst[1], src, n);
}


/*
 * Append n characters from src into buf, advancing *pos. Stops when
 * *pos >= cap-1; always leaves room for a terminating NUL written by
 * the caller.
 */
static void
buf_append(char *buf, size_t cap, size_t *pos, const char *src, size_t n)
{
    size_t i;
    for (i = 0; i < n && *pos < cap - 1; i++) {
        buf[(*pos)++] = src[i];
    }
}


/*
 * Render an unsigned long in base 10 or 16 (lowercase or uppercase).
 * Optionally zero-pad to a minimum width up to 16. Writes into buf at
 * *pos, advancing it. base must be 10 or 16.
 */
static void
buf_append_uint(char *buf, size_t cap, size_t *pos,
                unsigned long v, int base, int upper, int min_width)
{
    char tmp[24];
    int n;
    int i;
    const char *digits;

    digits = upper
        ? "0123456789ABCDEF"
        : "0123456789abcdef";

    n = 0;
    if (v == 0) {
        tmp[n++] = '0';
    } else {
        while (v > 0 && n < (int)sizeof(tmp)) {
            tmp[n++] = digits[v % (unsigned long)base];
            v /= (unsigned long)base;
        }
    }
    /* Pad. */
    while (n < min_width && n < (int)sizeof(tmp)) {
        tmp[n++] = '0';
    }
    /* Reverse into buf. */
    for (i = n - 1; i >= 0; i--) {
        if (*pos < cap - 1) {
            buf[(*pos)++] = tmp[i];
        }
    }
}


/*
 * Render a signed long in base 10 (with leading '-' for negatives).
 */
static void
buf_append_int(char *buf, size_t cap, size_t *pos, long v)
{
    unsigned long u;
    if (v < 0) {
        if (*pos < cap - 1) {
            buf[(*pos)++] = '-';
        }
        /* Avoid INT_MIN overflow by casting through unsigned. */
        u = (unsigned long)(-(v + 1)) + 1UL;
    } else {
        u = (unsigned long)v;
    }
    buf_append_uint(buf, cap, pos, u, 10, 0, 0);
}


/* ----------------------------------------------------------------- */
/* Hand-rolled formatter                                             */
/* ----------------------------------------------------------------- */

static size_t
format_into(char *buf, size_t cap, const char *fmt, va_list ap)
{
    size_t pos = 0;
    const char *p;

    if (cap == 0) return 0;

    for (p = fmt; *p != '\0' && pos < cap - 1; p++) {
        char c;
        int is_long;
        int min_width;
        c = *p;
        if (c != '%') {
            buf[pos++] = c;
            continue;
        }
        p++;                            /* consume '%' */
        is_long = 0;
        min_width = 0;

        /* Optional zero-pad width like %04X. We support 0..16. */
        if (*p == '0') {
            p++;
            while (*p >= '0' && *p <= '9') {
                min_width = min_width * 10 + (*p - '0');
                p++;
            }
            if (min_width > 16) min_width = 16;
        }

        if (*p == 'l') {
            is_long = 1;
            p++;
        }

        switch (*p) {
        case 's':
            {
                const char *s = va_arg(ap, const char *);
                if (s == NULL) s = "(null)";
                buf_append(buf, cap, &pos, s, strlen(s));
            }
            break;
        case 'c':
            {
                int ci = va_arg(ap, int);
                if (pos < cap - 1) buf[pos++] = (char)ci;
            }
            break;
        case 'd':
            if (is_long) {
                long v = va_arg(ap, long);
                buf_append_int(buf, cap, &pos, v);
            } else {
                int v = va_arg(ap, int);
                buf_append_int(buf, cap, &pos, (long)v);
            }
            break;
        case 'u':
            if (is_long) {
                unsigned long v = va_arg(ap, unsigned long);
                buf_append_uint(buf, cap, &pos, v, 10, 0, min_width);
            } else {
                unsigned int v = va_arg(ap, unsigned int);
                buf_append_uint(buf, cap, &pos, (unsigned long)v, 10, 0, min_width);
            }
            break;
        case 'x':
            if (is_long) {
                unsigned long v = va_arg(ap, unsigned long);
                buf_append_uint(buf, cap, &pos, v, 16, 0, min_width);
            } else {
                unsigned int v = va_arg(ap, unsigned int);
                buf_append_uint(buf, cap, &pos, (unsigned long)v, 16, 0, min_width);
            }
            break;
        case 'X':
            if (is_long) {
                unsigned long v = va_arg(ap, unsigned long);
                buf_append_uint(buf, cap, &pos, v, 16, 1, min_width);
            } else {
                unsigned int v = va_arg(ap, unsigned int);
                buf_append_uint(buf, cap, &pos, (unsigned long)v, 16, 1, min_width);
            }
            break;
        case 'p':
            {
                void *v = va_arg(ap, void *);
                buf_append_uint(buf, cap, &pos, (unsigned long)v, 16, 1, 8);
            }
            break;
        case '%':
            if (pos < cap - 1) buf[pos++] = '%';
            break;
        default:
            /* Unknown specifier: echo the '%' and the char. */
            if (pos < cap - 1) buf[pos++] = '%';
            if (pos < cap - 1) buf[pos++] = *p;
            break;
        }
    }

    buf[pos] = '\0';
    return pos;
}


/* ----------------------------------------------------------------- */
/* Public API                                                        */
/* ----------------------------------------------------------------- */

OSErr
OSTLS_LogInit(void)
{
    long desk_dir;
    short desk_vol;
    FSSpec spec;
    OSStatus err;
    const unsigned char log_name_pstr[] = "\pMacTLSTest.log";

    if (gLogOpen) {
        OSTLS_LogClose();
    }

    err = FindFolder(kOnSystemDisk, kDesktopFolderType,
                     0 /* don't create */,
                     &desk_vol, &desk_dir);
    if (err != noErr) {
        return (OSErr)err;
    }

    err = FSMakeFSSpec(desk_vol, desk_dir, log_name_pstr, &spec);
    /* fnfErr = -43 is the expected "doesn't exist yet" result --
     * FSMakeFSSpec still populates 'spec' so FSpCreate works. */
    if (err != noErr && err != -43 /* fnfErr */) {
        return (OSErr)err;
    }

    /* Delete any existing log so each run starts fresh. */
    (void)FSpDelete(&spec);

    err = FSpCreate(&spec, 'ttxt', 'TEXT', smRoman);
    if (err != noErr) {
        return (OSErr)err;
    }

    err = FSpOpenDF(&spec, fsRdWrPerm, &gLogRef);
    if (err != noErr) {
        return (OSErr)err;
    }

    gLogVol = spec.vRefNum;
    gLogOpen = 1;
    return noErr;
}


void
OSTLS_LogClose(void)
{
    if (gLogOpen) {
        FSClose(gLogRef);
        FlushVol(NULL, gLogVol);
        gLogOpen = 0;
    }
}


static void
write_buf(const char *buf, size_t len)
{
    long count;
    unsigned char cr;

    if (!gLogOpen) {
        return;
    }
    count = (long)len;
    FSWrite(gLogRef, &count, buf);
    cr = 0x0D;                  /* Mac CR line ending */
    count = 1L;
    FSWrite(gLogRef, &count, &cr);
    FlushVol(NULL, gLogVol);
}


void
OSTLS_LogLine(const char *s)
{
    if (!gLogOpen || s == NULL) {
        return;
    }
    write_buf(s, strlen(s));
}


void
OSTLS_LogLinef(const char *fmt, ...)
{
    char buf[256];
    size_t n;
    va_list ap;

    if (!gLogOpen || fmt == NULL) {
        return;
    }

    va_start(ap, fmt);
    n = format_into(buf, sizeof buf, fmt, ap);
    va_end(ap);

    write_buf(buf, n);
}


void
OSTLS_LogBlank(void)
{
    if (!gLogOpen) {
        return;
    }
    write_buf("", 0);
}
