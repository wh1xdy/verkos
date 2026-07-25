/* systemv — VerkOS' own init, and the systemd replacement the OS is heading for.
 *
 * One binary, two stages, chosen automatically:
 *
 *   Stage 1 — early userspace (the initramfs /init). The kernel unpacks the
 *   initramfs and runs us as PID 1 before any real filesystem exists. We bring
 *   up the pseudo-filesystems, find the root device from the kernel command
 *   line, mount it, carry our mounts across and switch_root into the real init.
 *
 *   Stage 2 — PID 1 on the real root. We mount the rest of the filesystems, set
 *   the hostname, start the services described in /etc/systemv/services, then
 *   supervise them: reap orphans, respawn what should come back, and shut the
 *   machine down cleanly on signal.
 *
 * Which stage runs is decided by whether /etc/systemv/services exists (it does
 * on the real root, it does not in the initramfs); systemv.stage=1|2 on the
 * kernel command line overrides that. Built static so the initramfs needs
 * nothing but us.
 *
 * Kernel command line options honoured:
 *      root=DEV         block device (or LABEL=/UUID= passthrough) to mount
 *      rootfstype=FS    filesystem type (default: try ext4 then auto-detect)
 *      ro | rw          mount the root read-only (default) or writable
 *      init=PATH        what to exec after switching (default /sbin/init)
 *      rootdelay=N      extra seconds to wait for the root device
 *      systemv.stage=N  force stage 1 or 2
 *      systemv.debug    keep going verbosely instead of quietly
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/reboot.h>
#include <dirent.h>
#include <signal.h>
#include <time.h>

#define NEWROOT "/newroot"

static int debug;

/* ---- console output (we are PID 1; keep it dependency-free) -------------- */
static void say(const char *s) {
    fputs("  [systemv] ", stderr); fputs(s, stderr); fputc('\n', stderr);
}
static void say2(const char *s, const char *t) {
    fputs("  [systemv] ", stderr); fputs(s, stderr); fputs(t, stderr); fputc('\n', stderr);
}
static void warn_errno(const char *what) {
    fputs("  [systemv] ", stderr); fputs(what, stderr);
    fputs(": ", stderr); fputs(strerror(errno), stderr); fputc('\n', stderr);
}
/* Unrecoverable: say why and stop. Panicking as PID 1 would just reboot-loop. */
static void panic(const char *why) {
    fputs("\n  [systemv] PANIC: ", stderr); fputs(why, stderr);
    fputs("\n  [systemv] dropping to a shell if one exists\n", stderr);
    execl("/bin/sh", "sh", (char *)NULL);
    for (;;) pause();
}

/* ---- kernel command line ------------------------------------------------- */
static char cmdline[4096];
static void read_cmdline(void) {
    int fd = open("/proc/cmdline", O_RDONLY);
    if (fd < 0) return;
    ssize_t n = read(fd, cmdline, sizeof cmdline - 1);
    close(fd);
    if (n > 0) cmdline[n] = 0;
}
/* value of key=... into out; returns 1 if found */
static int cmdline_get(const char *key, char *out, size_t cap) {
    size_t klen = strlen(key);
    const char *p = cmdline;
    while (*p) {
        while (*p == ' ' || *p == '\n') p++;
        if (!strncmp(p, key, klen) && p[klen] == '=') {
            const char *v = p + klen + 1;
            size_t i = 0;
            while (v[i] && v[i] != ' ' && v[i] != '\n' && i < cap - 1) { out[i] = v[i]; i++; }
            out[i] = 0;
            return 1;
        }
        while (*p && *p != ' ' && *p != '\n') p++;
    }
    return 0;
}
/* bare flag (e.g. "ro") present? */
static int cmdline_flag(const char *flag) {
    size_t flen = strlen(flag);
    const char *p = cmdline;
    while (*p) {
        while (*p == ' ' || *p == '\n') p++;
        if (!strncmp(p, flag, flen) && (p[flen] == ' ' || p[flen] == '\n' || !p[flen])) return 1;
        while (*p && *p != ' ' && *p != '\n') p++;
    }
    return 0;
}

/* ---- early filesystems --------------------------------------------------- */
static void mount_pseudo(void) {
    mkdir("/proc", 0555); mkdir("/sys", 0555); mkdir("/dev", 0755); mkdir("/run", 0755);
    if (mount("proc", "/proc", "proc", MS_NOSUID | MS_NOEXEC | MS_NODEV, NULL) && errno != EBUSY)
        warn_errno("mount /proc");
    if (mount("sysfs", "/sys", "sysfs", MS_NOSUID | MS_NOEXEC | MS_NODEV, NULL) && errno != EBUSY)
        warn_errno("mount /sys");
    /* devtmpfs gives us /dev/vda etc. without running a device manager */
    if (mount("devtmpfs", "/dev", "devtmpfs", MS_NOSUID, "mode=0755") && errno != EBUSY)
        warn_errno("mount /dev");
}

/* ---- waiting for the root device ---------------------------------------- */
static int device_ready(const char *dev) {
    struct stat st;
    return stat(dev, &st) == 0 && S_ISBLK(st.st_mode);
}
static int wait_for_device(const char *dev, int secs) {
    for (int i = 0; i <= secs * 10; i++) {
        if (device_ready(dev)) return 1;
        usleep(100000);                          /* 100ms between polls */
        if (debug && i && i % 10 == 0) say2("still waiting for ", dev);
    }
    return 0;
}

/* ---- the handoff --------------------------------------------------------- */
/* Move a mount we set up in the initramfs into the real root. */
static void carry_mount(const char *from, const char *to) {
    char path[256];
    snprintf(path, sizeof path, NEWROOT "%s", to);
    mkdir(path, 0755);
    if (mount(from, path, NULL, MS_MOVE, NULL)) {
        /* not fatal: the real init can mount it again itself */
        if (debug) warn_errno(from);
    }
}
static void switch_root(const char *init) {
    carry_mount("/dev", "/dev");
    carry_mount("/proc", "/proc");
    carry_mount("/sys", "/sys");
    carry_mount("/run", "/run");

    if (chdir(NEWROOT)) { warn_errno("chdir " NEWROOT); panic("cannot enter the new root"); }
    /* make the new root the actual root, then drop into it */
    if (mount(".", "/", NULL, MS_MOVE, NULL)) { warn_errno("mount --move . /"); panic("cannot move the root"); }
    if (chroot(".")) { warn_errno("chroot"); panic("cannot chroot"); }
    if (chdir("/")) warn_errno("chdir /");

    say2("handing off to ", init);
    fputc('\n', stderr);
    execl(init, init, (char *)NULL);
    /* systemd lives here on VerkOS; try it before giving up */
    execl("/usr/lib/systemd/systemd", "systemd", (char *)NULL);
    execl("/sbin/init", "init", (char *)NULL);
    execl("/bin/sh", "sh", (char *)NULL);
    panic("no init to exec");
}

/* =========================================================================
 * Stage 2 — PID 1 on the real root
 * ========================================================================= */
#define SVC_DIR      "/etc/systemv/services"
#define MAX_SVC      32
#define RESPAWN_NO   0
#define RESPAWN_ON_FAILURE 1
#define RESPAWN_ALWAYS     2

typedef struct {
    char name[64];              /* from the file name, minus any NN- prefix */
    char desc[128];
    char pre[256];              /* optional one-shot run before exec */
    char exec[256];
    int  respawn;
    pid_t pid;                  /* 0 when not running */
    int  restarts;
    time_t window;              /* start of the current restart-rate window */
} svc_t;

static svc_t svcs[MAX_SVC];
static int   n_svcs;
static volatile sig_atomic_t want_shutdown;   /* 0 none, 1 halt, 2 reboot */

static void on_signal(int sig) {
    if (sig == SIGINT) want_shutdown = 2;     /* ctrl-alt-del */
    else want_shutdown = 1;                   /* SIGTERM / SIGUSR1 */
}

/* ---- filesystems the real root needs ------------------------------------ */
static void mount_system(void) {
    /* the root was mounted read-only by stage 1 (or by the kernel); make it
     * writable now that we are past the point of no return */
    if (mount(NULL, "/", NULL, MS_REMOUNT, NULL) && debug) warn_errno("remount / rw");
    mkdir("/dev/pts", 0755); mkdir("/dev/shm", 1777);
    if (mount("devpts", "/dev/pts", "devpts", MS_NOSUID | MS_NOEXEC, "gid=5,mode=620")
        && errno != EBUSY && debug) warn_errno("mount /dev/pts");
    if (mount("tmpfs", "/dev/shm", "tmpfs", MS_NOSUID | MS_NODEV, "mode=1777")
        && errno != EBUSY && debug) warn_errno("mount /dev/shm");
    if (mount("tmpfs", "/run", "tmpfs", MS_NOSUID | MS_NODEV, "mode=0755")
        && errno != EBUSY && debug) warn_errno("mount /run");
    if (mount("tmpfs", "/tmp", "tmpfs", MS_NOSUID | MS_NODEV, "mode=1777")
        && errno != EBUSY && debug) warn_errno("mount /tmp");
}
static void set_hostname_from_file(void) {
    int fd = open("/etc/hostname", O_RDONLY);
    if (fd < 0) return;
    char buf[128];
    ssize_t n = read(fd, buf, sizeof buf - 1);
    close(fd);
    if (n <= 0) return;
    buf[n] = 0;
    char *nl = strchr(buf, '\n'); if (nl) *nl = 0;
    if (buf[0] && sethostname(buf, strlen(buf)) && debug) warn_errno("sethostname");
}

/* ---- service definitions ------------------------------------------------- */
static void svc_set(svc_t *s, const char *key, const char *val) {
    if      (!strcmp(key, "description")) snprintf(s->desc, sizeof s->desc, "%s", val);
    else if (!strcmp(key, "exec"))        snprintf(s->exec, sizeof s->exec, "%s", val);
    else if (!strcmp(key, "pre"))         snprintf(s->pre,  sizeof s->pre,  "%s", val);
    else if (!strcmp(key, "respawn")) {
        if      (!strcmp(val, "always"))     s->respawn = RESPAWN_ALWAYS;
        else if (!strcmp(val, "on-failure")) s->respawn = RESPAWN_ON_FAILURE;
        else                                 s->respawn = RESPAWN_NO;
    }
}
static int load_service(const char *dir, const char *file) {
    if (n_svcs >= MAX_SVC) return -1;
    char path[512];
    snprintf(path, sizeof path, "%s/%s", dir, file);
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    svc_t *s = &svcs[n_svcs];
    memset(s, 0, sizeof *s);
    /* "20-sshd.svc" -> "sshd": the numeric prefix only fixes start order */
    const char *base = file;
    while (*base >= '0' && *base <= '9') base++;
    if (*base == '-') base++;
    snprintf(s->name, sizeof s->name, "%s", base);
    char *dot = strrchr(s->name, '.'); if (dot && !strcmp(dot, ".svc")) *dot = 0;

    char line[512];
    while (fgets(line, sizeof line, f)) {
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '#' || *p == '\n' || !*p) continue;
        char *nl = strpbrk(p, "\r\n"); if (nl) *nl = 0;
        char *sp = strpbrk(p, " \t");
        if (!sp) continue;
        *sp++ = 0;
        while (*sp == ' ' || *sp == '\t') sp++;
        svc_set(s, p, sp);
    }
    fclose(f);
    if (!s->exec[0]) return -1;                    /* nothing to run */
    n_svcs++;
    return 0;
}
static int cmp_name(const void *a, const void *b) {
    return strcmp(*(const char **)a, *(const char **)b);
}
static void load_services(void) {
    DIR *d = opendir(SVC_DIR);
    if (!d) { say("no " SVC_DIR " — nothing to start"); return; }
    static char names[MAX_SVC][256];
    const char *ptrs[MAX_SVC];
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d)) && n < MAX_SVC) {
        if (e->d_name[0] == '.') continue;
        const char *dot = strrchr(e->d_name, '.');
        if (!dot || strcmp(dot, ".svc")) continue;
        snprintf(names[n], sizeof names[n], "%s", e->d_name);
        ptrs[n] = names[n];
        n++;
    }
    closedir(d);
    /* file names carry the order: 10-… before 20-… */
    qsort(ptrs, n, sizeof ptrs[0], cmp_name);
    for (int i = 0; i < n; i++) load_service(SVC_DIR, ptrs[i]);
}

/* ---- running things ------------------------------------------------------ */
/* split a command line on whitespace (no quoting — keep execs simple) */
static void split_args(char *cmd, char **argv, int cap) {
    int n = 0;
    char *p = cmd;
    while (*p && n < cap - 1) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        argv[n++] = p;
        while (*p && *p != ' ' && *p != '\t') p++;
        if (*p) *p++ = 0;
    }
    argv[n] = NULL;
}
/* run a command to completion (used for 'pre' steps) */
static void run_sync(const char *cmd) {
    char buf[256]; snprintf(buf, sizeof buf, "%s", cmd);
    char *argv[32]; split_args(buf, argv, 32);
    if (!argv[0]) return;
    pid_t p = fork();
    if (p == 0) { execv(argv[0], argv); _exit(127); }
    if (p > 0) { int st; while (waitpid(p, &st, 0) < 0 && errno == EINTR) {} }
}
/* Expand %console% to the tty the kernel is using (ttyAMA0 on arm, ttyS0 on
 * x86, …) so one service file works on every architecture. */
static void expand_console(char *cmd, size_t cap) {
    char *hit = strstr(cmd, "%console%");
    if (!hit) return;
    char con[64] = "console", raw[128] = "";
    if (cmdline_get("console", raw, sizeof raw)) {
        size_t i = 0;
        while (raw[i] && raw[i] != ',' && i < sizeof con - 1) { con[i] = raw[i]; i++; }
        con[i] = 0;
    }
    char out[256];
    size_t head = (size_t)(hit - cmd);
    snprintf(out, sizeof out, "%.*s%s%s", (int)head, cmd, con, hit + strlen("%console%"));
    snprintf(cmd, cap, "%s", out);
}
static void start_service(svc_t *s) {
    if (s->pre[0]) run_sync(s->pre);
    char buf[256]; snprintf(buf, sizeof buf, "%s", s->exec);
    expand_console(buf, sizeof buf);
    char *argv[32]; split_args(buf, argv, 32);
    if (!argv[0]) return;
    pid_t p = fork();
    if (p < 0) { warn_errno("fork"); return; }
    if (p == 0) {
        /* every service gets its own session so it cannot steal our terminal */
        setsid();
        execv(argv[0], argv);
        _exit(127);
    }
    s->pid = p;
    fputs("  [systemv] started ", stderr); fputs(s->name, stderr);
    if (s->desc[0]) { fputs(" — ", stderr); fputs(s->desc, stderr); }
    fputc('\n', stderr);
}
static svc_t *svc_by_pid(pid_t p) {
    for (int i = 0; i < n_svcs; i++) if (svcs[i].pid == p) return &svcs[i];
    return NULL;
}
/* should this service come back, and are we allowed to restart it again? */
static int should_respawn(svc_t *s, int status) {
    if (want_shutdown) return 0;
    if (s->respawn == RESPAWN_NO) return 0;
    if (s->respawn == RESPAWN_ON_FAILURE &&
        WIFEXITED(status) && WEXITSTATUS(status) == 0) return 0;
    time_t now = time(NULL);
    if (now - s->window > 60) { s->window = now; s->restarts = 0; }   /* new window */
    if (++s->restarts > 5) {
        fputs("  [systemv] ", stderr); fputs(s->name, stderr);
        fputs(" is restarting too fast — giving up on it\n", stderr);
        s->respawn = RESPAWN_NO;
        return 0;
    }
    return 1;
}

/* ---- shutdown ------------------------------------------------------------ */
static void stop_everything(void) {
    say("stopping services");
    for (int i = 0; i < n_svcs; i++)
        if (svcs[i].pid > 0) kill(svcs[i].pid, SIGTERM);
    kill(-1, SIGTERM);                       /* and anything else still around */
    for (int i = 0; i < 50; i++) {           /* up to 5s for a clean exit */
        if (waitpid(-1, NULL, WNOHANG) <= 0 && errno == ECHILD) break;
        usleep(100000);
    }
    kill(-1, SIGKILL);
    say("unmounting filesystems");
    sync();
    umount("/tmp"); umount("/run"); umount("/dev/shm"); umount("/dev/pts");
    mount(NULL, "/", NULL, MS_REMOUNT | MS_RDONLY, NULL);
    sync();
}
static void do_shutdown(int mode) {
    stop_everything();
    say(mode == 2 ? "rebooting" : "powering off");
    sleep(1);
    reboot(mode == 2 ? RB_AUTOBOOT : RB_POWER_OFF);
    reboot(RB_HALT_SYSTEM);                  /* if power-off is unsupported */
    for (;;) pause();
}

static int stage2(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT,  &sa, NULL);           /* ctrl-alt-del via the kernel */
    sigaction(SIGUSR1, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);
    /* PID 1 must claim ctrl-alt-del or the kernel reboots without asking us */
    reboot(RB_DISABLE_CAD);

    say("VerkOS init (stage 2) — SystemV");
    mount_pseudo();
    mount_system();
    set_hostname_from_file();

    load_services();
    say2("services: ", n_svcs ? "starting" : "none configured");
    for (int i = 0; i < n_svcs; i++) start_service(&svcs[i]);

    fputs("\n  [systemv] VerkOS is up\n\n", stderr);

    /* supervise: reap whatever dies, restart what should come back */
    for (;;) {
        if (want_shutdown) do_shutdown(want_shutdown);
        int status;
        pid_t p = waitpid(-1, &status, 0);
        if (p < 0) {
            if (errno == EINTR) continue;    /* a signal — loop and check */
            if (errno == ECHILD) { sleep(1); continue; }
            warn_errno("waitpid");
            sleep(1);
            continue;
        }
        svc_t *s = svc_by_pid(p);
        if (!s) continue;                    /* an orphan we just reaped */
        s->pid = 0;
        if (should_respawn(s, status)) {
            say2("restarting ", s->name);
            start_service(s);
        } else if (!want_shutdown) {
            say2("stopped ", s->name);
        }
    }
    return 0;                                    /* not reached */
}

/* =========================================================================
 * Stage 1 — the initramfs /init
 * ========================================================================= */
static int stage1(void) {
    char root[256] = "", fstype[64] = "", init[256] = "", delay[16] = "";

    say("VerkOS early userspace (stage 1)");
    if (debug) say2("cmdline: ", cmdline);

    if (!cmdline_get("root", root, sizeof root)) {
        /* No root= at all: the kernel would not have got here without one, but
         * be explicit rather than mounting something arbitrary. */
        panic("no root= on the kernel command line");
    }
    if (!cmdline_get("init", init, sizeof init)) strcpy(init, "/sbin/init");
    cmdline_get("rootfstype", fstype, sizeof fstype);

    int extra = 0;
    if (cmdline_get("rootdelay", delay, sizeof delay)) extra = atoi(delay);

    say2("root device: ", root);
    if (!wait_for_device(root, 10 + extra)) {
        say2("root device never appeared: ", root);
        panic("no root device");
    }

    if (mkdir(NEWROOT, 0755) && errno != EEXIST) warn_errno("mkdir " NEWROOT);

    /* Mount read-only first (fsck-friendly, and 'rw' on the cmdline is really a
     * request for the real init to remount); honour rw when asked. */
    unsigned long flags = cmdline_flag("rw") ? 0 : MS_RDONLY;
    const char *types[] = { fstype[0] ? fstype : "ext4", "ext4", "ext2", "vfat", NULL };
    int mounted = 0;
    for (int i = 0; types[i] && !mounted; i++) {
        if (mount(root, NEWROOT, types[i], flags, NULL) == 0) {
            say2("mounted root as ", types[i]);
            mounted = 1;
        } else if (debug) {
            warn_errno(types[i]);
        }
    }
    if (!mounted) { warn_errno("mount root"); panic("cannot mount the root filesystem"); }

    /* sanity: does the new root actually contain an init? */
    struct stat st;
    char probe[512];
    snprintf(probe, sizeof probe, NEWROOT "%s", init);
    if (stat(probe, &st) && stat(NEWROOT "/usr/lib/systemd/systemd", &st))
        say("warning: no init found in the new root, trying anyway");

    switch_root(init);
    return 0;                                    /* not reached */
}

int main(void) {
    mount_pseudo();                              /* /proc first: we need cmdline */
    read_cmdline();
    debug = cmdline_flag("systemv.debug");

    /* Stage 2 is for the real root, stage 1 for the initramfs. The service
     * directory only exists on the real root, which tells them apart; allow an
     * explicit override for odd setups and for testing. */
    char forced[8] = "";
    int stage;
    if (cmdline_get("systemv.stage", forced, sizeof forced))
        stage = (forced[0] == '2') ? 2 : 1;
    else {
        struct stat st;
        stage = (stat(SVC_DIR, &st) == 0 && S_ISDIR(st.st_mode)) ? 2 : 1;
    }
    return stage == 2 ? stage2() : stage1();
}
