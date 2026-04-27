/* Freestanding libc replacements + TLSF allocator wrappers */

#include "hlp.h"

/* --- Memory functions --- */

void* memcpy(void* dst, const void* src, size_t n) {
    uint8_t* d = dst;
    const uint8_t* s = src;
    while (n--) *d++ = *s++;
    return dst;
}

void* memset(void* dst, int c, size_t n) {
    uint8_t* d = dst;
    while (n--) *d++ = (uint8_t)c;
    return dst;
}

int memcmp(const void* a, const void* b, size_t n) {
    const uint8_t* pa = a;
    const uint8_t* pb = b;
    while (n--) {
        if (*pa != *pb) return *pa - *pb;
        pa++; pb++;
    }
    return 0;
}

void* memmove(void* dst, const void* src, size_t n) {
    uint8_t* d = dst;
    const uint8_t* s = src;
    if (d < s) {
        while (n--) *d++ = *s++;
    } else {
        d += n; s += n;
        while (n--) *--d = *--s;
    }
    return dst;
}

/* --- String functions --- */

size_t strlen(const char* s) {
    const char* p = s;
    while (*p) p++;
    return (size_t)(p - s);
}

int strcmp(const char* a, const char* b) {
    while (*a && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}

int strncmp(const char* a, const char* b, size_t n) {
    while (n && *a && *a == *b) { a++; b++; n--; }
    return n ? (unsigned char)*a - (unsigned char)*b : 0;
}

char* strcpy(char* dst, const char* src) {
    char* d = dst;
    while ((*d++ = *src++));
    return dst;
}

char* strncpy(char* dst, const char* src, size_t n) {
    char* d = dst;
    while (n && (*d++ = *src++)) n--;
    while (n--) *d++ = '\0';
    return dst;
}

void* memchr(const void* s, int c, size_t n) {
    const uint8_t* p = s;
    while (n--) {
        if (*p == (uint8_t)c) return (void*)p;
        p++;
    }
    return 0;
}

/* --- TLSF allocator --- */

#define tlsf_assert(x) ((void)0)
#include "tlsf.h"

static tlsf_t g_tlsf = 0;

static void grow_heap(size_t needed) {
    size_t cur = __builtin_wasm_memory_size(0) * 65536;
    size_t want = cur + (needed > 65536 ? needed : 65536);
    size_t pages = (want - cur + 65535) / 65536;
    if (__builtin_wasm_memory_grow(0, pages) != (size_t)-1) {
        tlsf_add_pool(g_tlsf, (void*)cur, pages * 65536);
    }
}

EXPORT(malloc)
void* malloc(size_t size) {
    void* p = tlsf_malloc(g_tlsf, size);
    if (!p) {
        grow_heap(size);
        p = tlsf_malloc(g_tlsf, size);
    }
    return p;
}

EXPORT(free)
void free(void* ptr) {
    if (ptr) tlsf_free(g_tlsf, ptr);
}

void* realloc(void* ptr, size_t size) {
    void* p = tlsf_realloc(g_tlsf, ptr, size);
    if (!p && size) {
        grow_heap(size);
        p = tlsf_realloc(g_tlsf, ptr, size);
    }
    return p;
}

void* calloc(size_t count, size_t size) {
    size_t total = count * size;
    void* p = malloc(total);
    if (p) memset(p, 0, total);
    return p;
}

/*
 * __heap_base is provided by wasm-ld, marks the start of free memory.
 * __builtin_wasm_memory_size gives total WASM pages (64KB each).
 */
EXPORT(hlp_init)
void hlp_init(void) {
    extern unsigned char __heap_base;
    unsigned char* heap_start = &__heap_base;
    size_t total_mem = __builtin_wasm_memory_size(0) * 65536;
    size_t heap_size = total_mem - (size_t)heap_start;
    g_tlsf = tlsf_create_with_pool(heap_start, heap_size);
}
