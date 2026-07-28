/* The three lines every program here repeats, in one place.
 *
 * Not a libc. There is no allocator, no buffering and no errno — a program
 * gets the kernel's return value, negative for an error, which is what the
 * kernel actually returns and what every wrapper in this directory was
 * translating identically. */
#ifndef VERK_PROG_SYSCALLS_H
#define VERK_PROG_SYSCALLS_H

static long syscall3(long n, long a, long b, long c) {
    register long x8 __asm__("x8") = n;
    register long x0 __asm__("x0") = a;
    register long x1 __asm__("x1") = b;
    register long x2 __asm__("x2") = c;
    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2) : "memory");
    return x0;
}
__attribute__((unused)) static long write_(const char *s, unsigned long n) { return syscall3(64, 1, (long)s, (long)n); }
__attribute__((unused)) static void exit_(int c) { syscall3(93, c, 0, 0); __builtin_unreachable(); }

__attribute__((unused)) static unsigned long slen(const char *s) { unsigned long n = 0; while (s[n]) n++; return n; }
__attribute__((unused)) static void say(const char *s) { write_(s, slen(s)); }
__attribute__((unused)) static void num(long v) {
    char b[24]; int i = 23; b[i--] = 0;
    if (v < 0) { say("-"); v = -v; }
    if (!v) b[i--] = '0';
    while (v) { b[i--] = '0' + (char)(v % 10); v /= 10; }
    say(&b[i + 1]);
}
/* The kernel starts a program the way every aarch64 Linux one is started: the
 * stack pointer holds argc, then argv, then a NULL, then the environment. There
 * is no libc here to unpack that, so _start hands the stack pointer to a C
 * function and never comes back. */
#define VERK_START(fn) \
    __asm__(".global _start\n_start:\n  mov x0, sp\n  b " #fn "\n"); \
    void fn(unsigned long *sp)

#endif
