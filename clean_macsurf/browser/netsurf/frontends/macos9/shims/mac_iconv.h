#ifndef ICONV_H
#define ICONV_H

#include <stddef.h>

typedef void* iconv_t;

iconv_t iconv_open(const char *to, const char *from);
size_t  iconv(iconv_t cd, char **inbuf, size_t *inbytesleft,
              char **outbuf, size_t *outbytesleft);
int     iconv_close(iconv_t cd);

#endif
