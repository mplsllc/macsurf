#ifndef MACSURF_SHIMS_STDBOOL_H
#define MACSURF_SHIMS_STDBOOL_H
#ifdef __RETRO68__
/* GCC provides _Bool as a built-in type. Map it to bool.
 * true/false come from Universal MacTypes.h enum (included via prefix). */
#ifndef __cplusplus
#define bool _Bool
#endif
#elif defined(__MWERKS__)
#ifndef __MACTYPES__
#include <MacTypes.h>
#endif
#ifndef __cplusplus
#define bool unsigned char
#define true 1
#define false 0
#define __bool_true_false_are_defined 1
#endif
#else
#include <stdbool.h>
#endif
#endif
