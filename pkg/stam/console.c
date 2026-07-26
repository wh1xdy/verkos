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

/* 8250/16550 */
#define UART_THR   0x00
#define UART_LSR   0x05
#define UART_LSR_THRE (1u << 5)

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

const char *console_kind(void) {
    switch (uart_kind) {
    case UART_PL011: return "PL011";
    case UART_8250:  return "8250";
    case UART_GENI:  return "GENI (untested)";
    default:         return "none";
    }
}

uint64_t console_addr(void) { return (uint64_t)(uintptr_t)uart_base; }
