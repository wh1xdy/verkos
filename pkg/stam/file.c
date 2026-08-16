/* stam — file objects and pipes.
 *
 * Fixed pools, not an allocator. A pipe is one page of buffer and there are
 * eight of them; a machine that wants more than eight pipes at once wants a
 * different kernel, and saying so with a number is better than discovering it
 * as a failed page allocation somewhere else. Refusing is `-EMFILE`, which is
 * a thing programs already know how to be told.
 */
#include "file.h"
#include "sched.h"
#include "fat.h"
#include "net.h"
#include "tcp.h"
#include "console.h"
#include "lock.h"
#include "cpio.h"

#define NPIPE      8
#define NFILE      32
#define PIPE_SIZE  4096

#define FILE_FREE     0
#define FILE_CONSOLE  1
#define FILE_PIPE_R   2
#define FILE_PIPE_W   3
#define FILE_INITRD   4        /* read-only, straight out of the cpio archive */
#define FILE_MEM      5        /* a named buffer that lasts until the reboot  */
#define FILE_FAT      6        /* off the disk                                */
#define FILE_FATDIR   7        /* a directory on the disk, only readable as names */
#define FILE_UDP      8        /* a datagram socket                           */
#define FILE_TCP      9        /* a connection                                */

#define NMEM       4
#define MEM_SIZE   4096
#define NAME_MAX   32

typedef struct pipe {
    char     buf[PIPE_SIZE];
    unsigned head, tail;       /* head is where the next byte is written    */
    int      readers, writers; /* open ends, not blocked threads            */
    int      used;
} pipe_t;

typedef struct memfile {
    char     name[NAME_MAX];
    char     data[MEM_SIZE];
    uint64_t len;
    int      used;
} memfile_t;

struct file {
    int      refs;             /* how many descriptors, in any process      */
    int      kind;
    pipe_t  *pipe;
    /* For the two kinds that are a sequence of bytes rather than a channel.
     * The position belongs to the open file, not to the file: two programs
     * that opened the same name each read it from the beginning, which is
     * what `a < f | b < f` would need and what every Unix does. */
    memfile_t     *mem;
    const uint8_t *ro;         /* initrd contents, borrowed not owned       */
    uint64_t       ro_len;
    fat_file_t     fat;        /* where it is on the disk, for FILE_FAT     */
    udp_sock_t    *sock;       /* for FILE_UDP                              */
    tcp_sock_t    *conn;       /* for FILE_TCP                              */
    int            writable;   /* a FILE_FAT opened for writing              */
    uint64_t       pos;
};

static pipe_t   pipes[NPIPE];
static struct file files[NFILE];
static struct file *console_file;

static struct file *file_alloc(int kind, pipe_t *p) {
    for (int i = 0; i < NFILE; i++)
        if (files[i].kind == FILE_FREE) {
            files[i].kind = kind;
            files[i].refs = 1;
            files[i].pipe = p;
            files[i].mem  = 0;
            files[i].ro   = 0;
            files[i].ro_len = 0;
            files[i].pos  = 0;
            files[i].sock = 0;
            files[i].conn = 0;
            return &files[i];
        }
    return 0;
}

static memfile_t memfiles[NMEM];
static uint64_t archive_addr, archive_size;

void file_set_archive(uint64_t addr, uint64_t size) {
    archive_addr = addr; archive_size = size;
}

static int name_eq(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

/* A leading slash is accepted and ignored, as cpio_find does: the archive
 * stores names without one and people type one. */
static const char *skip_slash(const char *s) { return s[0] == '/' ? s + 1 : s; }

int file_open(const char *name, int mode, file_t **out) {
    /* An absolute name means the disk, when there is one. A relative one is a
     * memory file, as it has been since D-060 — so the shell's `hello > saved`
     * still keeps its output in memory and `hello > /saved` puts it on the
     * medium. One character, and it is the difference between a file that
     * survives the reboot and one that does not, which is a thing a program
     * should be able to say rather than have decided for it. */
    int absolute = (name[0] == '/');
    name = skip_slash(name);

    /* The disk, before the lock and not under it.
     *
     * Looking a name up on the medium is a directory scan, and creating one is
     * that plus an allocation and a cluster to clear — hundreds of polled
     * sector reads, every one of them with interrupts masked if this were
     * inside the section. That is the timer not ticking and the console not
     * being read for the duration, which showed as a boot that sometimes did
     * not finish rather than as anything pointing here. Nothing else can reach
     * an open file that does not exist yet, so there is nothing to guard. */
    fat_file_t ff;
    int fat_kind = 0;                 /* 0 none, FILE_FAT, or FILE_FATDIR */
    int fat_write_open = 0;
    if (fat_mounted()) {
        if (mode != OPEN_READ && absolute) {
            int have = (fat_open(name, &ff) == 0 && !ff.is_dir);
            if (!have && fat_create(name, &ff) != 0) return -2;
            if (have && mode == OPEN_WRITE && fat_truncate(&ff) != 0) return -5;
            fat_kind = FILE_FAT;
            fat_write_open = 1;
        } else if (mode == OPEN_READ) {
            /* Looked up now, used only if the memory files and the initrd have
             * nothing by this name — the order in D-079, unchanged. */
            if (fat_open(name, &ff) == 0) fat_kind = ff.is_dir ? FILE_FATDIR : FILE_FAT;
        }
    }

    uint64_t s = sched_lock_irq();

    if (fat_write_open) {
        struct file *df = file_alloc(FILE_FAT, 0);
        if (!df) { sched_unlock_irq(s); return -24; }
        df->fat      = ff;
        df->ro_len   = ff.size;
        df->writable = 1;
        df->pos      = (mode == OPEN_APPEND) ? ff.size : 0;
        *out = df;
        sched_unlock_irq(s);
        return 0;
    }

    if (mode != OPEN_READ) {
        memfile_t *m = 0;
        for (int i = 0; i < NMEM; i++)
            if (memfiles[i].used && name_eq(memfiles[i].name, name)) { m = &memfiles[i]; break; }
        if (!m)
            for (int i = 0; i < NMEM; i++)
                if (!memfiles[i].used) { m = &memfiles[i]; break; }
        if (!m) { sched_unlock_irq(s); return -24; }              /* -EMFILE */
        unsigned k = 0;
        for (; k < NAME_MAX - 1 && name[k]; k++) m->name[k] = name[k];
        m->name[k] = 0;
        /* Truncating is what `>` means and appending is what `>>` means, and
         * the difference is one line here and the whole difference between
         * losing yesterday's log and keeping it. */
        int existed = m->used;
        if (mode == OPEN_WRITE || !existed) m->len = 0;
        m->used = 1;
        struct file *f = file_alloc(FILE_MEM, 0);
        if (!f) { sched_unlock_irq(s); return -24; }
        f->mem = m;
        f->pos = (mode == OPEN_APPEND) ? m->len : 0;
        *out = f;
        sched_unlock_irq(s);
        return 0;
    }

    /* Reading: a memory file if one exists by that name, otherwise the
     * initrd. That order matters — a program that wrote a file expects to
     * read back what it wrote, not the archive entry it shadowed. */
    for (int i = 0; i < NMEM; i++)
        if (memfiles[i].used && name_eq(memfiles[i].name, name)) {
            struct file *f = file_alloc(FILE_MEM, 0);
            if (!f) { sched_unlock_irq(s); return -24; }
            f->mem = &memfiles[i]; f->pos = 0;
            *out = f;
            sched_unlock_irq(s);
            return 0;
        }

    uint64_t at = 0, len = 0;
    if (!archive_size || cpio_find(archive_addr, archive_size, name, &at, &len) != 0) {
        /* Then the disk. Last, so that a name in the initrd still wins: the
         * initrd is what the machine was started with and the disk is what it
         * found, and a boot that silently preferred the disk would be a boot
         * whose behaviour depends on what is plugged in. */
        if (fat_kind) {
            struct file *df = file_alloc(fat_kind, 0);
            if (!df) { sched_unlock_irq(s); return -24; }
            df->fat = ff;
            df->ro_len = ff.size;
            df->pos = 0;
            *out = df;
            sched_unlock_irq(s);
            return 0;
        }
        sched_unlock_irq(s);
        return -2;                                            /* -ENOENT */
    }
    struct file *f = file_alloc(FILE_INITRD, 0);
    if (!f) { sched_unlock_irq(s); return -24; }
    f->ro = (const uint8_t *)(uintptr_t)at;
    f->ro_len = len;
    f->pos = 0;
    *out = f;
    sched_unlock_irq(s);
    return 0;
}

file_t *file_console(void) {
    uint64_t f = sched_lock_irq();
    if (!console_file) console_file = file_alloc(FILE_CONSOLE, 0);
    else console_file->refs++;
    struct file *r = console_file;
    sched_unlock_irq(f);
    return r;
}

int file_pipe(file_t **read_end, file_t **write_end) {
    uint64_t f = sched_lock_irq();
    pipe_t *p = 0;
    for (int i = 0; i < NPIPE; i++)
        if (!pipes[i].used) { p = &pipes[i]; break; }
    if (!p) { sched_unlock_irq(f); return -1; }

    p->used = 1; p->head = p->tail = 0; p->readers = 1; p->writers = 1;
    struct file *r = file_alloc(FILE_PIPE_R, p);
    struct file *w = r ? file_alloc(FILE_PIPE_W, p) : 0;
    if (!r || !w) {
        /* Undo everything, in the order that leaves nothing half-open. */
        if (r) { r->kind = FILE_FREE; r->refs = 0; }
        p->used = 0;
        sched_unlock_irq(f);
        return -1;
    }
    *read_end = r; *write_end = w;
    sched_unlock_irq(f);
    return 0;
}

file_t *file_ref(file_t *f) {
    if (!f) return 0;
    uint64_t s = sched_lock_irq();
    f->refs++;
    sched_unlock_irq(s);
    return f;
}

/* The last holder of an end closes that end, and *that* is what the other side
 * sees: a reader whose writers reach zero is at end of file, a writer whose
 * readers reach zero is writing into nothing. Waking the other side here is
 * not an optimisation — without it a shell blocks forever on a command that
 * has already finished. */
void file_close(file_t *f) {
    if (!f) return;
    uint64_t s = sched_lock_irq();
    if (--f->refs > 0) { sched_unlock_irq(s); return; }

    pipe_t *p = f->pipe;
    int kind = f->kind;
    int was_written = (kind == FILE_FAT && f->writable);
    udp_sock_t *sock = (kind == FILE_UDP) ? f->sock : 0;
    tcp_sock_t *conn = (kind == FILE_TCP) ? f->conn : 0;
    f->kind = FILE_FREE;
    f->pipe = 0;
    f->sock = 0;
    f->conn = 0;
    if (f == console_file) console_file = 0;

    if (p) {
        if (kind == FILE_PIPE_R) p->readers--;
        else                     p->writers--;
        if (p->readers <= 0 && p->writers <= 0) p->used = 0;
        thread_wake_pipe(p);
    }
    sched_unlock_irq(s);

    /* Outside the lock, for the same reason as the sync below: udp_close walks
     * the socket table, which no scheduling decision should ever wait for. */
    if (sock) udp_close(sock);
    /* And this one sends a FIN and waits briefly for the answer, which is
     * far too much to do holding the lock every scheduling decision on the
     * machine goes through. */
    if (conn) tcp_close(conn);

    /* Outside the lock: recounting the free list is a few hundred sector reads,
     * and every scheduling decision on the machine waits behind that lock. On
     * close and not on every write, because the number is a hint to other
     * readers and not something this code depends on. */
    if (was_written) fat_sync();
}

int file_udp(file_t **out) {
    udp_sock_t *s = udp_open();
    if (!s) return -105;                    /* -ENOBUFS: no socket free  */
    struct file *f = file_alloc(FILE_UDP, 0);
    if (!f) { udp_close(s); return -24; }   /* -EMFILE                   */
    f->sock = s;
    *out = f;
    return 0;
}

udp_sock_t *file_udp_sock(file_t *f) {
    return (f && f->kind == FILE_UDP) ? f->sock : 0;
}

int file_tcp(file_t **out) {
    tcp_sock_t *c = tcp_open();
    if (!c) return -105;                    /* -ENOBUFS: no connection free */
    struct file *f = file_alloc(FILE_TCP, 0);
    if (!f) { tcp_close(c); return -24; }   /* -EMFILE                      */
    f->conn = c;
    *out = f;
    return 0;
}

tcp_sock_t *file_tcp_conn(file_t *f) {
    return (f && f->kind == FILE_TCP) ? f->conn : 0;
}

static unsigned pipe_bytes(const pipe_t *p) {
    return (p->head - p->tail) % PIPE_SIZE;
}

long file_read(file_t *f, char *buf, uint64_t len) {
    if (!f || f->kind == FILE_FREE) return -9;              /* -EBADF */
    if (f->kind == FILE_PIPE_W) return -9;                  /* wrong end */
    if (f->kind == FILE_CONSOLE) return console_read_line(buf, len);
    /* A connection is a stream, so read means exactly what it means on a pipe:
     * whatever has arrived, and 0 when the other end has closed. */
    if (f->kind == FILE_TCP) return tcp_recv(f->conn, (uint8_t *)buf, (unsigned)len);
    if (f->kind == FILE_UDP) return -9;     /* use recvfrom; a datagram has a sender */
    if (!len) return 0;

    /* A file is a sequence of bytes with a position, and running off the end
     * is end of file rather than an error — which is what makes `upper < f`
     * terminate at all. */
    if (f->kind == FILE_INITRD || f->kind == FILE_MEM) {
        const char *src = f->kind == FILE_MEM ? f->mem->data : (const char *)f->ro;
        uint64_t total  = f->kind == FILE_MEM ? f->mem->len  : f->ro_len;
        uint64_t s2 = sched_lock_irq();
        uint64_t n = 0;
        while (n < len && f->pos < total) buf[n++] = src[f->pos++];
        sched_unlock_irq(s2);
        return (long)n;
    }

    if (f->kind == FILE_FAT) {
        /* Not under the scheduler lock. A sector read polls the device for as
         * long as it takes, and holding the lock that the console, the pipes
         * and every scheduling decision need for the duration of a disk access
         * is a machine that stops answering while it reads. The position
         * belongs to this open file and nothing else touches it. */
        long n = fat_read(&f->fat, f->pos, buf, len);
        if (n > 0) f->pos += (uint64_t)n;
        return n;
    }

    pipe_t *p = f->pipe;
    uint64_t s = sched_lock_irq();
    /* Masked across the test and the block, for the reason in
     * thread_block_on_console: a writer filling the pipe in the gap would
     * otherwise wake nobody and this reader would sleep on data that is
     * already there. */
    while (pipe_bytes(p) == 0) {
        if (p->writers <= 0) { sched_unlock_irq(s); return 0; }  /* end of file */
        thread_block_on_pipe(p);
    }
    uint64_t n = 0;
    while (n < len && pipe_bytes(p) > 0) {
        buf[n++] = p->buf[p->tail];
        p->tail = (p->tail + 1) % PIPE_SIZE;
    }
    thread_wake_pipe(p);            /* room for a writer that was waiting */
    sched_unlock_irq(s);
    return (long)n;
}

/* Names out of a directory, in the layout Linux's getdents64 uses.
 *
 * The position is an entry index rather than a byte offset, which is what the
 * call's d_off field is for — a program that keeps it and hands it back gets
 * the next name. Every entry is padded to a multiple of eight because the
 * structure begins with a 64-bit field and the next one follows it directly.
 */
long file_getdents(file_t *f, char *buf, uint64_t len) {
    if (!f || f->kind != FILE_FATDIR) return -20;          /* -ENOTDIR */
    uint64_t used = 0;
    for (;;) {
        char name[261];
        uint32_t size; int is_dir;
        if (fat_dirent(&f->fat, (uint32_t)f->pos, name, &size, &is_dir) != 0) break;
        unsigned n = 0; while (name[n]) n++;
        uint64_t rec = (19 + n + 1 + 7) & ~7ull;
        if (used + rec > len) {
            /* Nothing at all fits: say so rather than return zero, which a
             * program reads as "the directory ended". */
            if (used == 0) return -22;                     /* -EINVAL */
            break;
        }
        char *e = buf + used;
        for (int i = 0; i < 8; i++) e[i] = 0;              /* d_ino: we have none */
        uint64_t off = f->pos + 1;
        for (int i = 0; i < 8; i++) e[8 + i] = (char)(off >> (8 * i));
        e[16] = (char)(rec & 0xff);
        e[17] = (char)(rec >> 8);
        e[18] = (char)(is_dir ? 4 : 8);                    /* DT_DIR : DT_REG */
        for (unsigned i = 0; i <= n; i++) e[19 + i] = name[i];
        for (uint64_t i = 19 + n + 1; i < rec; i++) e[i] = 0;
        used += rec;
        f->pos++;
    }
    return (long)used;
}

long file_write(file_t *f, const char *buf, uint64_t len) {
    if (!f || f->kind == FILE_FREE) return -9;              /* -EBADF */
    if (f->kind == FILE_PIPE_R) return -9;                  /* wrong end */
    if (f->kind == FILE_CONSOLE) {
        for (uint64_t i = 0; i < len; i++) kputc(buf[i]);
        return (long)len;
    }
    if (f->kind == FILE_TCP) return tcp_send(f->conn, (const uint8_t *)buf, (unsigned)len);
    if (f->kind == FILE_UDP) return -9;     /* use sendto; a datagram needs a destination */
    if (f->kind == FILE_INITRD) return -9;   /* the archive is read-only */
    if (f->kind == FILE_FAT) {
        if (!f->writable) return -9;
        /* Not under the scheduler lock, for the reason file_read gives: a
         * sector write polls the device, and the console and the pipes need
         * that lock while it does. */
        long n = fat_write(&f->fat, f->pos, buf, len);
        if (n > 0) f->pos += (uint64_t)n;
        return n;
    }
    if (f->kind == FILE_MEM) {
        /* A short write when the buffer fills, which is what a program should
         * be told rather than having the excess silently dropped. */
        uint64_t s2 = sched_lock_irq();
        uint64_t n = 0;
        while (n < len && f->pos < MEM_SIZE) { f->mem->data[f->pos++] = buf[n++]; }
        if (f->pos > f->mem->len) f->mem->len = f->pos;
        sched_unlock_irq(s2);
        if (n == 0 && len) { return -28; }   /* -ENOSPC */
        return (long)n;
    }
    pipe_t *p = f->pipe;
    uint64_t s = sched_lock_irq();
    uint64_t n = 0;
    while (n < len) {
        /* Nobody left to read it. Linux raises SIGPIPE as well; there are no
         * signals here, so the error is the whole of the answer. */
        if (p->readers <= 0) { sched_unlock_irq(s); return n ? (long)n : -32; } /* -EPIPE */
        if (pipe_bytes(p) == PIPE_SIZE - 1) { thread_block_on_pipe(p); continue; }
        p->buf[p->head] = buf[n++];
        p->head = (p->head + 1) % PIPE_SIZE;
    }
    thread_wake_pipe(p);
    sched_unlock_irq(s);
    return (long)n;
}
