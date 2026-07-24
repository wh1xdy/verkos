/* vsys — VerkOS' own system tools (a multi-call sysadmin binary).
 *
 * Dispatches on argv[0]: df, dmesg, pgrep, pkill, killall, pidof, watch. Reads
 * /proc + statvfs + the kernel log. Installed under those familiar names.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <dirent.h>
#include <ctype.h>
#include <time.h>
#include <sys/statvfs.h>
#include <sys/klog.h>
#include <sys/wait.h>

/* ---- df ----------------------------------------------------------------- */
static void human(unsigned long long kb, char *out) {
    double v = kb; const char *u = "K";
    if (v >= 1e9) { v /= 1e9; u = "T"; } else if (v >= 1e6) { v /= 1e6; u = "G"; } else if (v >= 1e3) { v /= 1e3; u = "M"; }
    sprintf(out, "%.1f%s", v, u);
}
static int cmd_df(int argc, char **argv) {
    int hflag = 0;
    for (int i = 1; i < argc; i++) if (!strcmp(argv[i], "-h")) hflag = 1;
    FILE *f = fopen("/proc/mounts", "r"); if (!f) { perror("df"); return 1; }
    printf("%-24s %10s %10s %10s %5s %s\n", "Filesystem",
           hflag ? "Size" : "1K-blocks", "Used", "Avail", "Use%", "Mounted on");
    char dev[256], mnt[256], fstype[64], line[1024];
    while (fgets(line, sizeof line, f)) {
        if (sscanf(line, "%255s %255s %63s", dev, mnt, fstype) != 3) continue;
        if (!strcmp(fstype, "proc") || !strcmp(fstype, "sysfs") || !strcmp(fstype, "devtmpfs") ||
            !strcmp(fstype, "cgroup") || !strcmp(fstype, "cgroup2") || !strcmp(fstype, "devpts") ||
            !strcmp(fstype, "mqueue") || !strcmp(fstype, "debugfs") || !strcmp(fstype, "tracefs") ||
            !strcmp(fstype, "pstore") || !strcmp(fstype, "bpf") || !strcmp(fstype, "securityfs") ||
            !strcmp(fstype, "configfs") || !strcmp(fstype, "fusectl") || !strcmp(fstype, "hugetlbfs") ||
            !strcmp(fstype, "autofs") || !strcmp(fstype, "efivarfs") || !strcmp(fstype, "ramfs")) continue;
        struct statvfs vfs;
        if (statvfs(mnt, &vfs) != 0) continue;
        unsigned long long bs = vfs.f_frsize;
        unsigned long long total = vfs.f_blocks * bs / 1024;
        unsigned long long avail = vfs.f_bavail * bs / 1024;
        unsigned long long used = (vfs.f_blocks - vfs.f_bfree) * bs / 1024;
        if (total == 0) continue;
        int pct = (int)((vfs.f_blocks - vfs.f_bfree) * 100 / (vfs.f_blocks ? vfs.f_blocks : 1));
        if (hflag) { char a[16], b[16], c[16]; human(total, a); human(used, b); human(avail, c);
            printf("%-24s %10s %10s %10s %4d%% %s\n", dev, a, b, c, pct, mnt); }
        else printf("%-24s %10llu %10llu %10llu %4d%% %s\n", dev, total, used, avail, pct, mnt);
    }
    fclose(f);
    return 0;
}

/* ---- dmesg -------------------------------------------------------------- */
static int cmd_dmesg(int argc, char **argv) {
    (void)argc; (void)argv;
    int len = klogctl(10, NULL, 0);              /* SYSLOG_ACTION_SIZE_BUFFER */
    if (len < 4096) len = 1 << 18;
    char *buf = malloc(len + 1);
    int n = klogctl(3, buf, len);                /* SYSLOG_ACTION_READ_ALL */
    if (n < 0) { perror("dmesg"); free(buf); return 1; }
    buf[n] = 0;
    for (char *p = buf; *p; ) {
        char *nl = strchr(p, '\n'); if (nl) *nl = 0;
        char *msg = p;
        if (*msg == '<') { char *gt = strchr(msg, '>'); if (gt) msg = gt + 1; }   /* strip <level> */
        printf("%s\n", msg);
        if (!nl) break;
        p = nl + 1;
    }
    free(buf);
    return 0;
}

/* ---- pgrep / pkill / killall / pidof ------------------------------------ */
static int read_comm(int pid, char *comm, size_t n) {
    char path[64]; snprintf(path, sizeof path, "/proc/%d/comm", pid);
    FILE *f = fopen(path, "r"); if (!f) return -1;
    if (!fgets(comm, n, f)) { fclose(f); return -1; }
    comm[strcspn(comm, "\n")] = 0; fclose(f); return 0;
}
static int read_cmdline(int pid, char *out, size_t n) {
    char path[64]; snprintf(path, sizeof path, "/proc/%d/cmdline", pid);
    FILE *f = fopen(path, "r"); if (!f) return -1;
    size_t r = fread(out, 1, n - 1, f); fclose(f);
    for (size_t i = 0; i < r; i++) if (out[i] == 0) out[i] = ' ';
    out[r] = 0; return 0;
}
/* mode: 0 pgrep, 1 pkill, 2 killall (exact), 3 pidof (exact, one line) */
static int proc_match(int mode, int argc, char **argv) {
    int sig = SIGTERM, exact = (mode >= 2), full = 0, lst = 0;
    const char *pat = NULL;
    for (int i = 1; i < argc; i++) {
        if (argv[i][0] == '-' && isdigit((unsigned char)argv[i][1])) sig = atoi(argv[i] + 1);
        else if (argv[i][0] == '-' && argv[i][1] == 'S' && argv[i][2] == 'I') { /* -SIGxxx: leave TERM */ }
        else if (!strcmp(argv[i], "-x")) exact = 1;
        else if (!strcmp(argv[i], "-f")) full = 1;
        else if (!strcmp(argv[i], "-l")) lst = 1;
        else if (argv[i][0] != '-') pat = argv[i];
    }
    if (!pat) { fprintf(stderr, "usage: %s [-signal] [-x] [-f] NAME\n", argv[0]); return 1; }
    DIR *d = opendir("/proc"); if (!d) return 1;
    struct dirent *e; int hits = 0; int me = getpid();
    while ((e = readdir(d))) {
        if (e->d_name[0] < '1' || e->d_name[0] > '9') continue;
        int pid = atoi(e->d_name); if (pid == me) continue;
        char comm[256], cmd[4096]; if (read_comm(pid, comm, sizeof comm) != 0) continue;
        int m;
        if (full && read_cmdline(pid, cmd, sizeof cmd) == 0) m = strstr(cmd, pat) != NULL;
        else m = exact ? !strcmp(comm, pat) : (strstr(comm, pat) != NULL);
        if (!m) continue;
        hits++;
        if (mode == 0) { if (lst) printf("%d %s\n", pid, comm); else printf("%d\n", pid); }
        else if (mode == 3) printf("%s%d", hits > 1 ? " " : "", pid);
        else { if (kill(pid, sig) == 0 && mode == 2) {} }
    }
    closedir(d);
    if (mode == 3 && hits) printf("\n");
    return hits ? 0 : 1;
}

/* ---- watch -------------------------------------------------------------- */
static int cmd_watch(int argc, char **argv) {
    int interval = 2, i = 1;
    for (; i < argc; i++) {
        if (!strcmp(argv[i], "-n") && i + 1 < argc) interval = atoi(argv[++i]);
        else if (!strncmp(argv[i], "-n", 2)) interval = atoi(argv[i] + 2);
        else break;
    }
    if (i >= argc) { fprintf(stderr, "usage: watch [-n SEC] CMD...\n"); return 1; }
    char cmdline[4096] = ""; for (int k = i; k < argc; k++) { strcat(cmdline, argv[k]); if (k + 1 < argc) strcat(cmdline, " "); }
    for (;;) {
        printf("\033[H\033[2JEvery %ds: %s\n\n", interval, cmdline);
        fflush(stdout);
        pid_t p = fork();
        if (p == 0) { execl("/bin/sh", "sh", "-c", cmdline, (char *)NULL); _exit(127); }
        if (p > 0) waitpid(p, NULL, 0);
        sleep(interval > 0 ? interval : 2);
    }
    return 0;
}

int main(int argc, char **argv) {
    const char *me = strrchr(argv[0], '/'); me = me ? me + 1 : argv[0];
    if (!strcmp(me, "df")) return cmd_df(argc, argv);
    if (!strcmp(me, "dmesg")) return cmd_dmesg(argc, argv);
    if (!strcmp(me, "pgrep")) return proc_match(0, argc, argv);
    if (!strcmp(me, "pkill")) return proc_match(1, argc, argv);
    if (!strcmp(me, "killall")) return proc_match(2, argc, argv);
    if (!strcmp(me, "pidof")) return proc_match(3, argc, argv);
    if (!strcmp(me, "watch")) return cmd_watch(argc, argv);
    if (argc > 1) {
        char *s = argv[1];
        if (!strcmp(s, "df")) return cmd_df(argc - 1, argv + 1);
        if (!strcmp(s, "dmesg")) return cmd_dmesg(argc - 1, argv + 1);
        if (!strcmp(s, "pgrep")) return proc_match(0, argc - 1, argv + 1);
        if (!strcmp(s, "pkill")) return proc_match(1, argc - 1, argv + 1);
        if (!strcmp(s, "killall")) return proc_match(2, argc - 1, argv + 1);
        if (!strcmp(s, "pidof")) return proc_match(3, argc - 1, argv + 1);
        if (!strcmp(s, "watch")) return cmd_watch(argc - 1, argv + 1);
    }
    fprintf(stderr, "vsys: link as df/dmesg/pgrep/pkill/killall/pidof/watch\n");
    return 1;
}
