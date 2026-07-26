#ifndef STAM_SCHED_H
#define STAM_SCHED_H
#include <stdint.h>

#define THREAD_RUNNABLE 0
#define THREAD_ZOMBIE   1   /* finished, but nobody has collected the result */
#define THREAD_BLOCKED  2   /* waiting for a child to finish                 */
#define THREAD_FREE     3   /* collected; the slot may be reused             */

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
} thread_t;

void      sched_init(const char *name_of_current);
thread_t *thread_create(const char *name, void (*fn)(void *), void *arg);
thread_t *thread_fork(const void *parent_frame, unsigned frame_bytes, uint64_t space);
void      schedule(void);
void      thread_yield(void);
void      thread_exit(int status);
int       thread_wait(int *status_out);
thread_t *thread_current(void);
thread_t *thread_at(int i);
int       thread_count(void);
int       threads_alive(void);
#endif
