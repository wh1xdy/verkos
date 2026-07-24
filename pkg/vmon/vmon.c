/* vmon — VerkOS' own process/system monitors (a procps in one binary).
 *
 * BusyBox/verkbox-style multi-call: dispatches on argv[0] to free, uptime,
 * vmstat, ps, or top, each reading /proc. Installed under those familiar names.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <ctype.h>
#include <time.h>
#include <termios.h>
#include <sys/select.h>
#include <sys/ioctl.h>

static long HZ_;

/* ---- small helpers ------------------------------------------------------ */
static long meminfo(const char *key) {          /* value in kB, or -1 */
    FILE *f = fopen("/proc/meminfo", "r"); if (!f) return -1;
    char line[128]; long v = -1; size_t kl = strlen(key);
    while (fgets(line, sizeof line, f))
        if (!strncmp(line, key, kl) && line[kl] == ':') { v = atol(line + kl + 1); break; }
    fclose(f); return v;
}
static double uptime_secs(void) {
    FILE *f = fopen("/proc/uptime", "r"); if (!f) return 0;
    double u = 0; if (fscanf(f, "%lf", &u) != 1) u = 0; fclose(f); return u;
}
static void loadavg(char *out, size_t n) {
    FILE *f = fopen("/proc/loadavg", "r"); if (!f) { snprintf(out, n, "?"); return; }
    double a = 0, b = 0, c = 0; if (fscanf(f, "%lf %lf %lf", &a, &b, &c) != 3) {}
    fclose(f); snprintf(out, n, "%.2f, %.2f, %.2f", a, b, c);
}
static void fmt_dur(double s, char *out, size_t n) {
    long t = (long)s, d = t / 86400, h = (t % 86400) / 3600, m = (t % 3600) / 60;
    if (d) snprintf(out, n, "%ld day%s, %2ld:%02ld", d, d == 1 ? "" : "s", h, m);
    else if (h) snprintf(out, n, "%ld:%02ld", h, m);
    else snprintf(out, n, "%ld min", m);
}

/* ---- free --------------------------------------------------------------- */
static int cmd_free(int argc, char **argv) {
    long div = 1; const char *unit = ""; int human = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-m")) { div = 1024; unit = ""; }
        else if (!strcmp(argv[i], "-g")) { div = 1024 * 1024; }
        else if (!strcmp(argv[i], "-k")) div = 1;
        else if (!strcmp(argv[i], "-h")) human = 1;
    }
    long total = meminfo("MemTotal"), freem = meminfo("MemFree"), avail = meminfo("MemAvailable");
    long buffers = meminfo("Buffers"), cached = meminfo("Cached"), srecl = meminfo("SReclaimable");
    long shmem = meminfo("Shmem"), stot = meminfo("SwapTotal"), sfree = meminfo("SwapFree");
    if (avail < 0) avail = freem;
    long bc = buffers + cached + (srecl > 0 ? srecl : 0);
    long used = total - freem - bc;
    (void)unit;
    printf("%15s %11s %11s %11s %11s %11s %11s\n", "", "total", "used", "free", "shared", "buff/cache", "available");
    if (human) {
        char a[8], b[8], c[8], d[8], e[8], g[8];
        #define H(x, buf) do { double v = (x) * 1024.0; const char*u="B"; \
            if (v>=1e9){v/=1e9;u="G";} else if (v>=1e6){v/=1e6;u="M";} else if(v>=1e3){v/=1e3;u="K";} \
            snprintf(buf,8,"%.1f%s",v,u); } while(0)
        H(total,a); H(used,b); H(freem,c); H(shmem>0?shmem:0,d); H(bc,e); H(avail,g);
        printf("%-15s %11s %11s %11s %11s %11s %11s\n", "Mem:", a, b, c, d, e, g);
        char sa[8], sb[8], sc[8]; H(stot>0?stot:0,sa); H(stot>0?stot-sfree:0,sb); H(sfree>0?sfree:0,sc);
        printf("%-15s %11s %11s %11s\n", "Swap:", sa, sb, sc);
        #undef H
    } else {
        printf("%-15s %11ld %11ld %11ld %11ld %11ld %11ld\n", "Mem:",
               total/div, used/div, freem/div, (shmem>0?shmem:0)/div, bc/div, avail/div);
        printf("%-15s %11ld %11ld %11ld\n", "Swap:",
               (stot>0?stot:0)/div, (stot>0?stot-sfree:0)/div, (sfree>0?sfree:0)/div);
    }
    return 0;
}

/* ---- uptime ------------------------------------------------------------- */
static int cmd_uptime(int argc, char **argv) {
    (void)argc; (void)argv;
    time_t now = time(NULL); struct tm *tm = localtime(&now);
    char dur[64], load[64]; fmt_dur(uptime_secs(), dur, sizeof dur); loadavg(load, sizeof load);
    printf(" %02d:%02d:%02d up %s,  load average: %s\n", tm->tm_hour, tm->tm_min, tm->tm_sec, dur, load);
    return 0;
}

/* ---- process table (shared by ps and top) ------------------------------- */
struct proc { int pid; char comm[64]; char state; long rss; unsigned long long jiff; char user[64]; };

static void uid_name(int uid, char *out, size_t n) {
    FILE *f = fopen("/etc/passwd", "r");
    if (f) { char line[256]; while (fgets(line, sizeof line, f)) {
        char nm[64]; int u; if (sscanf(line, "%63[^:]:%*[^:]:%d:", nm, &u) == 2 && u == uid) { snprintf(out, n, "%s", nm); fclose(f); return; } }
        fclose(f); }
    snprintf(out, n, "%d", uid);
}
/* read one /proc/<pid>; return 0 on success */
static int read_proc(int pid, struct proc *p) {
    char path[64]; snprintf(path, sizeof path, "/proc/%d/stat", pid);
    FILE *f = fopen(path, "r"); if (!f) return -1;
    char buf[1024]; if (!fgets(buf, sizeof buf, f)) { fclose(f); return -1; } fclose(f);
    char *o = strrchr(buf, ')'); if (!o) return -1;
    char *lp = strchr(buf, '(');
    p->pid = pid;
    int cl = (int)(o - lp - 1); if (cl > 63) cl = 63; memcpy(p->comm, lp + 1, cl); p->comm[cl] = 0;
    unsigned long ut = 0, st = 0; long rss = 0; char state = '?';
    /* after ") ": state ppid pgrp session tty tpgid flags min flt cmin maj cmaj utime stime ... rss(24th field overall) */
    sscanf(o + 2, "%c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %lu %lu %*d %*d %*d %*d %*d %*d %*u %*u %ld",
           &state, &ut, &st, &rss);
    p->state = state; p->jiff = (unsigned long long)ut + st; p->rss = rss * (sysconf(_SC_PAGESIZE) / 1024);
    snprintf(path, sizeof path, "/proc/%d/status", pid);
    p->user[0] = 0;
    f = fopen(path, "r");
    if (f) { char line[128]; while (fgets(line, sizeof line, f)) { int uid; if (sscanf(line, "Uid:\t%d", &uid) == 1) { uid_name(uid, p->user, sizeof p->user); break; } } fclose(f); }
    if (!p->user[0]) snprintf(p->user, sizeof p->user, "?");
    return 0;
}
static int collect(struct proc *ps, int max) {
    DIR *d = opendir("/proc"); if (!d) return 0;
    struct dirent *e; int n = 0;
    while ((e = readdir(d)) && n < max) {
        if (e->d_name[0] < '1' || e->d_name[0] > '9') continue;
        if (read_proc(atoi(e->d_name), &ps[n]) == 0) n++;
    }
    closedir(d);
    return n;
}

/* ---- ps ----------------------------------------------------------------- */
static int cmd_ps(int argc, char **argv) {
    int detailed = 0;
    for (int i = 1; i < argc; i++)
        if (strchr(argv[i], 'a') || strchr(argv[i], 'u') || strchr(argv[i], 'e') || strchr(argv[i], 'x')) detailed = 1;
    struct proc ps[4096]; int n = collect(ps, 4096);
    double up = uptime_secs(); (void)up;
    if (detailed) {
        printf("%-9s %6s %5s %8s %c   %s\n", "USER", "PID", "STAT", "TIME", 'S', "COMMAND");
        for (int i = 0; i < n; i++) {
            long secs = ps[i].jiff / (HZ_ ? HZ_ : 100);
            printf("%-9s %6d %5c %3ld:%02ld %c   %s\n", ps[i].user, ps[i].pid, ps[i].state,
                   secs / 60, secs % 60, ps[i].state, ps[i].comm);
        }
    } else {
        printf("%6s %-5s %8s %s\n", "PID", "STAT", "TIME", "CMD");
        for (int i = 0; i < n; i++) {
            long secs = ps[i].jiff / (HZ_ ? HZ_ : 100);
            printf("%6d %-5c %3ld:%02ld %s\n", ps[i].pid, ps[i].state, secs / 60, secs % 60, ps[i].comm);
        }
    }
    return 0;
}

/* ---- vmstat ------------------------------------------------------------- */
static void cpu_line(unsigned long long v[10]) {
    FILE *f = fopen("/proc/stat", "r"); if (!f) { memset(v, 0, 80); return; }
    char line[256];
    while (fgets(line, sizeof line, f)) if (!strncmp(line, "cpu ", 4)) {
        sscanf(line + 4, "%llu %llu %llu %llu %llu %llu %llu %llu",
               &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7]); break; }
    fclose(f);
}
static unsigned long long procstat_val(const char *key) {
    FILE *f = fopen("/proc/stat", "r"); if (!f) return 0;
    char line[256]; unsigned long long v = 0; size_t kl = strlen(key);
    while (fgets(line, sizeof line, f)) if (!strncmp(line, key, kl)) { v = strtoull(line + kl, NULL, 10); break; }
    fclose(f); return v;
}
static int cmd_vmstat(int argc, char **argv) {
    int delay = argc > 1 ? atoi(argv[1]) : 0, count = argc > 2 ? atoi(argv[2]) : (delay ? 0 : 1);
    printf("procs -----------memory---------- ---swap-- -----io---- -system-- ------cpu-----\n");
    printf(" r  b   swpd   free   buff  cache   si   so    bi    bo   in   cs us sy id wa st\n");
    unsigned long long pc[10] = {0}, ctxt0 = 0, intr0 = 0; int first = 1;
    for (int k = 0; count == 0 || k < count; k++) {
        unsigned long long c[10]; cpu_line(c);
        unsigned long long ctxt = procstat_val("ctxt "), intr = procstat_val("intr ");
        unsigned long long tot = 0, dt; for (int i = 0; i < 8; i++) tot += c[i];
        unsigned long long ptot = 0; for (int i = 0; i < 8; i++) ptot += pc[i];
        dt = tot - ptot; if (!dt) dt = 1;
        int us = (int)(100 * ((c[0]+c[1]) - (pc[0]+pc[1])) / dt);
        int sy = (int)(100 * (c[2] - pc[2]) / dt);
        int id = (int)(100 * (c[3] - pc[3]) / dt);
        int wa = (int)(100 * (c[4] - pc[4]) / dt);
        if (first) { us = sy = wa = 0; id = 100; }
        long r = procstat_val("procs_running "), b = procstat_val("procs_blocked ");
        printf("%2ld %2ld %6d %6ld %6ld %6ld %4d %4d %5d %5d %4llu %4llu %2d %2d %2d %2d  0\n",
               r, b, 0, meminfo("MemFree"), meminfo("Buffers"), meminfo("Cached"), 0, 0, 0, 0,
               first ? intr : intr - intr0, first ? ctxt : ctxt - ctxt0, us, sy, id, wa);
        memcpy(pc, c, sizeof pc); ctxt0 = ctxt; intr0 = intr; first = 0;
        if (count && k + 1 >= count) break;
        sleep(delay > 0 ? delay : 1);
    }
    return 0;
}

/* ---- top ---------------------------------------------------------------- */
static int getkey_nb(void) {                    /* nonblocking single char, -1 if none */
    fd_set r; FD_ZERO(&r); FD_SET(0, &r);
    struct timeval tv = {0, 0};
    if (select(1, &r, NULL, NULL, &tv) > 0) { char c; if (read(0, &c, 1) == 1) return c; }
    return -1;
}
static int by_cpu(const void *a, const void *b) {
    double x = ((const double *)a)[1], y = ((const double *)b)[1];
    return x < y ? 1 : x > y ? -1 : 0;
}
static int cmd_top(int argc, char **argv) {
    int batch = 0, iters = -1, delay = 2;
    for (int i = 1; i < argc; i++) {               /* handle -b, -n N, -bn1, -d2, ... */
        if (argv[i][0] != '-') continue;
        for (char *p = argv[i] + 1; *p; p++) {
            if (*p == 'b') batch = 1;
            else if (*p == 'n') { iters = atoi(p[1] ? p + 1 : (i + 1 < argc ? argv[++i] : "0")); break; }
            else if (*p == 'd') { delay = atoi(p[1] ? p + 1 : (i + 1 < argc ? argv[++i] : "2")); break; }
        }
    }
    struct termios old, raw; int israw = 0;
    if (!batch && isatty(0)) { tcgetattr(0, &old); raw = old; raw.c_lflag &= ~(ICANON | ECHO); tcsetattr(0, TCSANOW, &raw); israw = 1; }

    static struct proc a[4096], b[4096]; static unsigned long long prev[65536];
    int na = collect(a, 4096);
    for (int i = 0; i < na; i++) if (a[i].pid < 65536) prev[a[i].pid] = a[i].jiff;
    unsigned long long ptot = 0; { unsigned long long c[10]; cpu_line(c); for (int i = 0; i < 8; i++) ptot += c[i]; }

    for (int it = 0; iters < 0 || it < iters; it++) {
        sleep(delay);
        int nb = collect(b, 4096);
        unsigned long long c[10]; cpu_line(c); unsigned long long tot = 0; for (int i = 0; i < 8; i++) tot += c[i];
        unsigned long long dtot = tot - ptot; if (!dtot) dtot = 1;
        static double rows[4096][3];   /* pid, %cpu, index */
        for (int i = 0; i < nb; i++) {
            unsigned long long pj = b[i].pid < 65536 ? prev[b[i].pid] : 0;
            double pct = 100.0 * (double)(b[i].jiff - pj) / (double)dtot * 8.0;   /* approx, per total */
            if (pct < 0) pct = 0;
            rows[i][0] = b[i].pid; rows[i][1] = pct; rows[i][2] = i;
        }
        qsort(rows, nb, sizeof rows[0], by_cpu);

        char up[64], load[64]; fmt_dur(uptime_secs(), up, sizeof up); loadavg(load, sizeof load);
        long mt = meminfo("MemTotal"), mf = meminfo("MemFree"), ma = meminfo("MemAvailable");
        if (!batch) printf("\033[H\033[2J");
        printf("top - up %s,  load average: %s\n", up, load);
        printf("Tasks: %d total\n", nb);
        printf("MiB Mem : %8.1f total, %8.1f free, %8.1f used, %8.1f avail\n",
               mt/1024.0, mf/1024.0, (mt-mf)/1024.0, (ma<0?mf:ma)/1024.0);
        printf("\n%6s %-9s %-4s %6s %8s  %s\n", "PID", "USER", "STAT", "%CPU", "RES(kB)", "COMMAND");
        int show = nb < 20 ? nb : 20;
        for (int r = 0; r < show; r++) {
            int i = (int)rows[r][2];
            printf("%6d %-9s %-4c %5.1f %8ld  %s\n", b[i].pid, b[i].user, b[i].state, rows[r][1], b[i].rss, b[i].comm);
        }
        fflush(stdout);
        for (int i = 0; i < nb; i++) if (b[i].pid < 65536) prev[b[i].pid] = b[i].jiff;
        ptot = tot;
        if (israw) { int k = getkey_nb(); if (k == 'q') break; }
        if (iters > 0 && it + 1 >= iters) break;
    }
    if (israw) tcsetattr(0, TCSANOW, &old);
    return 0;
}

int main(int argc, char **argv) {
    HZ_ = sysconf(_SC_CLK_TCK);
    const char *me = strrchr(argv[0], '/'); me = me ? me + 1 : argv[0];
    if (!strcmp(me, "free")) return cmd_free(argc, argv);
    if (!strcmp(me, "uptime")) return cmd_uptime(argc, argv);
    if (!strcmp(me, "vmstat")) return cmd_vmstat(argc, argv);
    if (!strcmp(me, "ps")) return cmd_ps(argc, argv);
    if (!strcmp(me, "top")) return cmd_top(argc, argv);
    /* invoked as vmon: first arg selects */
    if (argc > 1) {
        char *sub = argv[1];
        if (!strcmp(sub, "free")) return cmd_free(argc - 1, argv + 1);
        if (!strcmp(sub, "uptime")) return cmd_uptime(argc - 1, argv + 1);
        if (!strcmp(sub, "vmstat")) return cmd_vmstat(argc - 1, argv + 1);
        if (!strcmp(sub, "ps")) return cmd_ps(argc - 1, argv + 1);
        if (!strcmp(sub, "top")) return cmd_top(argc - 1, argv + 1);
    }
    fprintf(stderr, "vmon: multi-call binary. link as free/uptime/vmstat/ps/top,\n"
                    "      or run: vmon <free|uptime|vmstat|ps|top> [args]\n");
    return 1;
}
