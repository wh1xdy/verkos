#ifndef STAM_FAT_H
#define STAM_FAT_H
#include <stdint.h>

/* A file the reader has found: where its chain starts, how long it is, and
 * whether it is a directory. Deliberately not a handle — nothing is held open,
 * so nothing has to be closed and a stale one cannot exist. */
typedef struct {
    uint32_t cluster;
    uint32_t size;
    int      is_dir;
} fat_file_t;

int  fat_mount(void);
int  fat_mounted(void);
/* Absolute paths, '/' separated, case-insensitive. 0 if found. */
int  fat_open(const char *path, fat_file_t *out);
long fat_read(const fat_file_t *f, uint64_t off, void *buf, uint64_t len);
#endif
