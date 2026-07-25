/* verkmod — VerkOS' own kernel module tools (insmod, rmmod, lsmod, modinfo,
 * depmod, modprobe in one multi-call binary, BusyBox-style).
 *
 * kmod was removed with systemd, and without a loader the modules the kernel
 * build installs are unreachable. This replaces it: the loading itself is two
 * syscalls, and the only real work is answering "which file is module X, and
 * what does it need first?" — which we do by reading each module's .modinfo
 * section straight out of the ELF, the same place depmod reads it.
 *
 *   insmod  FILE [param=value ...]   load exactly this file
 *   rmmod   NAME...                  unload
 *   lsmod                            what is loaded (from /proc/modules)
 *   modinfo NAME|FILE                show a module's metadata
 *   depmod                           (re)build modules.dep
 *   modprobe [-r] NAME [param=...]   load a module and its dependencies
 *
 * Modules are expected uncompressed (.ko); VerkOS builds them that way.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/utsname.h>
#include <elf.h>

#define MODDIR_BASE "/usr/lib/modules"
#define MAX_DEPS 32

static int verbose;
static char moddir[512];            /* /usr/lib/modules/<release> */

/* ---- the two syscalls that actually load and unload ---------------------- */
static int sys_finit_module(int fd, const char *args, int flags) {
    return (int)syscall(SYS_finit_module, fd, args, flags);
}
static int sys_delete_module(const char *name, int flags) {
    return (int)syscall(SYS_delete_module, name, flags);
}

/* ---- where the modules live ---------------------------------------------- */
static void find_moddir(void) {
    struct utsname u;
    if (uname(&u)) { snprintf(moddir, sizeof moddir, MODDIR_BASE); return; }
    snprintf(moddir, sizeof moddir, MODDIR_BASE "/%s", u.release);
}

/* ---- reading .modinfo out of the ELF -------------------------------------
 * A module's dependencies, name and parameters live in a plain section of
 * NUL-separated "key=value" strings. Both architectures VerkOS targets are
 * 64-bit little-endian ELF, so one reader covers them. */
static char *read_modinfo(const char *path, size_t *out_len) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return NULL;
    struct stat st;
    if (fstat(fd, &st) || (size_t)st.st_size < sizeof(Elf64_Ehdr)) { close(fd); return NULL; }

    char *buf = malloc((size_t)st.st_size);
    if (!buf) { close(fd); return NULL; }
    ssize_t got = read(fd, buf, (size_t)st.st_size);
    close(fd);
    if (got != st.st_size) { free(buf); return NULL; }

    Elf64_Ehdr *eh = (Elf64_Ehdr *)buf;
    if (memcmp(eh->e_ident, ELFMAG, SELFMAG) || eh->e_ident[EI_CLASS] != ELFCLASS64) {
        free(buf); return NULL;
    }
    if (eh->e_shoff == 0 || eh->e_shstrndx >= eh->e_shnum) { free(buf); return NULL; }

    Elf64_Shdr *sh = (Elf64_Shdr *)(buf + eh->e_shoff);
    const char *shstr = buf + sh[eh->e_shstrndx].sh_offset;
    for (int i = 0; i < eh->e_shnum; i++) {
        if (strcmp(shstr + sh[i].sh_name, ".modinfo")) continue;
        char *info = malloc(sh[i].sh_size + 1);
        if (!info) break;
        memcpy(info, buf + sh[i].sh_offset, sh[i].sh_size);
        info[sh[i].sh_size] = 0;
        *out_len = sh[i].sh_size;
        free(buf);
        return info;
    }
    free(buf);
    return NULL;
}
/* pull one key out of a .modinfo blob (the blob is NUL-separated entries) */
static int modinfo_get(const char *info, size_t len, const char *key, char *out, size_t cap) {
    size_t klen = strlen(key);
    for (size_t i = 0; i < len; ) {
        const char *e = info + i;
        size_t elen = strlen(e);
        if (!strncmp(e, key, klen) && e[klen] == '=') {
            snprintf(out, cap, "%s", e + klen + 1);
            return 1;
        }
        i += elen + 1;
        if (!elen) i++;                       /* skip padding NULs */
    }
    return 0;
}

/* ---- what is loaded right now -------------------------------------------- */
static int is_loaded(const char *name) {
    FILE *f = fopen("/proc/modules", "r");
    if (!f) return 0;
    char line[512], mod[128];
    int found = 0;
    while (fgets(line, sizeof line, f))
        if (sscanf(line, "%127s", mod) == 1 && !strcmp(mod, name)) { found = 1; break; }
    fclose(f);
    return found;
}
static int cmd_lsmod(void) {
    FILE *f = fopen("/proc/modules", "r");
    if (!f) { fprintf(stderr, "lsmod: /proc/modules: %s\n", strerror(errno)); return 1; }
    printf("%-28s %8s  %s\n", "Module", "Size", "Used by");
    char line[512];
    while (fgets(line, sizeof line, f)) {
        char mod[128], used[256] = "";
        long size = 0; int refs = 0;
        /* name size refcount deps state address */
        if (sscanf(line, "%127s %ld %d %255s", mod, &size, &refs, used) < 3) continue;
        if (!strcmp(used, "-")) used[0] = 0;
        size_t l = strlen(used);
        if (l && used[l-1] == ',') used[l-1] = 0;   /* trailing comma */
        printf("%-28s %8ld  %d %s\n", mod, size, refs, used);
    }
    fclose(f);
    return 0;
}

/* ---- module name <-> file ------------------------------------------------- */
static void basename_noext(const char *path, char *out, size_t cap) {
    const char *b = strrchr(path, '/');
    b = b ? b + 1 : path;
    snprintf(out, cap, "%s", b);
    char *dot = strstr(out, ".ko");
    if (dot) *dot = 0;
    for (char *p = out; *p; p++) if (*p == '-') *p = '_';   /* kernel normalises */
}
/* ---- the module index ----------------------------------------------------
 * Lookups used to re-walk the whole tree each time, which made depmod
 * O(modules^2): ~1300 modules times ~1300 files per dependency is millions of
 * stat() calls, and it took minutes. Walk once, keep the names in memory. */
struct modent { char name[80]; char rel[256]; };
static struct modent *index_ents;
static int index_n, index_cap;

static void index_add(const char *path, size_t striplen) {
    if (index_n == index_cap) {
        index_cap = index_cap ? index_cap * 2 : 512;
        struct modent *g = realloc(index_ents, (size_t)index_cap * sizeof *g);
        if (!g) { perror("verkmod"); exit(1); }
        index_ents = g;
    }
    struct modent *m = &index_ents[index_n++];
    basename_noext(path, m->name, sizeof m->name);
    snprintf(m->rel, sizeof m->rel, "%s", path + striplen);
}
static void index_walk(const char *dir, size_t striplen) {
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        /* "build" and "source" are symlinks into the kernel *build* tree, which
         * holds a second copy of every .ko. Indexing them doubles the index and
         * makes dependencies resolve to the build tree instead of the installed
         * module. lstat, not stat: never follow a symlink out of the tree. */
        if (!strcmp(e->d_name, "build") || !strcmp(e->d_name, "source")) continue;
        char path[1024];
        snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
        struct stat st;
        if (lstat(path, &st)) continue;
        if (S_ISLNK(st.st_mode)) continue;
        if (S_ISDIR(st.st_mode)) index_walk(path, striplen);
        else if (strstr(e->d_name, ".ko")) index_add(path, striplen);
    }
    closedir(d);
}
static void index_build(void) {
    if (index_ents) return;
    index_walk(moddir, strlen(moddir) + 1);
}
static const char *index_rel(const char *name) {
    index_build();
    /* The kernel treats - and _ as the same character in module names, and
     * modinfo's depends= uses whichever the author wrote ("tps6594-core" for
     * a file named tps6594_core.ko). Normalise the query like the entries. */
    char q[80];
    snprintf(q, sizeof q, "%s", name);
    for (char *c = q; *c; c++) if (*c == '-') *c = '_';
    for (int i = 0; i < index_n; i++)
        if (!strcmp(index_ents[i].name, q)) return index_ents[i].rel;
    return NULL;
}
/* absolute path of module <name>, 0 on success (mirrors the old find_module) */
static int find_module(const char *name, char *out, size_t cap) {
    const char *rel = index_rel(name);
    if (!rel) return -1;
    snprintf(out, cap, "%s/%s", moddir, rel);
    return 0;
}

/* Modules compiled in rather than built as .ko are listed in modules.builtin;
 * asking for one is a no-op, not a failure. */
static int is_builtin(const char *name) {
    char p[1024];
    snprintf(p, sizeof p, "%s/modules.builtin", moddir);
    FILE *f = fopen(p, "r");
    if (!f) return 0;
    char line[512]; int found = 0;
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\n")] = 0;
        char base[128];
        basename_noext(line, base, sizeof base);
        if (!strcmp(base, name)) { found = 1; break; }
    }
    fclose(f);
    return found;
}

/* ---- loading -------------------------------------------------------------- */
static int load_file(const char *path, const char *params, int quiet_if_loaded) {
    char name[128];
    basename_noext(path, name, sizeof name);
    if (is_loaded(name)) {
        if (!quiet_if_loaded && verbose) fprintf(stderr, "verkmod: %s is already loaded\n", name);
        return 0;
    }
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) { fprintf(stderr, "verkmod: %s: %s\n", path, strerror(errno)); return 1; }
    int rc = sys_finit_module(fd, params ? params : "", 0);
    close(fd);
    if (rc) {
        /* EEXIST means someone won the race; that is success for our purposes */
        if (errno == EEXIST) return 0;
        fprintf(stderr, "verkmod: loading %s failed: %s\n", path, strerror(errno));
        return 1;
    }
    if (verbose) fprintf(stderr, "verkmod: loaded %s\n", name);
    return 0;
}

/* ---- dependencies --------------------------------------------------------- */
/* Load everything <path> depends on, depth first. Cycles are not possible in
 * module deps, but keep a depth cap rather than trusting that. */
static int load_deps(const char *path, int depth) {
    if (depth > 16) return 0;
    size_t len = 0;
    char *info = read_modinfo(path, &len);
    if (!info) return 0;
    char deps[512] = "";
    modinfo_get(info, len, "depends", deps, sizeof deps);
    free(info);
    if (!deps[0]) return 0;

    int rc = 0;
    char buf[512]; snprintf(buf, sizeof buf, "%s", deps);
    /* strtok_r, not strtok: this function recurses, and a nested call would
     * reset the shared strtok state so the outer loop stops at the first
     * dependency that has dependencies of its own — silently skipping the
     * rest, after which the module fails to load on an unresolved symbol. */
    char *save = NULL;
    for (char *tok = strtok_r(buf, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
        for (char *c = tok; *c; c++) if (*c == '-') *c = '_';
        if (!*tok || is_loaded(tok)) continue;
        char dpath[1024];
        if (find_module(tok, dpath, sizeof dpath)) {
            fprintf(stderr, "verkmod: dependency %s not found\n", tok);
            rc = 1; continue;
        }
        rc |= load_deps(dpath, depth + 1);
        rc |= load_file(dpath, "", 1);
    }
    return rc;
}

static int cmd_modprobe(int argc, char **argv) {
    int remove = 0, i = 1;
    for (; i < argc && argv[i][0] == '-'; i++) {
        if (!strcmp(argv[i], "-r")) remove = 1;
        else if (!strcmp(argv[i], "-v")) verbose = 1;
        else { fprintf(stderr, "modprobe: unknown option %s\n", argv[i]); return 1; }
    }
    if (i >= argc) { fprintf(stderr, "usage: modprobe [-r] [-v] NAME [param=value ...]\n"); return 1; }
    char name[128]; snprintf(name, sizeof name, "%s", argv[i++]);
    for (char *p = name; *p; p++) if (*p == '-') *p = '_';

    if (remove) {
        if (sys_delete_module(name, O_NONBLOCK)) {
            fprintf(stderr, "modprobe: removing %s failed: %s\n", name, strerror(errno));
            return 1;
        }
        return 0;
    }
    if (is_loaded(name)) return 0;

    char path[1024];
    if (find_module(name, path, sizeof path)) {
        if (is_builtin(name)) {
            if (verbose) fprintf(stderr, "modprobe: %s is built into the kernel\n", name);
            return 0;                    /* already there — nothing to do */
        }
        fprintf(stderr, "modprobe: module %s not found in %s\n", name, moddir);
        return 1;
    }
    /* remaining arguments are module parameters */
    char params[512] = "";
    for (; i < argc; i++) {
        if (params[0]) strncat(params, " ", sizeof params - strlen(params) - 1);
        strncat(params, argv[i], sizeof params - strlen(params) - 1);
    }
    if (load_deps(path, 0)) return 1;
    return load_file(path, params, 0);
}

static int cmd_insmod(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: insmod FILE [param=value ...]\n"); return 1; }
    char params[512] = "";
    for (int i = 2; i < argc; i++) {
        if (params[0]) strncat(params, " ", sizeof params - strlen(params) - 1);
        strncat(params, argv[i], sizeof params - strlen(params) - 1);
    }
    return load_file(argv[1], params, 0);
}
static int cmd_rmmod(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: rmmod NAME...\n"); return 1; }
    int rc = 0;
    for (int i = 1; i < argc; i++) {
        if (argv[i][0] == '-') continue;
        char name[128]; snprintf(name, sizeof name, "%s", argv[i]);
        char *dot = strstr(name, ".ko"); if (dot) *dot = 0;
        for (char *p = name; *p; p++) if (*p == '-') *p = '_';
        if (sys_delete_module(name, O_NONBLOCK)) {
            fprintf(stderr, "rmmod: %s: %s\n", name, strerror(errno));
            rc = 1;
        }
    }
    return rc;
}
static int cmd_modinfo(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: modinfo NAME|FILE\n"); return 1; }
    char path[1024];
    if (strstr(argv[1], ".ko") && !access(argv[1], R_OK)) snprintf(path, sizeof path, "%s", argv[1]);
    else if (find_module(argv[1], path, sizeof path)) {
        fprintf(stderr, "modinfo: module %s not found\n", argv[1]); return 1;
    }
    size_t len = 0;
    char *info = read_modinfo(path, &len);
    if (!info) { fprintf(stderr, "modinfo: %s has no .modinfo\n", path); return 1; }
    printf("%-16s %s\n", "filename:", path);
    for (size_t i = 0; i < len; ) {
        const char *e = info + i;
        size_t elen = strlen(e);
        if (elen) {
            const char *eq = strchr(e, '=');
            if (eq) {
                char key[32];
                int kl = (int)(eq - e); if (kl > 30) kl = 30;
                snprintf(key, sizeof key, "%.*s:", kl, e);   /* modinfo prints key: value */
                printf("%-16s %s\n", key, eq + 1);
            }
        }
        i += elen + 1;
        if (!elen) i++;
    }
    free(info);
    return 0;
}

/* ---- depmod: write the index kmod's depmod used to write -----------------
 * Note the division of labour: modules.dep exists because other software and
 * people expect it, but modprobe resolves dependencies from each module's own
 * .modinfo, so a stale index can never make the wrong module load. */
static int write_deps(FILE *out) {
    index_build();
    int n = 0;
    for (int i = 0; i < index_n; i++) {
        char path[1024];
        snprintf(path, sizeof path, "%s/%s", moddir, index_ents[i].rel);
        size_t len = 0;
        char *info = read_modinfo(path, &len);
        char deps[512] = "";
        if (info) { modinfo_get(info, len, "depends", deps, sizeof deps); free(info); }
        fprintf(out, "%s:", index_ents[i].rel);
        char *save = NULL;
        for (char *tok = strtok_r(deps, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
            if (!*tok) continue;
            const char *rel = index_rel(tok);
            if (rel) fprintf(out, " %s", rel);
            else fprintf(stderr, "depmod: %s needs %s, which is not installed\n",
                         index_ents[i].name, tok);
        }
        fputc('\n', out);
        n++;
    }
    return n;
}
static int cmd_depmod(int argc, char **argv) {
    const char *basedir = "", *release = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-b") && i + 1 < argc) basedir = argv[++i];
        else if (!strcmp(argv[i], "-v") || !strcmp(argv[i], "-a") || !strcmp(argv[i], "-A")) continue;
        else if (argv[i][0] == '-') { fprintf(stderr, "depmod: unknown option %s\n", argv[i]); return 1; }
        else release = argv[i];
    }
    if (basedir[0] || release) {
        struct utsname u;
        if (!release) { if (uname(&u)) { perror("depmod: uname"); return 1; } release = u.release; }
        snprintf(moddir, sizeof moddir, "%s" MODDIR_BASE "/%s", basedir, release);
    }
    char out[1024];
    snprintf(out, sizeof out, "%s/modules.dep", moddir);
    FILE *f = fopen(out, "w");
    if (!f) { fprintf(stderr, "depmod: %s: %s\n", out, strerror(errno)); return 1; }
    int n = write_deps(f);
    fclose(f);
    if (verbose) fprintf(stderr, "depmod: indexed %d modules into %s\n", n, out);
    return 0;
}

static void usage(void) {
    printf("verkmod — VerkOS' kernel module tools (call it by any of these names)\n\n"
           "  insmod  FILE [param=value ...]   load exactly this file\n"
           "  rmmod   NAME...                  unload\n"
           "  lsmod                            list loaded modules\n"
           "  modinfo NAME|FILE                show a module's metadata\n"
           "  modprobe [-r] [-v] NAME [p=v]    load (or -r remove) with dependencies\n"
           "  depmod  [-b BASEDIR] [RELEASE]   rebuild modules.dep\n");
}

int main(int argc, char **argv) {
    const char *me = strrchr(argv[0], '/');
    me = me ? me + 1 : argv[0];
    find_moddir();
    for (int i = 1; i < argc; i++) if (!strcmp(argv[i], "-v")) verbose = 1;

    if (!strcmp(me, "insmod"))   return cmd_insmod(argc, argv);
    if (!strcmp(me, "rmmod"))    return cmd_rmmod(argc, argv);
    if (!strcmp(me, "lsmod"))    return cmd_lsmod();
    if (!strcmp(me, "modinfo"))  return cmd_modinfo(argc, argv);
    if (!strcmp(me, "modprobe")) return cmd_modprobe(argc, argv);
    if (!strcmp(me, "depmod"))   return cmd_depmod(argc, argv);

    /* called as verkmod: first argument selects the tool */
    if (argc > 1) {
        char *sub = argv[1];
        argv[1] = argv[0];
        if (!strcmp(sub, "insmod"))   return cmd_insmod(argc - 1, argv + 1);
        if (!strcmp(sub, "rmmod"))    return cmd_rmmod(argc - 1, argv + 1);
        if (!strcmp(sub, "lsmod"))    return cmd_lsmod();
        if (!strcmp(sub, "modinfo"))  return cmd_modinfo(argc - 1, argv + 1);
        if (!strcmp(sub, "modprobe")) return cmd_modprobe(argc - 1, argv + 1);
        if (!strcmp(sub, "depmod"))   return cmd_depmod(argc - 1, argv + 1);
    }
    usage();
    return 1;
}
