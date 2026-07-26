/* stam — what to say when something goes wrong.
 *
 * The whole value of this file is that a fault produces a readable report
 * rather than a silent hang. Every hour spent here is repaid the first time a
 * page table is wrong.
 */
#include "console.h"
#include <stdint.h>

/* The register block exceptions.S pushed. */
typedef struct {
    uint64_t x[30];
    uint64_t lr;
    uint64_t elr;      /* where the exception happened     */
    uint64_t spsr;     /* processor state at that moment   */
} regs_t;

/* A switch and not an array of pointers, deliberately. An initialised table of
 * string pointers is data holding link-time addresses, and it needs a runtime
 * relocation to be correct anywhere else. Nothing applies relocations to this
 * kernel when UEFI loads it at whatever address it likes, so such a table reads
 * as garbage — which is how this was found: an exception report that printed an
 * empty description of itself. A switch materialises each address with a
 * PC-relative instruction instead, which is right wherever the code lands. */
static const char *kind_name(unsigned k) {
    switch (k & 15) {
    case 0:  return "synchronous (EL1t)";
    case 1:  return "IRQ (EL1t)";
    case 2:  return "FIQ (EL1t)";
    case 3:  return "SError (EL1t)";
    case 4:  return "synchronous (kernel)";
    case 5:  return "IRQ (kernel)";
    case 6:  return "FIQ (kernel)";
    case 7:  return "SError (kernel)";
    case 8:  return "synchronous (user)";
    case 9:  return "IRQ (user)";
    case 10: return "FIQ (user)";
    case 11: return "SError (user)";
    case 12: return "synchronous (32-bit user)";
    case 13: return "IRQ (32-bit user)";
    case 14: return "FIQ (32-bit user)";
    default: return "SError (32-bit user)";
    }
}

/* ESR_EL1's exception class — the top six bits — says what kind of fault it
 * was. Naming the common ones turns a hex number into a sentence. */
static const char *ec_name(unsigned ec) {
    switch (ec) {
    case 0x15: return "SVC (system call)";
    case 0x18: return "trapped MSR/MRS";
    case 0x20: case 0x21: return "instruction abort (bad fetch)";
    case 0x24: case 0x25: return "data abort (bad load or store)";
    case 0x22: return "misaligned instruction fetch";
    case 0x26: return "stack pointer misaligned";
    case 0x2c: return "floating point";
    case 0x30: case 0x31: return "breakpoint";
    case 0x3c: return "BRK instruction";
    default:   return "unknown";
    }
}

/* The low bits of ESR for an abort say why the translation failed. */
static const char *fault_reason(unsigned iss) {
    switch (iss & 0x3f) {
    case 0x04: case 0x05: case 0x06: case 0x07: return "translation fault (nothing mapped)";
    case 0x09: case 0x0a: case 0x0b: return "access flag fault";
    case 0x0d: case 0x0e: case 0x0f: return "permission fault";
    case 0x10: return "external abort";
    case 0x21: return "alignment fault";
    default:   return "";
    }
}

void exception_fatal(unsigned long kind, regs_t *r) {
    uint64_t esr, far;
    __asm__ volatile("mrs %0, esr_el1" : "=r"(esr));
    __asm__ volatile("mrs %0, far_el1" : "=r"(far));
    unsigned ec = (unsigned)(esr >> 26) & 0x3f;

    kprintf("\n");
    kprintf("  ---- stam: unhandled exception ----\n");
    kprintf("  kind      %s\n", kind_name((unsigned)kind));
    kprintf("  cause     %s (EC 0x%llx)\n", ec_name(ec), (unsigned long long)ec);
    if (ec == 0x24 || ec == 0x25 || ec == 0x20 || ec == 0x21) {
        const char *why = fault_reason((unsigned)esr);
        if (why[0]) kprintf("  reason    %s\n", why);
        kprintf("  address   0x%llx    (what it tried to touch)\n", (unsigned long long)far);
    }
    kprintf("  at        0x%llx    (the instruction)\n", (unsigned long long)r->elr);
    kprintf("  esr       0x%llx\n", (unsigned long long)esr);
    kprintf("  spsr      0x%llx\n", (unsigned long long)r->spsr);
    kprintf("  x0-x3     0x%llx 0x%llx 0x%llx 0x%llx\n",
            (unsigned long long)r->x[0], (unsigned long long)r->x[1],
            (unsigned long long)r->x[2], (unsigned long long)r->x[3]);
    kprintf("  lr        0x%llx\n", (unsigned long long)r->lr);
    kprintf("\n  halted. this is a kernel bug, not a machine fault.\n\n");
    for (;;) __asm__ volatile("wfe");
}
