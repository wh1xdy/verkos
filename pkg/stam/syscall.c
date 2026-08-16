/* stam — what a program may ask the kernel for.
 *
 * Linux's aarch64 numbers, deliberately: 64 is write, 93 is exit, 172 is
 * getpid. Inventing our own would be a decision we could only ever undo, and
 * the numbering is the cheapest part of the ABI to agree with.
 *
 * Seven calls now — read, write, exit, getpid, clone, execve, wait4 — which is
 * exactly the set a shell needs and no more. That is the measure being used:
 * not how many calls exist, but whether an ordinary program can be written
 * against them without the kernel knowing anything about it.
 */
#include "console.h"
#include "sched.h"
#include "file.h"
#include "net.h"
#include "tcp.h"
#include "timer.h"
#include "lock.h"
#include "vm.h"
#include "mmu.h"
#include "cpio.h"
#include "elf.h"
#include "bootinfo.h"
#include "fat.h"
#include <stdint.h>

typedef struct {
    uint64_t x[30];
    uint64_t lr;
    uint64_t elr;
    uint64_t spsr;
    uint64_t sp_el0;      /* saved with the rest: see vectors.S */
} regs_t;

extern void exception_report(unsigned long kind, regs_t *r);

#define SYS_WAIT4  260
#define SYS_EXECVE 221
#define SYS_CLONE  220        /* aarch64 Linux has no fork; clone does both */
#define SYS_READ    63
#define SYS_WRITE   64
#define SYS_EXIT    93
#define SYS_GETPID 172
#define SYS_KILL   129
#define SYS_SETPGID 154
#define SYS_GETPGID 155
#define SYS_IOCTL   29
#define SYS_GETDENTS64 61
#define SYS_MKDIRAT    34
#define SYS_UNLINKAT   35
#define SYS_RT_SIGACTION 134
#define SYS_RT_SIGRETURN 139
#define TIOCGPGRP  0x540F
#define TIOCSPGRP  0x5410
#define SYS_CLOSE   57
#define SYS_PIPE2   59        /* aarch64 Linux has no plain pipe; pipe2 does both */
#define SYS_DUP     23
#define SYS_DUP3    24        /* and no dup2 either; dup3 covers it */
#define SYS_OPENAT  56        /* aarch64 has no open(); openat is the call  */
/* Sockets. Linux's aarch64 numbers, like every other call here — a program
 * built for Linux and run on stam must not need a different table. */
#define SYS_SOCKET    198
#define SYS_BIND      200
#define SYS_SENDTO    206
#define SYS_RECVFROM  207
#define SYS_CONNECT   203

#define AF_INET      2
#define SOCK_DGRAM   2
#define SOCK_STREAM  1
#define O_WRONLY    1
#define O_CREAT     0100
#define O_APPEND    02000
#define WNOHANG     1

#define EFAULT ((uint64_t)-14)

/* A descriptor names an object now, and the object decides what writing means.
 * `write(1, ...)` reaching the console is a fact about this process's table,
 * not about the number 1 — which is exactly what makes `a | b` possible. */
static file_t *fd_get(uint64_t fd) {
    if (fd >= NFD) return 0;
    return thread_current()->fd[fd];
}

static uint64_t sys_write(uint64_t fd, uint64_t buf, uint64_t len) {
    file_t *f = fd_get(fd);
    if (!f) return (uint64_t)-9;                        /* -EBADF */
    /* The buffer address came from a program. Without this the call
     * `write(1, kernel_address, n)` printed kernel memory, and an unmapped one
     * faulted at EL1 — which is not a program being killed but the machine
     * stopping. */
    if (!vm_user_ok(buf, len, 0)) return EFAULT;
    return (uint64_t)file_write(f, (const char *)(uintptr_t)buf, len);
}

/* fork. The frame is passed in because the child needs a copy of it — this is
 * the one system call whose implementation is about the shape of the stack it
 * was called on rather than about its arguments. */
static uint64_t do_fork(void *frame) {
    uint64_t child_space = vm_copy_space(mmu_current_user_table_kva());
    if (!child_space) { kprintf("  [kernel] fork: no memory for a copy\n"); return (uint64_t)-12; }
    thread_t *child = thread_fork(frame, 272, child_space);
    if (!child) { kprintf("  [kernel] fork: no room for another process\n"); return (uint64_t)-11; }
    return (uint64_t)child->id;
}

/* Where the initrd is, so exec has somewhere to look. Set once at boot. */
static uint64_t archive_addr, archive_size;
void syscall_set_archive(uint64_t addr, uint64_t size) {
    archive_addr = addr; archive_size = size;
    file_set_archive(addr, size);
}

typedef struct { uint64_t x[30]; uint64_t lr, elr, spsr, sp_el0; } frame_t;

/* exec: keep the process, replace everything in it.
 *
 * The trick is that this does not jump anywhere. It edits the exception frame
 * the system call will return through — the address to resume at, and the
 * stack — so the ordinary return to EL0 lands in the new program instead of
 * after the svc in the old one. The process id, the kernel stack and the place
 * in the scheduler all stay exactly as they were, which is what makes it the
 * same process running something else.
 *
 * The old address space is not reclaimed. Nothing here frees pages yet; that
 * is a real leak and it is written down rather than hidden.
 */
#define MAX_ARGS  16
#define ARG_BYTES 512

static uint64_t do_execve(const char *path, char *const *argv, void *frameptr) {
    /* No initrd is no longer no programs: there may be a disk. */
    if (!archive_size && !fat_mounted()) return (uint64_t)-2;   /* -ENOENT */

    /* Copy the name out of the caller's memory before touching the address
     * space. The moment the new one is installed the old string is gone, and
     * the message that follows would print whatever now lives at that address
     * — which it did, on the first attempt. */
    /* The path is a pointer from a program, and so is every entry of argv, and
     * so is every string those entries point at. Three separate things to be
     * lied to about. */
    long plen = vm_user_strlen((uint64_t)(uintptr_t)path, 63);
    if (plen < 0) return EFAULT;
    char name[64];
    unsigned i = 0;
    for (; i < (unsigned)plen; i++) name[i] = path[i];
    name[i] = 0;

    /* And the arguments, for exactly the same reason and one more: they have to
     * end up in the *new* address space, so they must survive the moment when
     * neither space is fully in place. Flattened into one kernel buffer here,
     * unpacked onto the new stack below.
     *
     * Bounded, and it refuses rather than truncating. A silently shortened
     * argument list is a program quietly given different instructions, which is
     * this log's recurring defect in the one place it would be hardest to see.
     */
    char  argbuf[ARG_BYTES];
    unsigned arglen[MAX_ARGS];
    int   argc = 0;
    unsigned used = 0;
    if (argv) {
        /* The vector itself, one entry at a time: a program may hand over an
         * array that is mapped for three entries and not for the fourth, and
         * finding that out by faulting is not an option at EL1. */
        for (; argc < MAX_ARGS; argc++) {
            uint64_t slot = (uint64_t)(uintptr_t)&argv[argc];
            if (!vm_user_ok(slot, sizeof(char *), 0)) return EFAULT;
            const char *s = argv[argc];
            if (!s) break;
            long n = vm_user_strlen((uint64_t)(uintptr_t)s, ARG_BYTES);
            if (n < 0) return EFAULT;
            if (used + (unsigned)n + 1 > sizeof argbuf) return (uint64_t)-7; /* -E2BIG */
            for (unsigned k = 0; k <= (unsigned)n; k++) argbuf[used + k] = s[k];
            arglen[argc] = (unsigned)n + 1;
            used += (unsigned)n + 1;
        }
        if (argc == MAX_ARGS) {
            uint64_t slot = (uint64_t)(uintptr_t)&argv[argc];
            if (!vm_user_ok(slot, sizeof(char *), 0)) return EFAULT;
            if (argv[argc]) return (uint64_t)-7;                     /* -E2BIG */
        }
    }
    /* A program is always told its own name, even when the caller passed none.
     * argv[0] being absent is a thing programs do not expect and do not check. */
    if (argc == 0) {
        unsigned n = 0;
        while (name[n]) n++;
        for (unsigned k = 0; k <= n; k++) argbuf[k] = name[k];
        arglen[0] = n + 1;
        used = n + 1;
        argc = 1;
    }

    /* The initrd first, then the disk — the same order file_open uses, and for
     * the same reason: the initrd is what the machine was started with. */
    uint64_t file = 0, len = 0;
    unsigned char *from_disk = 0;
    unsigned disk_pages = 0;
    if (cpio_find(archive_addr, archive_size, name, &file, &len) != 0) {
        fat_file_t ff;
        if (!fat_mounted() || fat_open(name, &ff) != 0 || ff.is_dir || !ff.size) {
            kprintf("  [kernel] exec: no '%s' in the initrd or on the disk\n", name);
            return (uint64_t)-2;
        }
        /* Read whole rather than mapped. Demand paging an executable off a
         * disk needs a page cache and a fault handler that can block, and
         * neither exists yet; reading it in is the honest version of the same
         * thing, and it is a program the loader then treats exactly as if it
         * had come out of the archive. */
        disk_pages = (unsigned)((ff.size + PAGE_SIZE - 1) / PAGE_SIZE);
        from_disk = alloc_pages(disk_pages);
        if (!from_disk) return (uint64_t)-12;                    /* -ENOMEM */
        long got = fat_read(&ff, 0, from_disk, ff.size);
        if (got != (long)ff.size) {
            for (unsigned p = 0; p < disk_pages; p++) free_page(from_disk + p * PAGE_SIZE);
            kprintf("  [kernel] exec: '%s' is %u bytes and only %ld could be read\n",
                    name, ff.size, got);
            return (uint64_t)-5;                                 /* -EIO */
        }
        file = (uint64_t)(uintptr_t)from_disk;
        len  = ff.size;
    }
#define DROP_DISK() do { if (from_disk) \
    for (unsigned p_ = 0; p_ < disk_pages; p_++) free_page(from_disk + p_ * PAGE_SIZE); } while (0)

    uint64_t space = vm_new_space();
    if (!space) { DROP_DISK(); return (uint64_t)-12; }
    uint64_t saved = mmu_current_user_table_kva();
    uint64_t saved_space = thread_current()->space;

    /* The two together, always.
     *
     * mmu_switch_user says which table this processor is filling in;
     * thread->space is what schedule_locked reprograms TTBR0 from when this
     * thread is resumed. Setting the first here and the second nineteen lines
     * later left a window across the whole of elf_load in which a preemption
     * restored the *old* space — so map_user_page wrote into the new table
     * while the hardware translated through the old one, and the loader's own
     * store to a page it had just mapped took a permission fault at a user
     * address it owned.
     *
     * It needed a preemption inside that window, which is why it appeared when
     * the disk driver gained a blocking point in exactly the code execve uses
     * to read the image, and not while it polled (D-101). */
    mmu_switch_user(space);
    thread_current()->space = space;

    uint64_t entry = 0;
    if (elf_load(file, len, &entry) != 0) {
        mmu_switch_user(saved);
        thread_current()->space = saved_space;
        DROP_DISK();
        return (uint64_t)-8;                                /* -ENOEXEC */
    }
    void *stack = alloc_page();
    uint64_t stack_va = 0x7f00100000ul;
    if (!stack || map_user_page(stack_va, (uint64_t)(uintptr_t)stack, 0)) {
        mmu_switch_user(saved);
        thread_current()->space = saved_space;
        DROP_DISK();
        return (uint64_t)-12;
    }
    /* The image has been copied into the new address space; the staging pages
     * have no further use, and holding them would leak seventeen pages per
     * program run from the disk. */
    DROP_DISK();
    thread_current()->space = space;

    /* Lay the arguments out at the top of the new stack, the way every aarch64
     * Linux program is started and therefore the way a compiler's idea of
     * main() already expects:
     *
     *     sp -> argc, argv[0..argc-1], NULL, envp NULL
     *           the strings themselves, higher up
     *
     * Written through the kernel's own view of the page rather than through the
     * user mapping: map_user_page was handed a kernel address and converts it
     * itself, so `stack` is somewhere the kernel can simply store to, and
     * nothing here depends on EL1 being allowed to write EL0's pages.
     */
    unsigned char *page = (unsigned char *)stack;
    unsigned str_off = (PAGE_SIZE - used) & ~15u;
    for (unsigned k = 0; k < used; k++) page[str_off + k] = argbuf[k];

    unsigned ptr_bytes = (unsigned)(8 * (argc + 3) + 15) & ~15u;
    unsigned sp_off    = str_off - ptr_bytes;
    uint64_t *p = (uint64_t *)(uintptr_t)(page + sp_off);
    p[0] = (uint64_t)argc;
    unsigned at = 0;
    for (int k = 0; k < argc; k++) {
        p[1 + k] = stack_va + str_off + at;
        at += arglen[k];
    }
    p[1 + argc] = 0;                 /* end of argv */
    p[2 + argc] = 0;                 /* an empty environment, but a real one */

    /* The new image has none of the old one's handler addresses in it, so a
     * caught signal goes back to the default action. What was ignored stays
     * ignored: that is a decision about the signal and not about the code
     * (POSIX draws the line in the same place). */
    sig_exec_reset();

    frame_t *f = frameptr;
    f->elr    = entry;               /* return to the new program, not the old */
    f->spsr   = 0;                   /* EL0t, interrupts on                    */
    f->sp_el0 = stack_va + sp_off;
    for (int i = 0; i < 30; i++) f->x[i] = 0;
    if (kchatty())
        kprintf("  [kernel] process %llu is now running %s\n",
                (unsigned long long)thread_current()->id, name);
    return 0;
}

static uint64_t sys_read(uint64_t fd, uint64_t buf, uint64_t len) {
    file_t *f = fd_get(fd);
    if (!f) return (uint64_t)-9;                             /* -EBADF */
    if (!len) return 0;
    /* Checked before a character is waited for, not after: a read that will
     * fail should fail now rather than after blocking a person at a prompt. */
    if (!vm_user_ok(buf, len, 1)) return EFAULT;
    return (uint64_t)file_read(f, (char *)(uintptr_t)buf, len);
}

/* Give a process the three descriptors every program assumes it was started
 * with. A program that finds fd 1 closed does not check; it writes anyway and
 * the message goes nowhere. */
void syscall_open_stdio(void) {
    thread_t *t = thread_current();
    for (int i = 0; i < 3; i++)
        if (!t->fd[i]) t->fd[i] = file_console();
}

static int fd_alloc(file_t *f) {
    thread_t *t = thread_current();
    for (int i = 0; i < NFD; i++)
        if (!t->fd[i]) { t->fd[i] = f; return i; }
    return -1;
}

/* struct sockaddr_in, as every program has it: family, port and address, the
 * last two in network order, then eight bytes nobody uses. Read and written a
 * byte at a time for the same reason every packet field is (see net.c), and
 * because the address came from a program and may be at any alignment it
 * likes. */
#define SOCKADDR_LEN 16

static int sockaddr_read(uint64_t va, uint64_t len, uint32_t *ip, uint16_t *port) {
    if (len < SOCKADDR_LEN) return -1;
    if (!vm_user_ok(va, SOCKADDR_LEN, 0)) return -2;
    const uint8_t *p = (const uint8_t *)(uintptr_t)va;
    /* The family is two bytes in the host's order, which on aarch64 is little
     * endian; the port and address that follow are in the network's. Mixing
     * the two up is the classic way for a socket call to bind to a port
     * nobody meant. */
    uint16_t family = (uint16_t)(p[0] | (p[1] << 8));
    if (family != AF_INET) return -1;
    *port = (uint16_t)((p[2] << 8) | p[3]);
    *ip   = ((uint32_t)p[4] << 24) | ((uint32_t)p[5] << 16) |
            ((uint32_t)p[6] << 8)  |  (uint32_t)p[7];
    return 0;
}

static void sockaddr_write(uint64_t va, uint32_t ip, uint16_t port) {
    uint8_t *p = (uint8_t *)(uintptr_t)va;
    for (int i = 0; i < SOCKADDR_LEN; i++) p[i] = 0;
    p[0] = AF_INET; p[1] = 0;
    p[2] = (uint8_t)(port >> 8); p[3] = (uint8_t)port;
    p[4] = (uint8_t)(ip >> 24); p[5] = (uint8_t)(ip >> 16);
    p[6] = (uint8_t)(ip >> 8);  p[7] = (uint8_t)ip;
}

static uint64_t sys_socket(uint64_t domain, uint64_t type) {
    if (domain != AF_INET) return (uint64_t)-97;            /* -EAFNOSUPPORT */
    if (!net_present()) return (uint64_t)-97;
    unsigned kind = (unsigned)type & 0xf;
    /* Only what exists. Anything else is refused rather than quietly served as
     * one of these two: a program that gets a descriptor and then loses data
     * with no error anywhere is worse off than one that was told no. */
    if (kind != SOCK_DGRAM && kind != SOCK_STREAM) return (uint64_t)-93;
    file_t *f = 0;
    int e = (kind == SOCK_STREAM) ? file_tcp(&f) : file_udp(&f);
    if (e < 0) return (uint64_t)(int64_t)e;
    int fd = fd_alloc(f);
    if (fd < 0) { file_close(f); return (uint64_t)-24; }    /* -EMFILE */
    return (uint64_t)fd;
}

static uint64_t sys_bind(uint64_t fd, uint64_t addr, uint64_t len) {
    udp_sock_t *s = file_udp_sock(fd_get(fd));
    if (!s) return (uint64_t)-88;                           /* -ENOTSOCK */
    uint32_t ip; uint16_t port;
    int e = sockaddr_read(addr, len, &ip, &port);
    if (e == -2) return EFAULT;
    if (e) return (uint64_t)-22;                            /* -EINVAL */
    /* The address in a bind is which of this machine's addresses to listen on,
     * and this machine has one. Anything but that or 0.0.0.0 is a request the
     * kernel cannot honour, and saying so beats listening on the only address
     * there is and calling it success. */
    if (ip && ip != net_my_ip()) return (uint64_t)-99;      /* -EADDRNOTAVAIL */
    if (udp_bind(s, port) != 0) return (uint64_t)-98;       /* -EADDRINUSE */
    return 0;
}

static uint64_t sys_connect(uint64_t fd, uint64_t addr, uint64_t len) {
    tcp_sock_t *c = file_tcp_conn(fd_get(fd));
    if (!c) return (uint64_t)-88;                           /* -ENOTSOCK */
    uint32_t ip; uint16_t port;
    int e = sockaddr_read(addr, len, &ip, &port);
    if (e == -2) return EFAULT;
    if (e) return (uint64_t)-22;
    return (uint64_t)(int64_t)tcp_connect(c, ip, port);
}

static uint64_t sys_sendto(uint64_t fd, uint64_t buf, uint64_t len,
                           uint64_t addr, uint64_t alen) {
    udp_sock_t *s = file_udp_sock(fd_get(fd));
    if (!s) return (uint64_t)-88;
    if (!vm_user_ok(buf, len, 0)) return EFAULT;
    uint32_t ip; uint16_t port;
    int e = sockaddr_read(addr, alen, &ip, &port);
    if (e == -2) return EFAULT;
    if (e) return (uint64_t)-22;
    if (udp_sendto(s, ip, port, (const void *)(uintptr_t)buf, (unsigned)len) != 0)
        return (uint64_t)-101;                              /* -ENETUNREACH */
    return len;
}

/* Blocks, by polling. The card has no interrupt yet, so nothing arrives unless
 * somebody asks — which means the waiting thread has to be the one that asks.
 * It yields between attempts, so the machine is not held; it is still a busy
 * wait and it is written down rather than dressed up.
 *
 * Bounded at five seconds. A datagram that never comes must return an error
 * and not a process that cannot be killed. */
static uint64_t sys_recvfrom(uint64_t fd, uint64_t buf, uint64_t len,
                             uint64_t addr, uint64_t alen_ptr) {
    udp_sock_t *s = file_udp_sock(fd_get(fd));
    if (!s) return (uint64_t)-88;
    if (!vm_user_ok(buf, len, 1)) return EFAULT;
    if (addr) {
        if (!vm_user_ok(addr, SOCKADDR_LEN, 1)) return EFAULT;
        if (alen_ptr && !vm_user_ok(alen_ptr, sizeof(uint32_t), 1)) return EFAULT;
    }
    if (udp_port(s) == 0 && udp_bind(s, 0) != 0) return (uint64_t)-98;

    uint64_t deadline = timer_ticks() + timer_hz() * 5;
    for (;;) {
        net_poll();
        uint32_t from_ip; uint16_t from_port;
        int n = udp_recvfrom(s, (void *)(uintptr_t)buf, (unsigned)len,
                             &from_ip, &from_port);
        if (n >= 0) {
            if (addr) {
                sockaddr_write(addr, from_ip, from_port);
                if (alen_ptr) *(uint32_t *)(uintptr_t)alen_ptr = SOCKADDR_LEN;
            }
            return (uint64_t)n;
        }
        if (timer_ticks() >= deadline) return (uint64_t)-11;  /* -EAGAIN */
        /* Sleeps if the card's interrupt has proved itself, spins if it has
         * not. Either way it is bounded above, so a datagram that never comes
         * is an error and not a process nothing can kill. */
        net_wait(1);
    }
}

static uint64_t do_syscall(uint64_t nr, uint64_t a, uint64_t b, uint64_t c, void *frame) {
    switch (nr) {
    case SYS_READ:  return sys_read(a, b, c);
    case SYS_SOCKET: return sys_socket(a, b);
    case SYS_BIND:   return sys_bind(a, b, c);
    case SYS_CONNECT: return sys_connect(a, b, c);
    /* Four arguments and five: taken out of the frame, because do_syscall is
     * handed three. Every other call here fits in three and these two do not,
     * and reaching into the frame is honest about where the values are rather
     * than widening every signature for two callers. */
    case SYS_SENDTO: {
        frame_t *r = (frame_t *)frame;
        return sys_sendto(a, b, c, r->x[4], r->x[5]);
    }
    case SYS_RECVFROM: {
        frame_t *r = (frame_t *)frame;
        return sys_recvfrom(a, b, c, r->x[4], r->x[5]);
    }
    case SYS_CLONE:  return do_fork(frame);
    case SYS_EXECVE: return do_execve((const char *)(uintptr_t)a,
                                      (char *const *)(uintptr_t)b, frame);
    case SYS_WRITE:  return sys_write(a, b, c);
    case SYS_GETPID: return (uint64_t)thread_current()->id;
    case SYS_KILL: {
        /* Linux's convention, kept: a negative pid names a process group. It
         * is what a shell already uses to stop a whole pipeline. */
        int n = thread_signal((int)(long)a, (int)b);
        return n ? 0 : (uint64_t)-3;                          /* -ESRCH */
    }
    case SYS_SETPGID: return (uint64_t)(long)thread_setpgid((int)(long)a, (int)(long)b);
    case SYS_GETPGID: return (uint64_t)(long)thread_pgid_of((int)(long)a);
    case SYS_IOCTL: {
        /* Only the console, and only the two calls that say which group is in
         * the foreground. That is the whole interface a shell needs to make
         * Ctrl-C reach the right processes, and it is the interface every
         * shell already speaks — inventing a friendlier one would mean vsh
         * could not be replaced by anything else (D-030). */
        file_t *f = fd_get(a);
        if (!f) return (uint64_t)-9;
        if (b == TIOCSPGRP) {
            if (!vm_user_ok(c, sizeof(int), 0)) return EFAULT;
            console_set_foreground(*(const int *)(uintptr_t)c);
            return 0;
        }
        if (b == TIOCGPGRP) {
            if (!vm_user_ok(c, sizeof(int), 1)) return EFAULT;
            *(int *)(uintptr_t)c = console_foreground();
            return 0;
        }
        return (uint64_t)-25;                                 /* -ENOTTY */
    }
    case SYS_CLOSE: {
        if (a >= NFD || !thread_current()->fd[a]) return (uint64_t)-9;
        file_close(thread_current()->fd[a]);
        thread_current()->fd[a] = 0;
        return 0;
    }
    case SYS_DUP: {
        file_t *f = fd_get(a);
        if (!f) return (uint64_t)-9;
        int n = fd_alloc(file_ref(f));
        if (n < 0) { file_close(f); return (uint64_t)-24; }   /* -EMFILE */
        return (uint64_t)n;
    }
    case SYS_DUP3: {
        /* dup3(old, new, flags): new becomes another name for old. Closing
         * what was at `new` first is the whole point — this is how a shell
         * makes a pipe be a program's standard output without the program
         * knowing. Duplicating onto itself is an error in dup3, unlike dup2,
         * and refusing is better than the alternative: closing `old` and then
         * duplicating a descriptor that no longer exists. */
        file_t *f = fd_get(a);
        if (!f || b >= NFD) return (uint64_t)-9;
        if (a == b) return (uint64_t)-22;                     /* -EINVAL */
        file_ref(f);
        if (thread_current()->fd[b]) file_close(thread_current()->fd[b]);
        thread_current()->fd[b] = f;
        return b;
    }
    case SYS_OPENAT: {
        /* openat(dirfd, path, flags, mode). There are no directories, so the
         * dirfd is accepted and ignored rather than pretended about — a
         * relative path has nowhere else to be relative to. */
        long n = vm_user_strlen(b, 63);
        if (n < 0) return EFAULT;
        char name[64];
        const char *p = (const char *)(uintptr_t)b;
        for (long k = 0; k <= n; k++) name[k] = p[k];
        file_t *f = 0;
        int mode = OPEN_READ;
        if (c & O_APPEND)                    mode = OPEN_APPEND;
        else if (c & (O_WRONLY | O_CREAT))   mode = OPEN_WRITE;
        int rc = file_open(name, mode, &f);
        if (rc < 0) return (uint64_t)(long)rc;
        int fd = fd_alloc(f);
        if (fd < 0) { file_close(f); return (uint64_t)-24; }
        return (uint64_t)fd;
    }
    case SYS_PIPE2: {
        if (!vm_user_ok(a, 2 * sizeof(int), 1)) return EFAULT;
        file_t *r = 0, *w = 0;
        if (file_pipe(&r, &w) != 0) return (uint64_t)-24;     /* -EMFILE */
        int rf = fd_alloc(r);
        int wf = rf >= 0 ? fd_alloc(w) : -1;
        if (rf < 0 || wf < 0) {
            if (rf >= 0) { thread_current()->fd[rf] = 0; }
            file_close(r); file_close(w);
            return (uint64_t)-24;
        }
        int *out = (int *)(uintptr_t)a;
        out[0] = rf; out[1] = wf;
        return 0;
    }
    case SYS_WAIT4: {
        /* Validated before waiting, so a bad pointer is an error rather than a
         * child collected and its status thrown at an address nobody owns. */
        if (b && !vm_user_ok(b, sizeof(int), 1)) return EFAULT;
        int st = 0;
        /* WNOHANG is what makes `&` survivable. A shell that never collects a
         * background job leaves a zombie holding a slot, and sixteen of those
         * is a machine that cannot fork — which is D-054's leak arriving by a
         * different road. Returning 0 for "nothing finished yet" is Linux's
         * answer and the one a shell already knows how to read. */
        int id = (c & WNOHANG) ? thread_reap(&st) : thread_wait(&st);
        if (id == 0 && (c & WNOHANG)) return 0;
        if (id < 0) return (uint64_t)-10;                    /* -ECHILD */
        /* Checked again: thread_wait blocks, and the answer to "is this
         * address the caller's" is only true at the moment it is used. */
        if (b && !vm_user_ok(b, sizeof(int), 1)) return EFAULT;
        /* Linux packs the status; a plain exit code sits in the second byte. */
        if (b) *(int *)(uintptr_t)b = (st & 0xff) << 8;
        return (uint64_t)id;
    }
    case SYS_RT_SIGACTION: {
        /* Not Linux's struct sigaction: that carries a mask whose size depends
         * on the fourth argument, and a flags word almost all of whose bits
         * mean something this kernel does not do. Reading a struct whose
         * fields are ignored is a call that looks supported and is not, so the
         * arguments are the three things that are actually used and the number
         * is Linux's only because the numbering is the cheap part (D-039).
         *
         *   a = signal, b = handler, c = restorer
         *
         * The mask a handler runs under is the signal itself, which sig_deliver
         * adds regardless. */
        return (uint64_t)(int64_t)sig_action((int)a, b, c, 0);
    }
    case SYS_GETDENTS64: {
        if (!vm_user_ok(b, c, 1)) return EFAULT;
        file_t *f = fd_get((int)a);
        if (!f) return (uint64_t)-9;                        /* -EBADF */
        return (uint64_t)(int64_t)file_getdents(f, (char *)(uintptr_t)b, c);
    }
    case SYS_MKDIRAT:
    case SYS_UNLINKAT: {
        /* dirfd is accepted and ignored: there is no working directory yet, so
         * every path is absolute and AT_FDCWD is the only value that could be
         * meant. Ignoring it silently would be a call that looks like it
         * supports relative-to-a-directory and does not, so anything else is
         * refused. */
        if ((int)a != -100) return (uint64_t)-22;           /* AT_FDCWD, or -EINVAL */
        long n = vm_user_strlen(b, 127);
        if (n < 0) return EFAULT;
        char path[128];
        const char *src = (const char *)(uintptr_t)b;
        for (long i = 0; i < n; i++) path[i] = src[i];
        path[n] = 0;
        if (!fat_mounted()) return (uint64_t)-30;           /* -EROFS: no disk */
        int rc = (nr == SYS_MKDIRAT) ? fat_mkdir(path) : fat_unlink(path);
        if (rc == 0) fat_sync();
        return rc == 0 ? 0 : (uint64_t)-2;                  /* -ENOENT */
    }
    case SYS_RT_SIGRETURN:
        return sig_return(frame);
    case SYS_EXIT:
        thread_exit((int)a);                 /* does not return */
        return 0;
    default:
        kprintf("  [kernel] unknown system call %llu — returning -ENOSYS\n",
                (unsigned long long)nr);
        return (uint64_t)-38;                /* -ENOSYS */
    }
}

/* Everything synchronous that arrives from EL0 comes here: system calls, but
 * also a program's own faults. Only the first is expected. */
void handle_user_sync(regs_t *r) {
    uint64_t esr;
    __asm__ volatile("mrs %0, esr_el1" : "=r"(esr));
    /* System calls run with interrupts enabled. They have to: a call that waits
     * for a device cannot be woken by that device if the exception entry left
     * interrupts masked, which is a deadlock rather than a slow path. It also
     * means a system call can be preempted, and this kernel has no locking —
     * one CPU and a scheduler simple enough that it holds, but it is the next
     * thing to be wrong when either of those changes. */
    __asm__ volatile("msr daifclr, #2" ::: "memory");

    if ((esr >> 26) == 0x15) {              /* SVC from AArch64 */
        r->x[0] = do_syscall(r->x[8], r->x[0], r->x[1], r->x[2], r);
        /* On the way back out is where a signal is acted on. Not where it is
         * raised — that happens in an interrupt, with the target's stack
         * somewhere else — and not at an arbitrary point, because the kernel
         * has to be finished with whatever it was holding. There are no
         * handlers, so acting on it means dying of it, and the status says
         * which signal so a shell can report it.
         *
         * Unless one is installed, in which case sig_deliver has just edited
         * this frame to enter the handler instead, and the return below goes
         * there rather than back to the instruction after the svc. */
        int sig = thread_signalled();
        if (sig && !sig_deliver(sig, r)) thread_exit(128 + sig);
        return;
    }
    /* A write to a page shared by copy-on-write is not a program doing
     * something wrong, it is a program doing the ordinary thing to memory the
     * kernel arranged to share behind its back. Telling that apart from a real
     * permission fault is the whole reason the handler looks at the tables
     * rather than at the fault status alone: the status says "you may not
     * write here" in both cases.
     *
     * EC 0x24 is a data abort from EL0; ESR bit 6 (WnR) says it was a write.
     * Anything else falls through and the program is killed, as before. */
    if ((esr >> 26) == 0x24 && (esr & (1u << 6))) {
        uint64_t far;
        __asm__ volatile("mrs %0, far_el1" : "=r"(far));
        if (vm_cow_fault(far) == 0) return;
    }

    /* A program that faults is killed; the machine is not. This is the point
     * of the whole address-space exercise — a program cannot take the system
     * down by being wrong, only itself. */
    exception_report(8, r);
    kprintf("  [kernel] killing the program; the kernel continues.\n");
    thread_exit(-1);
}

/* Drop to EL0 and start running. SPSR of zero means: exception level 0, using
 * SP_EL0, with DAIF clear — interrupts enabled, so the timer can preempt a user
 * program exactly as it preempts a kernel thread. */
void enter_user(uint64_t entry, uint64_t stack_top) {
    __asm__ volatile(
        "msr sp_el0, %1\n"
        "msr spsr_el1, xzr\n"
        "msr elr_el1, %0\n"
        "eret\n"
        :: "r"(entry), "r"(stack_top) : "memory");
    __builtin_unreachable();
}
