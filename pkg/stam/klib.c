/* stam — the handful of C library functions a freestanding kernel still needs.
 *
 * These carry their standard names on purpose. GCC emits calls to memcpy,
 * memset and memmove on its own, from struct assignments and array
 * initialisers, even with -ffreestanding and even where the source contains no
 * such call. Naming them anything else produces link errors that point at code
 * which looks innocent.
 */
#include <stddef.h>
#include <stdint.h>

void *memset(void *d, int c, size_t n) {
    uint8_t *p = d;
    while (n--) *p++ = (uint8_t)c;
    return d;
}

void *memcpy(void *d, const void *s, size_t n) {
    uint8_t *p = d; const uint8_t *q = s;
    while (n--) *p++ = *q++;
    return d;
}

void *memmove(void *d, const void *s, size_t n) {
    uint8_t *p = d; const uint8_t *q = s;
    if (p == q || n == 0) return d;
    if (p < q) { while (n--) *p++ = *q++; }
    else { p += n; q += n; while (n--) *--p = *--q; }
    return d;
}

int memcmp(const void *a, const void *b, size_t n) {
    const uint8_t *p = a, *q = b;
    for (; n--; p++, q++) if (*p != *q) return *p - *q;
    return 0;
}

size_t strlen(const char *s) { const char *p = s; while (*p) p++; return (size_t)(p - s); }

int strcmp(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return (uint8_t)*a - (uint8_t)*b;
}

int strncmp(const char *a, const char *b, size_t n) {
    for (; n; n--, a++, b++) {
        if (*a != *b) return (uint8_t)*a - (uint8_t)*b;
        if (!*a) break;
    }
    return 0;
}
