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
    /* Where the directory entry that describes it lives, so a write can put the
     * new length back. A file found in the root of a mounted volume always has
     * one; zero means "not from a directory", which is what fat_open returns
     * for the root itself. */
    uint64_t dir_sec;
    uint32_t dir_off;
} fat_file_t;

int  fat_mount(void);
int  fat_mounted(void);
/* Absolute paths, '/' separated, case-insensitive. 0 if found. */
int  fat_open(const char *path, fat_file_t *out);
long fat_read(const fat_file_t *f, uint64_t off, void *buf, uint64_t len);
/* Writes, extending the chain as needed and putting the new length back in the
 * directory. The file must already exist: fat_create makes one. */
long fat_write(fat_file_t *f, uint64_t off, const void *buf, uint64_t len);
/* A new empty file. Refuses a name that does not fit 8.3 — long names are
 * vmkfs's job, and a checksum this code alone computed would be a rule with
 * nothing to check it. Refuses one that exists, so the caller decides. */
int  fat_create(const char *path, fat_file_t *out);
/* Length to zero and the chain returned to the free list. */
int  fat_truncate(fat_file_t *f);
/* Recompute FSInfo's free count. Called when a file is closed rather than when
 * a byte is written. */
void fat_sync(void);

/* The n-th name in a directory, or -1 when there is no n-th. */
int  fat_dirent(const fat_file_t *dir, uint32_t index,
                char name[261], uint32_t *size, int *is_dir);
int  fat_mkdir(const char *path);
/* A file, or an empty directory. */
int  fat_unlink(const char *path);
#endif
