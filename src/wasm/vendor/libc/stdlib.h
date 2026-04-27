#ifndef _STDLIB_H
#define _STDLIB_H
#include <stddef.h>
void* malloc(size_t size);
void  free(void* ptr);
void* realloc(void* ptr, size_t size);
void* calloc(size_t count, size_t size);
static inline int abs(int x) { return x < 0 ? -x : x; }
#endif
