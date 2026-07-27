#ifndef STAM_SCHED_H
#define STAM_SCHED_H
#include <stdint.h>
#include "file.h"

#define NSIG    32          /* room for every signal number this kernel knows */

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
    int         pgid;        /* process group: what a signal from the        */
                             /* terminal is addressed to                     */
    int         pending;     /* signals raised and not yet acted on          */
    const void *wait_object; /* which pipe, when wait_reason is WAIT_PIPE   */
    uint64_t    blocked;     /* signals held off while a handler runs        */
    /* Where a caught signal goes. Index by signal number; handler 0 means the
     * default action, which is to die of it. */
    struct { uint64_t handler, restorer, mask; } sigact[NSIG];
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

/* Signals, with default dispositions and nothing else.
 *
 * A handler needs a frame built on the user stack, a way back through
 * sigreturn, and a mask that survives it — three things that are only worth
 * having once a program wants to catch something. Nothing here does. What is
 * needed is Ctrl-C: a program that will not stop is a shell nobody can use,
 * and terminating is a default action.
 *
 * So a signal is raised, and the process dies of it the next time the kernel
 * is in a position to say so. Saying which signal in the exit status is what
 * lets a shell report it. */
#define SIGINT   2
#define SIGKILL  9
#define SIGTERM 15

/* To one process, or to every member of a group when pid is negative. Returns
 * how many it reached — zero means nobody, which the caller may care about. */
int       thread_signal(int pid, int sig);

/* Handlers. sig_action installs one; sig_deliver arranges for the running
 * program to run it instead of dying, and answers 0 when there is nothing
 * installed and the caller must kill; sig_return undoes sig_deliver. */
int       sig_action(int sig, uint64_t handler, uint64_t restorer, uint64_t mask);
int       sig_deliver(int sig, void *frame);
uint64_t  sig_return(void *frame);
/* exec keeps what is ignored and drops what is caught: the handler's address
 * means nothing in the new image (POSIX says the same). */
void      sig_exec_reset(void);
/* Does the running thread have a signal it must die of? Called where the
 * kernel is about to return to EL0 or is about to block, because those are the
 * only two places where dying is safe and timely. */
int       thread_signalled(void);
int       thread_setpgid(int pid, int pgid);
int       thread_pgid_of(int pid);
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
