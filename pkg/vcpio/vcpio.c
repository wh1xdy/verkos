/* vcpio — VerkOS' own cpio (newc format).
 *
 * Reads file names on stdin and writes a "newc" archive (-o), or extracts/lists
 * one (-i / -it). Installed as `cpio`.
 *
 *   find . | cpio -o > arch.cpio
 *   cpio -i  < arch.cpio        (extract)
 *   cpio -it < arch.cpio        (list)
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

static int verbose;
static void pad4(FILE *o, long pos) { while (pos % 4) { fputc(0, o); pos++; } }

static void hexfield(FILE *o, unsigned v) { fprintf(o, "%08x", v); }

static void write_entry(FILE *o, const char *name, struct stat *st, const unsigned char *data, long *pos) {
    unsigned nsize = strlen(name) + 1;
    fputs("070701", o);
    hexfield(o, st ? (unsigned)st->st_ino : 0);
    hexfield(o, st ? (unsigned)st->st_mode : 0);
    hexfield(o, st ? (unsigned)st->st_uid : 0);
    hexfield(o, st ? (unsigned)st->st_gid : 0);
    hexfield(o, st ? (unsigned)st->st_nlink : 1);
    hexfield(o, st ? (unsigned)st->st_mtime : 0);
    hexfield(o, st ? (unsigned)st->st_size : 0);
    hexfield(o, 0); hexfield(o, 0);           /* devmajor/minor */
    hexfield(o, 0); hexfield(o, 0);           /* rdevmajor/minor */
    hexfield(o, nsize);
    hexfield(o, 0);                            /* check */
    *pos += 110;
    fwrite(name, 1, nsize, o); *pos += nsize;
    pad4(o, *pos); while (*pos % 4) (*pos)++;
    if (st && st->st_size && data) { fwrite(data, 1, st->st_size, o); *pos += st->st_size; pad4(o, *pos); while (*pos % 4) (*pos)++; }
}

static int do_create(FILE *o) {
    char line[4096]; long pos = 0;
    while (fgets(line, sizeof line, stdin)) {
        line[strcspn(line, "\n")] = 0;
        if (!line[0]) continue;
        struct stat st;
        if (lstat(line, &st) != 0) { fprintf(stderr, "cpio: %s: not found\n", line); continue; }
        if (S_ISREG(st.st_mode)) {
            FILE *f = fopen(line, "rb"); if (!f) continue;
            unsigned char *d = malloc(st.st_size + 1); size_t r = fread(d, 1, st.st_size, f); fclose(f);
            st.st_size = r;
            write_entry(o, line, &st, d, &pos); free(d);
        } else if (S_ISLNK(st.st_mode)) {
            /* A symlink's target IS its data in newc format: the size is the
             * length of the target and the bytes follow the header. Writing it
             * with size 0 (as this did) produces a link that points nowhere —
             * which silently breaks /bin/sh in an initramfs. */
            char target[4096];
            ssize_t n = readlink(line, target, sizeof target - 1);
            if (n < 0) { fprintf(stderr, "cpio: %s: cannot read link\n", line); continue; }
            target[n] = 0;
            st.st_size = n;
            write_entry(o, line, &st, (unsigned char *)target, &pos);
        } else { st.st_size = 0; write_entry(o, line, &st, NULL, &pos); }   /* dirs/etc: no data */
        if (verbose) fprintf(stderr, "%s\n", line);
    }
    struct stat z; memset(&z, 0, sizeof z); z.st_nlink = 1;
    write_entry(o, "TRAILER!!!", &z, NULL, &pos);
    return 0;
}

static unsigned rdhex(FILE *in) { char b[9]; if (fread(b, 1, 8, in) != 8) return 0xffffffff; b[8] = 0; return (unsigned)strtoul(b, NULL, 16); }
static int do_extract(FILE *in, int listonly) {
    char magic[7]; long pos = 0;
    for (;;) {
        if (fread(magic, 1, 6, in) != 6) break;
        magic[6] = 0; pos += 6;
        if (strcmp(magic, "070701")) { fprintf(stderr, "cpio: bad magic\n"); break; }
        unsigned f[13]; for (int i = 0; i < 13; i++) f[i] = rdhex(in); pos += 13 * 8;
        unsigned mode = f[1], filesize = f[6], nsize = f[11];
        char name[4096]; if (nsize > sizeof name) nsize = sizeof name; fread(name, 1, nsize, in); pos += nsize; name[nsize ? nsize - 1 : 0] = 0;
        while (pos % 4) { fgetc(in); pos++; }
        if (!strcmp(name, "TRAILER!!!")) break;
        if (listonly) printf("%s\n", name);
        FILE *of = NULL;
        if (!listonly && S_ISREG(mode)) { for (char *s = strchr(name, '/'); s; s = strchr(s + 1, '/')) { *s = 0; mkdir(name, 0755); *s = '/'; } of = fopen(name, "wb"); if (verbose) fprintf(stderr, "%s\n", name); }
        else if (!listonly && S_ISDIR(mode)) mkdir(name, mode & 07777);
        /* A symlink carries its target as the entry's data, so read it and
         * create the link rather than dropping the bytes on the floor. */
        int is_link = !listonly && S_ISLNK(mode) && filesize > 0 && filesize < 4096;
        char link_target[4096];
        unsigned rem = filesize; char buf[8192];
        unsigned got = 0;
        while (rem > 0) {
            size_t w = rem < sizeof buf ? rem : sizeof buf;
            size_t r = fread(buf, 1, w, in);
            if (!r) break;
            if (of) fwrite(buf, 1, r, of);
            if (is_link && got + r < sizeof link_target) { memcpy(link_target + got, buf, r); got += r; }
            rem -= r; pos += r;
        }
        if (is_link) {
            link_target[got] = 0;
            for (char *s = strchr(name, '/'); s; s = strchr(s + 1, '/')) { *s = 0; mkdir(name, 0755); *s = '/'; }
            unlink(name);
            if (symlink(link_target, name) && verbose) fprintf(stderr, "cpio: %s: cannot create link\n", name);
            else if (verbose) fprintf(stderr, "%s -> %s\n", name, link_target);
        }
        if (of) { fclose(of); chmod(name, mode & 07777); }
        while (pos % 4) { fgetc(in); pos++; }
    }
    return 0;
}

int main(int argc, char **argv) {
    int mode = 0, listonly = 0;
    for (int i = 1; i < argc; i++)
        for (char *p = argv[i] + (argv[i][0] == '-' ? 1 : 0); argv[i][0] == '-' && *p; p++) {
            if (*p == 'o') mode = 'o'; else if (*p == 'i') mode = 'i'; else if (*p == 't') listonly = 1; else if (*p == 'v') verbose = 1;
        }
    if (mode == 'o') return do_create(stdout);
    if (mode == 'i') return do_extract(stdin, listonly);
    fprintf(stderr, "usage: cpio -o | -i[t] [-v]\n");
    return 1;
}
