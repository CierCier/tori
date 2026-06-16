#include <stdlib.h>
#include <unistd.h>
#include <stdint.h>

// Simple bump-allocator malloc backed by brk().
// free() is a no-op for now.

void* malloc(size_t size) {
    if (size == 0) return NULL;

    // Align to 16 bytes
    size = (size + 15) & ~15;

    void* old = brk((void*)0);
    if (old == (void*)-1) return NULL;

    // Reserve space
    void* new_brk = brk((void*)((uintptr_t)old + size));
    if (new_brk == (void*)-1) return NULL;

    // If brk returned less than we expected, allocation failed
    if ((uintptr_t)new_brk < (uintptr_t)old + size) return NULL;

    return old;
}

void free(void* ptr) {
    (void)ptr;
}

void* calloc(size_t nmemb, size_t size) {
    size_t total = nmemb * size;
    void* p = malloc(total);
    if (p) {
        unsigned char* cp = (unsigned char*)p;
        for (size_t i = 0; i < total; ++i) cp[i] = 0;
    }
    return p;
}

void* realloc(void* ptr, size_t new_size) {
    (void)ptr;
    return malloc(new_size);
}
