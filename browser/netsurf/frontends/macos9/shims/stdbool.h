#ifndef MACSURF_STDBOOL_H
#define MACSURF_STDBOOL_H

/* 
 * MSL doesn't provide a C99-compliant stdbool.h in C89 mode.
 * Force-include MacTypes.h first to get its enum-based true/false.
 */

#ifdef __MWERKS__
  #ifndef __MACTYPES__
    #include <MacTypes.h>
  #endif
#endif

#ifndef __cplusplus

/* MacTypes.h provides: enum { false = 0, true = 1 }; 
 * If they are not already macros, we can leave them as enum constants.
 * But we must define the bool type if it is not a keyword.
 */

#ifndef bool
  #define bool unsigned char
#endif

/* Standard C99 requirements */
#ifndef true
  #define true 1
#endif
#ifndef false
  #define false 0
#endif

#define __bool_true_false_are_defined 1

#endif /* __cplusplus */

#endif /* MACSURF_STDBOOL_H */
