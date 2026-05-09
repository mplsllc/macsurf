#ifndef MACSURF_STDBOOL_H
#define MACSURF_STDBOOL_H

/* MSL doesn't provide a C99-compliant stdbool.h in C89 mode.
 * CW8 PPC has "Enable bool" option which makes bool/true/false keywords.
 * If that is OFF, we must provide them.
 */

#ifndef __cplusplus

#ifdef __MWERKS__
  /* In CW8, bool is a keyword if "Enable bool" is checked in the 
   * C/C++ Language panel. There is no easy way to detect it from 
   * the preprocessor, so we rely on the fact that if it is a keyword,
   * it is not a macro.  But if it is a keyword, we must NOT typedef it.
   */
  #ifndef __MACTYPES__
  #include <MacTypes.h>
  #endif
  
  /* MacTypes.h provides true/false as enum constants. 
   * We just need to make sure bool is defined.
   */
  #ifndef bool
    #define bool unsigned char
  #endif
  
#else
  /* Non-MWERKS (Linux cross-check) */
  #ifndef bool
    #define bool unsigned char
    #define true 1
    #define false 0
  #endif
#endif

#define __bool_true_false_are_defined 1

#endif /* __cplusplus */

#endif /* MACSURF_STDBOOL_H */
