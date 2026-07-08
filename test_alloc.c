/*
 * MacSurf -- test_alloc.c
 *
 * Standalone test driver to verify that allocations in libwapcaplet,
 * libdom, and libcss are successfully intercepted by the safe allocator
 * macros.
 *
 * Compile and link this with libwapcaplet, libdom, or libcss source files
 * to prove the redirect works.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <dom/dom.h>
#include <libcss/libcss.h>
#include <libwapcaplet/libwapcaplet.h>

/* Global counters to track allocations */
static int safe_alloc_count = 0;
static int safe_calloc_count = 0;
static int safe_realloc_count = 0;

/* Define the safe allocator functions to print interception traces */
void *macsurf_safe_alloc(size_t size)
{
    safe_alloc_count++;
    printf("macsurf_safe_alloc called for size: %lu\n", (unsigned long)size);
    return malloc(size);
}

void *macsurf_safe_calloc(size_t count, size_t size)
{
    safe_calloc_count++;
    printf("macsurf_safe_calloc called for count: %lu, size: %lu\n", (unsigned long)count, (unsigned long)size);
    return calloc(count, size);
}

void *macsurf_safe_realloc(void *ptr, size_t size)
{
    safe_realloc_count++;
    printf("macsurf_safe_realloc called for size: %lu\n", (unsigned long)size);
    return realloc(ptr, size);
}

int main(void)
{
    lwc_string *str = NULL;
    lwc_error err;

    printf("Starting allocator interception test...\n");

    /* Exercise libwapcaplet (string internment) */
    printf("\n--- Testing libwapcaplet allocation ---\n");
    err = lwc_intern_string("test_string", 11, &str);
    if (err == lwc_error_ok && str != NULL) {
        printf("Interned string successfully: %s\n", lwc_string_data(str));
        lwc_string_unref(str);
    } else {
        printf("Failed to intern string.\n");
    }

    /* Print summary of captured allocations */
    printf("\n--- Interception Summary ---\n");
    printf("macsurf_safe_alloc called: %d times\n", safe_alloc_count);
    printf("macsurf_safe_calloc called: %d times\n", safe_calloc_count);
    printf("macsurf_safe_realloc called: %d times\n", safe_realloc_count);

    if (safe_alloc_count > 0 || safe_calloc_count > 0 || safe_realloc_count > 0) {
        printf("\nSUCCESS: Allocations were successfully intercepted!\n");
        return 0;
    } else {
        printf("\nFAILURE: Allocations bypassed the hooks.\n");
        return 1;
    }
}
