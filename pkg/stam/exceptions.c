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

static const char *const kind_name[16] = {
    "synchronous (EL1t)", "IRQ (EL1t)", "FIQ (EL1t)", "SError (EL1t)",
    "synchronous (kernel)", "IRQ (kernel)", "FIQ (kernel)", "SError (kernel)",
    "synchronous (user)", "IRQ (user)", "FIQ (user)", "SError (user)",
    "synchronous (32-bit user)", "IRQ (32-bit user)", "FIQ (32-bit user)",
    "SError (32-bit user)",
};

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
    kprintf("  kind      %s\n", kind_name[kind & 15]);
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

/* Until the interrupt controller is wired up nothing can raise an IRQ, but
 * the vector table already routes them here. An empty handler now means the
 * table is complete and testable; the timer will fill this in. */
void irq_dispatch(void) { }
