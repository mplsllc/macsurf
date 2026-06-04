/*
 * MacSurf — macsurf_es6.h
 * ES6 -> ES5 source transpiler (Stage 1: let/const -> var). See macsurf_es6.c.
 */
#ifndef MACSURF_ES6_H
#define MACSURF_ES6_H

#include <stddef.h>

/* Transpile src[0..len) into out (cap bytes incl. NUL). Returns bytes written
 * (excl. NUL), or 0 on overflow. Output is never longer than the input. */
size_t macsurf_es6_transpile(const char *src, size_t len, char *out, size_t cap);

#endif
