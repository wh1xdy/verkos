/* stam — the console.
 *
 * Three serial drivers, chosen by what the device tree says is actually there.
 * More than one is what forces the code to be honest: with a single driver you
 * can pretend the register layout is universal, and every board that disagrees
 * costs a rewrite. PL011 is what QEMU's virt machine and most ARM designs use;
 * 8250/16550 is what nearly everything else does, including the x86 world; and
 * GENI is Qualcomm's, which is neither, and is the console on the Arduino
 * UNO Q and on a great many phones and boards besides.
 *
 * UNVERIFIED: the GENI path has never run. It is written from the register
 * sequence Qualcomm's hardware requires, but no Qualcomm hardware has been
 * asked to execute it. Everything else in this file has been run on a machine.
 */
#include "console.h"
#include "fdt.h"

extern size_t strlen(const char *);

/* PL011 */
#define PL011_DR   0x00
#define PL011_FR   0x18
#define PL011_FR_TXFF (1u << 5)
#define PL011_FR_RXFE (1u << 4)     /* receive FIFO empty */
#define PL011_FR_BUSY (1u << 3)     /* still shifting a character out       */
#define PL011_LCRH 0x2c             /* line control: word length, and FEN   */
#define PL011_LCRH_FEN (1u << 4)    /* the receive FIFO exists when this is set */
#define PL011_CR   0x30
#define PL011_CR_EN (1u << 0)       /* UARTEN: must be off to change LCR_H  */
#define PL011_IFLS 0x34             /* FIFO level that raises an interrupt */
#define PL011_IMSC 0x38             /* which interrupts are wanted         */
#define PL011_MIS  0x40             /* which are asserted, after masking   */
#define PL011_ICR  0x44             /* acknowledge                         */
#define PL011_INT_RX  (1u << 4)
#define PL011_INT_RT  (1u << 6)     /* receive timeout: the last few bytes */

/* 8250/16550 */
#define UART_THR   0x00
#define UART_RBR   0x00
#define UART_LSR   0x05
#define UART_LSR_THRE (1u << 5)
#define UART_LSR_DR   (1u << 0)     /* data ready */
#define UART_IER   0x01
#define UART_IER_RX (1u << 0)

/* Qualcomm GENI. Not a UART with registers so much as a programmable serial
 * engine told to behave like one: every character is a transfer that must be
 * announced, waited for, and acknowledged. */
#define GENI_M_CMD0        0x600
#define GENI_M_IRQ_STATUS  0x610
#define GENI_M_IRQ_CLEAR   0x618
#define GENI_TX_FIFO       0x700
#define GENI_TX_TRANS_LEN  0x270
#define GENI_M_CMD_DONE    (1u << 0)
#define GENI_M_TX_WATERMARK (1u << 30)
#define GENI_UART_START_TX (1u << 27)     /* opcode 1, shifted into place */

static volatile uint8_t *uart_base;
static int console_node = FDT_NONE;   /* the tree node it came from */
static enum { UART_NONE, UART_PL011, UART_8250, UART_GENI } uart_kind;
static uint32_t uart_shift;      /* reg-shift: registers may be spaced out  */
static uint32_t uart_width;      /* reg-io-width: 1, 2 or 4 bytes per access */

static uint32_t reg_read(uint32_t r) {
    volatile void *p = uart_base + ((size_t)r << uart_shift);
    if (uart_width == 4) return *(volatile uint32_t *)p;
    if (uart_width == 2) return *(volatile uint16_t *)p;
    return *(volatile uint8_t *)p;
}

static void reg_write(uint32_t r, uint32_t v) {
    volatile void *p = uart_base + ((size_t)r << uart_shift);
    if (uart_width == 4)      *(volatile uint32_t *)p = v;
    else if (uart_width == 2) *(volatile uint16_t *)p = (uint16_t)v;
    else                      *(volatile uint8_t *)p  = (uint8_t)v;
}

/* A bounded wait. On hardware that is not what we think it is, an unbounded
 * one is a hang with no output — the worst failure this kernel can produce,
 * and precisely what an untested driver invites. Giving up prints nothing but
 * lets the machine keep going, so the next thing that goes wrong is visible. */
static int wait_for(unsigned off, uint32_t mask, int want_set) {
    for (int spin = 0; spin < 1000000; spin++) {
        uint32_t v = reg_read(off) & mask;
        if (want_set ? (v != 0) : (v == 0)) return 1;
    }
    return 0;
}

void kputc(char c) {
    if (uart_kind == UART_NONE) return;
    if (c == '\n') kputc('\r');
    if (uart_kind == UART_PL011) {
        if (!wait_for(PL011_FR, PL011_FR_TXFF, 0)) return;
        reg_write(PL011_DR, (uint8_t)c);
    } else if (uart_kind == UART_8250) {
        if (!wait_for(UART_LSR, UART_LSR_THRE, 1)) return;
        reg_write(UART_THR, (uint8_t)c);
    } else {
        /* GENI: announce a one-byte transfer, start it, wait to be invited to
         * fill the FIFO, write the byte, then wait for the engine to say the
         * command finished. Each acknowledgement is required; skipping one
         * leaves the engine busy and the next character never goes out. */
        reg_write(GENI_TX_TRANS_LEN, 1);
        reg_write(GENI_M_CMD0, GENI_UART_START_TX);
        if (!wait_for(GENI_M_IRQ_STATUS, GENI_M_TX_WATERMARK, 1)) return;
        reg_write(GENI_TX_FIFO, (uint8_t)c);
        reg_write(GENI_M_IRQ_CLEAR, GENI_M_TX_WATERMARK);
        if (!wait_for(GENI_M_IRQ_STATUS, GENI_M_CMD_DONE, 1)) return;
        reg_write(GENI_M_IRQ_CLEAR, GENI_M_CMD_DONE);
    }
}

void kputs(const char *s) { while (*s) kputc(*s++); }

/* A character if one is waiting, or -1. Deliberately not a blocking read: the
 * kernel has no business deciding how long a program should wait, and the one
 * caller that does want to wait can yield between attempts.
 *
 * There is no interrupt behind this. A UART interrupt would let a reading
 * thread genuinely sleep instead of being scheduled only to find nothing; that
 * is the next thing this file needs, and until it exists a program waiting for
 * input costs a time slice each time round. Saying so is better than a comment
 * claiming it blocks. */
/* Once a person is sitting at the console, the kernel's running commentary is
 * noise between their prompt and their answer. It stays available — a program
 * that will not load is exactly when the loader's reasoning is wanted — but it
 * is off unless asked for. */
static int chatty = 1;
void kquiet(int on) { chatty = !on; }
int  kchatty(void)  { return chatty; }

/* Characters the interrupt has taken off the port but nobody has asked for
 * yet. Small on purpose: it holds a typed line, not a paste of a file, and
 * anything larger would be pretending to be a buffer cache. */
#define RING 256
static volatile char ring[RING];
static volatile unsigned ring_head, ring_tail;
static int interrupt_driven;

static int ring_get(void) {
    if (ring_head == ring_tail) return -1;
    int c = ring[ring_tail];
    ring_tail = (ring_tail + 1) % RING;
    return c;
}

static void ring_put(int c) {
    unsigned next = (ring_head + 1) % RING;
    if (next == ring_tail) return;       /* full: drop, rather than overwrite */
    ring[ring_head] = (char)c;
    ring_head = next;
}

/* Read straight from the port, with no buffering. Used by the interrupt to
 * drain it, and as the whole implementation when there is no interrupt. */
static int hw_getc(void) {
    switch (uart_kind) {
    case UART_PL011:
        if (reg_read(PL011_FR) & PL011_FR_RXFE) return -1;
        return (int)(reg_read(PL011_DR) & 0xff);
    case UART_8250:
        if (!(reg_read(UART_LSR) & UART_LSR_DR)) return -1;
        return (int)(reg_read(UART_RBR) & 0xff);
    default:
        return -1;          /* GENI receive is not written yet */
    }
}

/* Called from the interrupt. Drains the port completely: the interrupt says
 * that at least one character arrived, not how many, and leaving any behind
 * means the next one may never raise another. */
void console_interrupt(void) {
    /* Acknowledge *first*, then drain.
     *
     * The other order looks more natural and loses characters. Clearing after
     * the drain clears the status of anything that arrived while we were
     * draining: the character is sitting in the port, no interrupt is pending
     * for it, and the reader is asleep waiting for an interrupt that will never
     * come. Whatever arrives next wakes it and the earlier bytes appear late
     * and out of order — or, if nothing else is typed, never.
     *
     * Acknowledging first can only cost a spurious interrupt with nothing to
     * read, which the drain loop handles by finding the port empty. */
    if (uart_kind == UART_PL011)
        reg_write(PL011_ICR, PL011_INT_RX | PL011_INT_RT);
    int c;
    while ((c = hw_getc()) >= 0) ring_put(c);
}

int kgetc(void) {
    if (interrupt_driven) return ring_get();
    return hw_getc();
}

int console_is_interrupt_driven(void) { return interrupt_driven; }

/* Ask the port to raise an interrupt when something arrives. Until this is
 * called a reader has to poll, which works and wastes a time slice each time
 * round; afterwards it can sleep. */
void console_enable_interrupt(void) {
    if (uart_kind == UART_PL011) {
        /* Turn the receive FIFO on. Without FEN the port holds exactly one
         * character, so anything arriving faster than the reader drains it is
         * overwritten in the hardware and never seen — which is what a pasted
         * line is. IFLS was already being set to "raise at 1/8 full" and meant
         * nothing, because there was no FIFO to be an eighth of.
         *
         * LCR_H may only be changed with the UART disabled, and the port must
         * finish what it is transmitting first, or the character in flight is
         * mangled. Real hardware enforces this; QEMU does not, which is exactly
         * the sort of difference that is discovered on a board and not here. */
        uint32_t cr = reg_read(PL011_CR);
        reg_write(PL011_CR, cr & ~PL011_CR_EN);
        wait_for(PL011_FR, PL011_FR_BUSY, 0);
        reg_write(PL011_LCRH, reg_read(PL011_LCRH) | PL011_LCRH_FEN);
        reg_write(PL011_CR, cr);

        reg_write(PL011_IFLS, 0);                          /* raise at 1/8 full */
        reg_write(PL011_ICR, 0x7ff);                       /* start clean       */
        reg_write(PL011_IMSC, PL011_INT_RX | PL011_INT_RT);
        interrupt_driven = 1;
    } else if (uart_kind == UART_8250) {
        reg_write(UART_IER, UART_IER_RX);
        interrupt_driven = 1;
    }
    /* GENI is left polling: its receive path is unwritten, and enabling an
     * interrupt for a driver that cannot read would produce a storm. */
}

static void kputu(uint64_t v, unsigned base, int pad) {
    char buf[32]; int n = 0;
    if (!v) buf[n++] = '0';
    while (v) { unsigned d = (unsigned)(v % base); buf[n++] = (char)(d < 10 ? '0'+d : 'a'+d-10); v /= base; }
    while (n < pad) buf[n++] = '0';
    while (n--) kputc(buf[n]);
}

/* Enough of printf to describe a machine: %s %c %d %u %x %p %%, with an
 * optional zero-pad width for %x. Anything more belongs in userspace. */
void kprintf(const char *fmt, ...) {
    __builtin_va_list ap;
    __builtin_va_start(ap, fmt);
    for (; *fmt; fmt++) {
        if (*fmt != '%') { kputc(*fmt); continue; }
        fmt++;
        int pad = 0;
        while (*fmt >= '0' && *fmt <= '9') { pad = pad * 10 + (*fmt - '0'); fmt++; }
        if (*fmt == 'l') { fmt++; if (*fmt == 'l') fmt++; }
        switch (*fmt) {
        case 's': { const char *s = __builtin_va_arg(ap, const char *); kputs(s ? s : "(null)"); break; }
        case 'c':   kputc((char)__builtin_va_arg(ap, int)); break;
        case 'd': { long long v = __builtin_va_arg(ap, long long);
                    if (v < 0) { kputc('-'); v = -v; } kputu((uint64_t)v, 10, pad); break; }
        case 'u':   kputu(__builtin_va_arg(ap, unsigned long long), 10, pad); break;
        case 'x':   kputu(__builtin_va_arg(ap, unsigned long long), 16, pad); break;
        case 'p':   kputs("0x"); kputu((uint64_t)(uintptr_t)__builtin_va_arg(ap, void *), 16, 16); break;
        case '%':   kputc('%'); break;
        default:    kputc('%'); kputc(*fmt); break;
        }
    }
    __builtin_va_end(ap);
}

/* Find the console the firmware told us to use, and fall back to looking for
 * a serial port ourselves. Deliberately no hardcoded address anywhere. */
int console_init(const fdt_t *f) {
    int node = FDT_NONE;
    uint32_t len = 0;

    /* /chosen/stdout-path names the console the firmware has been using; it
     * may carry a baud suffix ("/pl011@9000000:115200n8") we cut off. */
    int chosen = fdt_find_path(f, "/chosen");
    if (chosen != FDT_NONE) {
        const char *sp = fdt_getprop(f, chosen, "stdout-path", &len);
        if (sp && len) {
            char path[128]; uint32_t i = 0;
            for (; i < len - 1 && i < sizeof path - 1 && sp[i] && sp[i] != ':'; i++) path[i] = sp[i];
            path[i] = 0;
            if (path[0] == '/') node = fdt_find_path(f, path);
        }
    }
    if (node == FDT_NONE) node = fdt_find_compatible(f, "arm,pl011");
    if (node == FDT_NONE) node = fdt_find_compatible(f, "ns16550a");
    if (node == FDT_NONE) node = fdt_find_compatible(f, "ns16550");
    if (node == FDT_NONE) node = fdt_find_compatible(f, "qcom,geni-debug-uart");
    if (node == FDT_NONE) node = fdt_find_compatible(f, "qcom,geni-uart");
    if (node == FDT_NONE) return -1;

    uint64_t addr = 0, size = 0;
    if (fdt_reg(f, node, &addr, &size) != 0) return -1;

    uart_shift = 0; uart_width = 1;
    const void *p = fdt_getprop(f, node, "reg-shift", &len);
    if (p && len == 4) uart_shift = (uint32_t)fdt_cells(p, 1);
    p = fdt_getprop(f, node, "reg-io-width", &len);
    if (p && len == 4) uart_width = (uint32_t)fdt_cells(p, 1);

    console_node = node;
    uart_base = (volatile uint8_t *)(uintptr_t)addr;
    if (fdt_is_compatible(f, node, "arm,pl011")) {
        uart_kind = UART_PL011; uart_shift = 0; uart_width = 4;
    } else if (fdt_is_compatible(f, node, "qcom,geni-debug-uart") ||
               fdt_is_compatible(f, node, "qcom,geni-uart")) {
        uart_kind = UART_GENI;  uart_shift = 0; uart_width = 4;
    } else {
        uart_kind = UART_8250;
    }

    /* The firmware has already configured the port it was printing on, so we
     * do not touch the baud rate. Reprogramming it is how you lose the output
     * that would have told you what went wrong. */
    return 0;
}

/* The UART's address came out of the device tree, so it is physical. Once the
 * kernel runs from the high half it must reach the same registers through its
 * own window onto that memory. */
void console_rebase(uint64_t offset) {
    if (uart_kind != UART_NONE) uart_base += offset;
}

/* The console's interrupt, out of the same node its address came from. A
 * device tree writes an SPI as type 0 and numbers it from the start of the
 * shared range, which begins at 32; a PPI is type 1 and starts at 16. Both
 * conventions are relative, and both are easy to get wrong in the direction
 * that produces a port which works when polled and never interrupts. */
int console_irq(const fdt_t *f) {
    if (console_node == FDT_NONE) return -1;
    uint32_t len = 0;
    const void *p = fdt_getprop(f, console_node, "interrupts", &len);
    if (!p || len < 12) return -1;
    uint32_t type = (uint32_t)fdt_cells(p, 1);
    uint32_t num  = (uint32_t)fdt_cells((const uint8_t *)p + 4, 1);
    return (int)(type == 1 ? num + 16 : num + 32);
}

const char *console_kind(void) {
    switch (uart_kind) {
    case UART_PL011: return "PL011";
    case UART_8250:  return "8250";
    case UART_GENI:  return "GENI (untested)";
    default:         return "none";
    }
}

uint64_t console_addr(void) { return (uint64_t)(uintptr_t)uart_base; }
