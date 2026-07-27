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
#include "console.h"
#include "lock.h"

#define NPIPE      8
#define NFILE      32
#define PIPE_SIZE  4096

#define FILE_FREE     0
#define FILE_CONSOLE  1
#define FILE_PIPE_R   2
#define FILE_PIPE_W   3

typedef struct pipe {
    char     buf[PIPE_SIZE];
    unsigned head, tail;       /* head is where the next byte is written    */
    int      readers, writers; /* open ends, not blocked threads            */
    int      used;
} pipe_t;

struct file {
    int      refs;             /* how many descriptors, in any process      */
    int      kind;
    pipe_t  *pipe;
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
            return &files[i];
        }
    return 0;
}

file_t *file_console(void) {
    uint64_t f = irq_save();
    if (!console_file) console_file = file_alloc(FILE_CONSOLE, 0);
    else console_file->refs++;
    struct file *r = console_file;
    irq_restore(f);
    return r;
}

int file_pipe(file_t **read_end, file_t **write_end) {
    uint64_t f = irq_save();
    pipe_t *p = 0;
    for (int i = 0; i < NPIPE; i++)
        if (!pipes[i].used) { p = &pipes[i]; break; }
    if (!p) { irq_restore(f); return -1; }

    p->used = 1; p->head = p->tail = 0; p->readers = 1; p->writers = 1;
    struct file *r = file_alloc(FILE_PIPE_R, p);
    struct file *w = r ? file_alloc(FILE_PIPE_W, p) : 0;
    if (!r || !w) {
        /* Undo everything, in the order that leaves nothing half-open. */
        if (r) { r->kind = FILE_FREE; r->refs = 0; }
        p->used = 0;
        irq_restore(f);
        return -1;
    }
    *read_end = r; *write_end = w;
    irq_restore(f);
    return 0;
}

file_t *file_ref(file_t *f) {
    if (!f) return 0;
    uint64_t s = irq_save();
    f->refs++;
    irq_restore(s);
    return f;
}

/* The last holder of an end closes that end, and *that* is what the other side
 * sees: a reader whose writers reach zero is at end of file, a writer whose
 * readers reach zero is writing into nothing. Waking the other side here is
 * not an optimisation — without it a shell blocks forever on a command that
 * has already finished. */
void file_close(file_t *f) {
    if (!f) return;
    uint64_t s = irq_save();
    if (--f->refs > 0) { irq_restore(s); return; }

    pipe_t *p = f->pipe;
    int kind = f->kind;
    f->kind = FILE_FREE;
    f->pipe = 0;
    if (f == console_file) console_file = 0;

    if (p) {
        if (kind == FILE_PIPE_R) p->readers--;
        else                     p->writers--;
        if (p->readers <= 0 && p->writers <= 0) p->used = 0;
        thread_wake_pipe(p);
    }
    irq_restore(s);
}

static unsigned pipe_bytes(const pipe_t *p) {
    return (p->head - p->tail) % PIPE_SIZE;
}

long file_read(file_t *f, char *buf, uint64_t len) {
    if (!f || f->kind == FILE_FREE) return -9;              /* -EBADF */
    if (f->kind == FILE_PIPE_W) return -9;                  /* wrong end */
    if (f->kind == FILE_CONSOLE) return console_read_line(buf, len);
    if (!len) return 0;

    pipe_t *p = f->pipe;
    uint64_t s = irq_save();
    /* Masked across the test and the block, for the reason in
     * thread_block_on_console: a writer filling the pipe in the gap would
     * otherwise wake nobody and this reader would sleep on data that is
     * already there. */
    while (pipe_bytes(p) == 0) {
        if (p->writers <= 0) { irq_restore(s); return 0; }  /* end of file */
        thread_block_on_pipe(p);
    }
    uint64_t n = 0;
    while (n < len && pipe_bytes(p) > 0) {
        buf[n++] = p->buf[p->tail];
        p->tail = (p->tail + 1) % PIPE_SIZE;
    }
    thread_wake_pipe(p);            /* room for a writer that was waiting */
    irq_restore(s);
    return (long)n;
}

long file_write(file_t *f, const char *buf, uint64_t len) {
    if (!f || f->kind == FILE_FREE) return -9;              /* -EBADF */
    if (f->kind == FILE_PIPE_R) return -9;                  /* wrong end */
    if (f->kind == FILE_CONSOLE) {
        for (uint64_t i = 0; i < len; i++) kputc(buf[i]);
        return (long)len;
    }
    pipe_t *p = f->pipe;
    uint64_t s = irq_save();
    uint64_t n = 0;
    while (n < len) {
        /* Nobody left to read it. Linux raises SIGPIPE as well; there are no
         * signals here, so the error is the whole of the answer. */
        if (p->readers <= 0) { irq_restore(s); return n ? (long)n : -32; } /* -EPIPE */
        if (pipe_bytes(p) == PIPE_SIZE - 1) { thread_block_on_pipe(p); continue; }
        p->buf[p->head] = buf[n++];
        p->head = (p->head + 1) % PIPE_SIZE;
    }
    thread_wake_pipe(p);
    irq_restore(s);
    return (long)n;
}
