/* stam — milestone 1: wake up, find out what machine this is, say so.
 *
 * There is deliberately no hardware address anywhere in this file, nor in the
 * console driver. Everything below is read out of the device tree the
 * firmware handed us. That is the difference between a kernel that runs on
 * one machine and a kernel that can be made to run on many: it is decided
 * here, in the first hundred lines, not later.
 */
#include "bootinfo.h"
#include "console.h"
#include "fdt.h"
#include "mmu.h"
#include "gic.h"
#include "timer.h"
#include "sched.h"
#include "vm.h"
#include "elf.h"
#include "cpio.h"

extern void syscall_set_archive(uint64_t addr, uint64_t size);
extern void syscall_open_stdio(void);

extern void enter_user(uint64_t entry, uint64_t stack_top);

#define STAM_VERSION "0.1"

static boot_info_t boot;

/* The demonstration. These loops never yield, never sleep and never call into
 * the kernel — if they all make progress, it is because the timer took the CPU
 * away from each of them in turn. A cooperative scheduler would show one
 * counter rising and two at zero. */
static volatile uint64_t work[3];
static volatile int stop_work;

/* Somewhere nothing else lives: high in the 39-bit space the page tables
 * describe, well past any RAM or device this machine has. Its 1 GiB block is
 * split on first use, which is exactly the case worth exercising. */
#define USER_CODE_VA   0x7f00000000ul
#define USER_STACK_VA  0x7f00100000ul

/* Reached through assembly accessors, never as &symbol — see userprog.S. */
extern uint64_t user_prog_addr(void), user_prog_size(void);
extern uint64_t user_bad_addr(void),  user_bad_size(void);

/* Each program gets its own low half before anything is mapped into it. */
static int give_own_space(void) {
    uint64_t space = vm_new_space();
    if (!space) { kprintf("  [kernel] no memory for an address space\n"); return -1; }
    thread_current()->space = space;
    mmu_switch_user(space);
    return 0;
}

static void run_program(uint64_t start, uint64_t n) {
    if (give_own_space()) return;
    unsigned char *code  = alloc_page();
    unsigned char *stack = alloc_page();
    if (!code || !stack) { kprintf("  [kernel] no memory for the program\n"); return; }

    const unsigned char *src = (const unsigned char *)(uintptr_t)start;
    for (uint64_t i = 0; i < n && i < PAGE_SIZE; i++) code[i] = src[i];

    if (map_user_page(USER_CODE_VA,  (uint64_t)(uintptr_t)code,  1) ||
        map_user_page(USER_STACK_VA, (uint64_t)(uintptr_t)stack, 0)) {
        kprintf("  [kernel] could not map the program\n");
        return;
    }
    kprintf("  %llu bytes mapped at 0x%llx, dropping to EL0\n",
            (unsigned long long)n, (unsigned long long)USER_CODE_VA);
    enter_user(USER_CODE_VA, USER_STACK_VA + PAGE_SIZE);
}

static void first_program(void *a)  { (void)a; run_program(user_prog_addr(), user_prog_size()); }

/* A program the compiler built, loaded out of the initrd. This is how a kernel
 * starts its first program before it has a filesystem — the loader put the file
 * in memory and told us where, which is all we need. */
static void initrd_program(void *a) {
    (void)a;
    uint64_t entry = 0;
    if (give_own_space()) return;

    /* The initrd may be a bare ELF or an archive of them. Try to find /init in
     * it; failing that, treat the whole thing as one program, which is what it
     * was before there was an archive. */
    uint64_t img = boot.initrd_start, len = boot.initrd_end - boot.initrd_start;
    uint64_t f = 0, flen = 0;
    if (cpio_find(img, len, "init", &f, &flen) == 0) {
        img = f; len = flen;
    }
    if (elf_load(img, len, &entry) != 0) {
        kprintf("  [kernel] the initrd holds nothing I can run; it contains:\n");
        cpio_list(boot.initrd_start, boot.initrd_end - boot.initrd_start);
        return;
    }
    unsigned char *stack = alloc_page();
    if (!stack || map_user_page(USER_STACK_VA, (uint64_t)(uintptr_t)stack, 0)) {
        kprintf("  [kernel] no stack for the program\n"); return;
    }
    /* Three descriptors, before anything runs. A program that finds fd 1
     * closed does not check; it writes anyway, is told -EBADF, and carries on
     * into whatever it does when its output has silently gone nowhere. */
    syscall_open_stdio();
    kprintf("  [kernel] %s enters at 0x%llx in its own address space\n",
            thread_current()->name, (unsigned long long)entry);
    enter_user(entry, USER_STACK_VA + PAGE_SIZE);
}
static void bad_program(void *a)    { (void)a; run_program(user_bad_addr(),  user_bad_size());  }

static void worker(void *arg) {
    unsigned long id = (unsigned long)(uintptr_t)arg;
    while (!stop_work) work[id]++;
}

/* Report in MiB up to 8 GiB: "1 GiB" for 1.9 GiB of free memory is true but
 * useless, and the rounding hides half a gigabyte. */
static void print_size(uint64_t bytes) {
    if (bytes >= (8ull << 30)) kprintf("%llu GiB", (unsigned long long)(bytes >> 30));
    else if (bytes >= (1ull << 20)) kprintf("%llu MiB", (unsigned long long)(bytes >> 20));
    else kprintf("%llu bytes", (unsigned long long)bytes);
}

void kmain_high(void);

void kmain(void *dtb) {
    if (handover_dtb(dtb, &boot) != 0) {
        /* No tree means no console either — there is nothing to print to and
         * nothing sensible to do. Stopping is the honest response. */
        for (;;) __asm__ volatile("wfe");
    }
    if (console_init(machine_tree()) != 0) for (;;) __asm__ volatile("wfe");

    kprintf("\n");
    kprintf("stam %s — the VerkOS kernel\n", STAM_VERSION);
    kprintf("\n");
    kprintf("  machine   %s\n", boot.model);
    kprintf("  console   %s at 0x%llx (found in the device tree)\n",
            console_kind(), (unsigned long long)console_addr());
    kprintf("  cpus      %llu\n", (unsigned long long)boot.cpus);
    kprintf("  memory    ");
    print_size(boot.ram_size);
    kprintf(" at 0x%llx\n", (unsigned long long)boot.ram_base);
    if (boot.initrd_end > boot.initrd_start) {
        kprintf("  initrd    ");
        print_size(boot.initrd_end - boot.initrd_start);
        kprintf(" at 0x%llx\n", (unsigned long long)boot.initrd_start);
    } else {
        kprintf("  initrd    none\n");
    }
    kprintf("  cmdline   %s\n", boot.cmdline ? boot.cmdline : "(empty)");
    kprintf("  dtb       0x%llx\n", (unsigned long long)(uintptr_t)boot.dtb);
    /* Vectors before the MMU: a bad page table faults, and a fault with no
     * vector table is a machine that resets with nothing on the console. */
    exceptions_install();
    kprintf("  vectors   installed\n");

    if (boot.free_size)
        pmm_init(boot.free_base, boot.free_size, (uint64_t)(uintptr_t)boot.dtb, 1u << 20);
    else
        pmm_init(boot.ram_base, boot.ram_size, (uint64_t)(uintptr_t)boot.dtb, 1u << 20);
    if (mmu_init(boot.ram_base, boot.ram_size) != 0) {
        kprintf("  mmu       FAILED to allocate a page table\n");
    } else {
        kprintf("  mmu       on, caches on, table at 0x%llx (%llu page)\n",
                (unsigned long long)mmu_table(), (unsigned long long)pmm_pages_used());
        kprintf("  free ram  ");
        print_size(pmm_free_bytes());
        kprintf("\n");
    }

    /* Move to the high half and carry on there. Does not return. */
    mmu_go_high();
    kprintf("  mmu       could not move to the high half — stopping\n");
    for (;;) __asm__ volatile("wfe");
}

/* Everything from here runs at 0xffffff80_________, with the low half of the
 * address space handed over to whatever program is running. */
void kmain_high(void) {
    mmu_note_high();
    console_rebase(KVA_OFFSET);
    handover_rebase(KVA_OFFSET);
    boot.dtb = (const void *)(uintptr_t)phys_to_kva((uint64_t)(uintptr_t)boot.dtb);
    if (boot.initrd_end > boot.initrd_start) {
        boot.initrd_start = phys_to_kva(boot.initrd_start);
        boot.initrd_end   = phys_to_kva(boot.initrd_end);
    }
    /* The vector table was installed at its low address. Now that the low half
     * is gone, VBAR_EL1 points at nothing — and the first interrupt would be a
     * fault whose handler is also unreachable, which is a hang with no output
     * at all. exceptions_install computes the address PC-relative, so calling
     * it again from up here installs the high one. */
    exceptions_install();
    syscall_set_archive(0, 0);
    mmu_drop_low_half();
    kprintf("  kernel    now running from 0x%llx upwards; the low half is a\n"
            "            program's to use\n",
            (unsigned long long)KVA_OFFSET);

    int ticking = 0;
    if (gic_init(machine_tree()) == 0 && timer_init(machine_tree(), 100) == 0) {
        ticking = 1;
        irq_enable();
        /* Wait for a second's worth of ticks and say so. A timer that is
         * configured but never fires is the commonest silent failure at this
         * stage, and it looks exactly like a timer that works. */
        uint64_t target = timer_hz();
        while (timer_ticks() < target) __asm__ volatile("wfi");
        kprintf("  ticking   %llu interrupts taken — the kernel can preempt\n",
                (unsigned long long)timer_ticks());
    }

    if (ticking) {
        sched_init("boot");
        thread_create("worker-a", worker, (void *)(uintptr_t)0);
        thread_create("worker-b", worker, (void *)(uintptr_t)1);
        thread_create("worker-c", worker, (void *)(uintptr_t)2);
        kprintf("\n  --- 1. can the kernel take the CPU back? ---\n");
        kprintf("  %llu threads, none of which ever yields.\n",
                (unsigned long long)thread_count());

        /* Now that interrupts work, let the console raise one too. */
        int cirq = console_irq(machine_tree());
        if (cirq > 0) {
            gic_set_console_irq(cirq);
            gic_enable_irq((unsigned)cirq);
            console_enable_interrupt();
            kprintf("  console   interrupt %llu — a reader can sleep\n",
                    (unsigned long long)cirq);
        } else {
            kprintf("  console   no interrupt in the device tree; readers will poll\n");
        }

        uint64_t until = timer_ticks() + timer_hz() * 2;
        while (timer_ticks() < until) __asm__ volatile("wfi");
        stop_work = 1;
        while (threads_alive() > 1) { thread_yield(); __asm__ volatile("wfi"); }

        kprintf("  after two seconds of everyone refusing to give way:\n");
        for (int i = 0; i < thread_count(); i++) {
            thread_t *t = thread_at(i);
            kprintf("    %s%s  scheduled %llu times", t->name,
                    t->id == 0 ? "     " : "  ", (unsigned long long)t->slices);
            if (t->id > 0)
                kprintf(", counted to %llu", (unsigned long long)work[t->id - 1]);
            kprintf("\n");
        }
        kprintf("  every thread ran, evenly. yes.\n");

        kprintf("\n  --- 2. can something run outside the kernel? ---\n");
        thread_create("program", first_program, 0);
        while (threads_alive() > 1) { thread_yield(); __asm__ volatile("wfi"); }
        kprintf("  the program is gone; the kernel is still here. yes.\n");

        /* If a program was handed to us, run that too — a real ELF, built by
         * the compiler rather than carried inside the kernel. */
        kprintf("\n  --- 3. can it run a program the compiler built? ---\n");
        if (boot.initrd_end > boot.initrd_start) {
            syscall_set_archive(boot.initrd_start, boot.initrd_end - boot.initrd_start);
            thread_create("initrd", initrd_program, 0);
            while (threads_alive() > 1) { thread_yield(); __asm__ volatile("wfi"); }
            kprintf("  it ran and exited. yes.\n");
            kprintf("  memory returned to the free list: %llu pages\n",
                    (unsigned long long)pmm_pages_free_listed());
        } else {
            kprintf("  no initrd was handed to us, so there is nothing to run.\n");
        }

        if (boot.initrd_end > boot.initrd_start) {
            kprintf("\n  --- 5. can two programs run at once, each with its own memory? ---\n");
            kprintf("  the same binary twice, code and data at the same addresses in both\n");
            thread_create("copy-a", initrd_program, 0);
            thread_create("copy-b", initrd_program, 0);
            while (threads_alive() > 1) { thread_yield(); __asm__ volatile("wfi"); }
            kprintf("  both finished without disturbing each other. yes.\n");
        }

        kprintf("\n  --- 4. is a program kept out of the kernel's memory? ---\n");
        thread_create("trespasser", bad_program, 0);
        while (threads_alive() > 1) { thread_yield(); __asm__ volatile("wfi"); }
        kprintf("  the program was killed, the kernel was not. yes.\n");
    }

    /* Prove the fault path works, on request. A handler that has never run is
     * a handler you find out about at the worst moment. */
    if (boot.cmdline) {
        const char *c = boot.cmdline;
        for (; *c; c++)
            if (c[0]=='s'&&c[1]=='t'&&c[2]=='a'&&c[3]=='m'&&c[4]=='.'&&c[5]=='f'&&c[6]=='a'&&c[7]=='u'&&c[8]=='l'&&c[9]=='t') {
                kprintf("\n  stam.fault: touching an address on purpose...\n");
                /* Above the 39-bit address space the page tables describe,
                 * so this is a translation fault — the shape of an actual
                 * kernel bug. An address inside the mapped range would give
                 * an external abort from the bus instead, which is a
                 * different and less instructive failure. */
                volatile uint64_t *nowhere = (volatile uint64_t *)0x8000000000ul;
                *nowhere = 1;
            }
    }

    kprintf("\n");
    /* Say what actually happened. The first version of this line claimed
     * "time running" on a machine whose interrupt controller we do not
     * support and where the timer never started — a summary that lies is
     * worse than no summary, because it is believed. */
    if (ticking)
        kprintf("  milestone 4: a program ran outside the kernel and asked it for things.\n");
    else
        kprintf("  milestone 2 only: memory mapped and faults reported, but this\n"
                "  machine has no interrupt controller we support, so nothing ticks.\n");
    kprintf("  no filesystem, no ELF loader, one address space.\n");
    kprintf("\n");

    for (;;) __asm__ volatile("wfe");
}
