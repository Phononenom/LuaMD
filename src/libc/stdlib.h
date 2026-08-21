/* Freestanding stand-in for <stdlib.h>. Arena allocator in src/shim.c. */
#ifndef LUACOREEMU_STDLIB_H
#define LUACOREEMU_STDLIB_H

#include <stddef.h>

#ifndef NULL
#define NULL ((void *)0)
#endif

#define EXIT_SUCCESS 0
#define EXIT_FAILURE 1

void *malloc(size_t n);
void *calloc(size_t n, size_t sz);
void *realloc(void *p, size_t n);
void  free(void *p);
int   abs(int v);

#endif
