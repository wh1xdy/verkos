#ifndef STAM_SCHED_H
#define STAM_SCHED_H
#include <stdint.h>
#include "file.h"

#define THREAD_RUNNABLE 0
#define THREAD_ZOMBIE   1   /* finished, but nobody has collected the result */
#define THREAD_BLOCKED  2   /* waiting for something: see wait_reason        */
#define THREAD_FREE     3   /* collected, or not yet built: the slot is spare */

typedef struct {
    uint64_t    sp;          /* where its registers are, while not running */
    uint64_t    stack_low;
    int         id;
    int         state;
    const char *name;
    uint64_t    slices;      /* how many times it has been given the CPU   */
    uint64_t    space;       /* its low half, or 0 for a kernel-only thread */
    int         parent;      /* who is entitled to collect its exit status  */
    int         status;      /* what it exited with, once it is a zombie    */
    int         wait_reason; /* WAIT_CHILD, WAIT_CONSOLE or WAIT_PIPE       */
    const void *wait_object; /* which pipe, when wait_reason is WAIT_PIPE   */
    /* The open files, this process's own. Copied by fork — the descriptors
     * are duplicated and the objects behind them gain a holder, which is why
     * both halves of `a | b` can close their unused end without the other
     * losing it. */
    struct file *fd[NFD];
} thread_t;

void      sched_init(const char *name_of_current);
thread_t *thread_create(const char *name, void (*fn)(void *), void *arg);
thread_t *thread_fork(const void *parent_frame, unsigned frame_bytes, uint64_t space);
void      schedule(void);
void      thread_yield(void);
void      thread_exit(int status);
int       thread_wait(int *status_out);
/* Non-blocking: the child's id, 0 if none has finished, -1 if there are none. */
int       thread_reap(int *status_out);
#define WAIT_CHILD   1
#define WAIT_CONSOLE 2
#define WAIT_PIPE    3
void      thread_block_on_console(void);
void      thread_wake_console(void);
/* Pipes wake by the object rather than by thread: a reader and a writer are
 * both waiting on the same buffer for opposite reasons, and neither knows who
 * else is there. Waking everyone and letting each re-test its own condition is
 * the shape that cannot miss one — a targeted wakeup needs a list of waiters,
 * which is bookkeeping for a case that does not exist yet. Call with
 * interrupts masked, for the reason in thread_block_on_console. */
void      thread_block_on_pipe(const void *object);
void      thread_wake_pipe(const void *object);
thread_t *thread_current(void);
thread_t *thread_at(int i);
int       thread_count(void);
int       threads_alive(void);
#endif
