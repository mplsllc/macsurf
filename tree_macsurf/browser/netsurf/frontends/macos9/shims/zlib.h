/*
 * MacSurf stub — zlib.h
 * Minimal C89-compatible stub for CodeWarrior 8 compilation.
 *
 * Symbols stubbed:
 *   types:   z_stream, gzFile
 *   consts:  Z_OK, Z_STREAM_END, Z_NO_FLUSH, Z_NULL, MAX_WBITS
 *   funcs:   inflateInit2, inflate, inflateEnd,
 *            gzopen, gzgets, gzclose, gzprintf
 */

#ifndef MACOS9_ZLIB_H
#define MACOS9_ZLIB_H

#include <stddef.h>

#define Z_OK          0
#define Z_STREAM_END  1
#define Z_NO_FLUSH    0
#define Z_NULL        0
#define MAX_WBITS     15

typedef struct z_stream_s {
    const unsigned char *next_in;
    unsigned int         avail_in;
    unsigned long        total_in;

    unsigned char       *next_out;
    unsigned int         avail_out;
    unsigned long        total_out;

    const char          *msg;
    void                *state;

    void *(*zalloc)(void *, unsigned int, unsigned int);
    void  (*zfree)(void *, void *);
    void                *opaque;

    int                  data_type;
    unsigned long        adler;
    unsigned long        reserved;
} z_stream;

typedef void *gzFile;

extern int inflateInit2_(z_stream *strm, int windowBits,
                         const char *version, int stream_size);
#define inflateInit2(strm, windowBits) \
    inflateInit2_((strm), (windowBits), "1.0", (int)sizeof(z_stream))

extern int inflate(z_stream *strm, int flush);
extern int inflateEnd(z_stream *strm);

extern gzFile gzopen(const char *path, const char *mode);
extern char  *gzgets(gzFile file, char *buf, int len);
extern int    gzclose(gzFile file);
extern int    gzprintf(gzFile file, const char *format, ...);

#endif /* MACOS9_ZLIB_H */
