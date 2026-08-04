#include "macos9.h"
#include <stdio.h>
#include <time.h>
#include "utils/ns_errors.h"

/* Extra stubs for missing libNS functions in Carbon */
void macos9_extra_stub_init(void) { }
void closedir(void *p){} void *opendir(const char *n){return 0;}
