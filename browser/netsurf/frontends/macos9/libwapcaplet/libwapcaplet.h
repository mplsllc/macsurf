/*
 * MacSurf stub — libwapcaplet/libwapcaplet.h
 * Minimal C89-compatible stub for CodeWarrior 8 compilation.
 *
 * Symbols stubbed:
 *   types:  lwc_string, lwc_error, lwc_hash, lwc_refcounter,
 *           lwc_iteration_callback_fn
 *   enums:  lwc_error_ok, lwc_error_oom, lwc_error_range
 *   funcs:  lwc_intern_string, lwc_iterate_strings,
 *           lwc_string_destroy, lwc__intern_caseless_string
 *   macros: lwc_string_ref, lwc_string_unref, lwc_string_isequal,
 *           lwc_string_caseless_isequal, lwc_string_data,
 *           lwc_string_length, lwc_string_hash_value
 */

#ifndef libwapcaplet_h_
#define libwapcaplet_h_

#include <stddef.h>

/* --- bool compatibility (C89) ---
 * true/false come from MacTypes.h as enum constants;
 * do NOT redefine them as macros.
 */
#ifndef bool
typedef unsigned char bool;
#endif

typedef unsigned long lwc_refcounter;
typedef unsigned long lwc_hash;

typedef struct lwc_string_s {
    struct lwc_string_s **prevptr;
    struct lwc_string_s  *next;
    size_t                len;
    lwc_hash              hash;
    lwc_refcounter        refcnt;
    struct lwc_string_s  *insensitive;
} lwc_string;

typedef void (*lwc_iteration_callback_fn)(lwc_string *str, void *pw);

typedef enum lwc_error_e {
    lwc_error_ok    = 0,
    lwc_error_oom   = 1,
    lwc_error_range = 2
} lwc_error;

/* --- extern functions --- */

extern lwc_error lwc_intern_string(const char *s, size_t slen,
                                   lwc_string **ret);

extern void lwc_string_destroy(lwc_string *str);

extern lwc_error lwc__intern_caseless_string(lwc_string *str);

extern void lwc_iterate_strings(lwc_iteration_callback_fn cb, void *pw);

/* --- lwc_string_ref --- */
#define lwc_string_ref(str) \
    (((str) != NULL) ? ((str)->refcnt++, (str)) : (str))

/* --- lwc_string_unref --- */
#define lwc_string_unref(str) do {                             \
        lwc_string *_lwc_s = (str);                            \
        if (_lwc_s != NULL) {                                  \
            _lwc_s->refcnt--;                                  \
            if ((_lwc_s->refcnt == 0) ||                       \
                ((_lwc_s->refcnt == 1) &&                      \
                 (_lwc_s->insensitive == _lwc_s)))             \
                lwc_string_destroy(_lwc_s);                    \
        }                                                      \
    } while (0)

/* --- lwc_string_isequal --- */
#define lwc_string_isequal(str1, str2, ret) \
    ((*(ret) = (unsigned char)((str1) == (str2))), lwc_error_ok)

/* --- lwc_string_caseless_isequal ---
 * Use unsigned char * instead of bool * to avoid CW8 type
 * mismatch between MacTypes.h bool and stdbool.h bool.
 */
static lwc_error
lwc_string_caseless_isequal(lwc_string *str1, lwc_string *str2,
                            unsigned char *ret)
{
    lwc_error err = lwc_error_ok;
    if (str1->insensitive == NULL)
        err = lwc__intern_caseless_string(str1);
    if (err == lwc_error_ok && str2->insensitive == NULL)
        err = lwc__intern_caseless_string(str2);
    if (err == lwc_error_ok)
        *ret = (unsigned char)(str1->insensitive == str2->insensitive);
    return err;
}

/* --- lwc_string_data --- */
#define lwc_string_data(str) ((const char *)((str) + 1))

/* --- lwc_string_length --- */
#define lwc_string_length(str) ((str)->len)

/* --- lwc_string_hash_value --- */
#define lwc_string_hash_value(str) ((str)->hash)

#endif /* libwapcaplet_h_ */
