#ifndef STAM_GPT_H
#define STAM_GPT_H
#include <stdint.h>

/* The EFI system partition, if the disk has a GPT with one in it. Returns 0
 * and fills both, or -1 — which covers "no table" and "a table with no ESP",
 * and prints which. */
int gpt_find_esp(uint64_t *first_lba, uint64_t *sectors);
#endif
