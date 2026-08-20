/* The capability manifest a sav program carries.
 *
 * savc writes the set of system calls a program can possibly make into a note
 * in its ELF, built from the capability methods it actually generated. This
 * kernel reads that note when it loads the program and refuses every call
 * outside it.
 *
 * That is the point of doing both halves: in sav, authority is visible in a
 * signature — `entry fn main(out: Console)` can write and can do nothing else
 * — and a language can only promise that about programs its own compiler
 * built honestly. The note survives compilation and the kernel enforces it, so
 * a binary that asks for more is stopped by the machine rather than by a rule
 * a reader has to believe. A binary with no note is an ordinary program and is
 * not restricted; nothing that ran before this existed changes.
 */
#ifndef STAM_SAVNOTE_H
#define STAM_SAVNOTE_H
#include <stdint.h>

#define SAV_MAX_CALLS 48
#define SAV_CAPS_BYTES 64

typedef struct {
    int      present;                  /* the image carried a manifest */
    unsigned ncalls;
    uint32_t call[SAV_MAX_CALLS];
    char     caps[SAV_CAPS_BYTES];     /* capability names, NUL between, NUL at the end */
} sav_manifest_t;

/* Whether the manifest carries this call. A manifest that is not present
 * permits everything: that is the answer for a program this compiler did not
 * build, and it is deliberate rather than a default nobody chose. */
int sav_allows(const sav_manifest_t *m, uint64_t nr);

#endif
