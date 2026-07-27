/* stam — what a file descriptor is.
 *
 * Until now `read` meant the console and `write` meant the console, and the
 * number in front of them was checked only to reject anything unexpected. A
 * shell cannot be written that way: `a | b` is two programs that believe they
 * are talking to a terminal, and the whole trick is that neither of them knows
 * otherwise. So the number has to name an object, and the object has to be the
 * process's own.
 *
 * Three things fall out of that and none of them is optional:
 *
 *   - the table belongs to the process, so fork duplicates it,
 *   - the objects are shared, so they are reference counted — after a fork both
 *     processes hold the same pipe, and it is closed when the last of them
 *     lets go, not when the first does,
 *   - closing the last writer must be visible to the reader, because that is
 *     what end-of-file *is*. A pipe that never reports EOF is a shell that
 *     hangs after every command.
 */
#ifndef STAM_FILE_H
#define STAM_FILE_H
#include <stdint.h>

#define NFD 8                  /* per process. A shell needs three and a pipe. */

typedef struct file file_t;

/* The console, as an object. One instance, shared by every process that has
 * not been redirected — refcounted like everything else so that closing it in
 * one process does not take it from another. */
file_t *file_console(void);

/* A pipe: two ends, one buffer. Returns 0, or -1 if the kernel is out of
 * either pipes or file objects. */
int file_pipe(file_t **read_end, file_t **write_end);

file_t *file_ref(file_t *f);   /* one more holder */
void    file_close(file_t *f); /* one fewer; the last one releases it */

/* Both block, and both are the reason a shell works at all: a reader waits for
 * a writer that has not written yet, and a writer waits for a reader that has
 * not caught up. Returning early instead would make `yes | head` a busy loop
 * and `big | slow` a lost stream. */
long file_read(file_t *f, char *buf, uint64_t len);
long file_write(file_t *f, const char *buf, uint64_t len);

#endif
