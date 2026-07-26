#ifndef STAM_SCHED_H
#define STAM_SCHED_H
#include <stdint.h>

#define THREAD_RUNNABLE 0
#define THREAD_FINISHED 1

typedef struct {
    uint64_t    sp;          /* where its registers are, while not running */
    uint64_t    stack_low;
    int         id;
    int         state;
    const char *name;
    uint64_t    slices;      /* how many times it has been given the CPU   */
} thread_t;

void      sched_init(const char *name_of_current);
thread_t *thread_create(const char *name, void (*fn)(void *), void *arg);
void      schedule(void);
void      thread_yield(void);
void      thread_exit(void);
thread_t *thread_current(void);
thread_t *thread_at(int i);
int       thread_count(void);
int       threads_alive(void);
#endif
