/* urverk — VerkOS' own init, and the systemd replacement the OS is heading for.
 *
 * One binary, two stages, chosen automatically:
 *
 *   Stage 1 — early userspace (the initramfs /init). The kernel unpacks the
 *   initramfs and runs us as PID 1 before any real filesystem exists. We bring
 *   up the pseudo-filesystems, find the root device from the kernel command
 *   line, mount it, carry our mounts across and switch_root into the real init.
 *
 *   Stage 2 — PID 1 on the real root. We mount the rest of the filesystems, set
 *   the hostname, start the services described in /etc/urverk/services, then
 *   supervise them: reap orphans, respawn what should come back, and shut the
 *   machine down cleanly on signal.
 *
 * Which stage runs is decided by whether /etc/urverk/services exists (it does
 * on the real root, it does not in the initramfs); urverk.stage=1|2 on the
 * kernel command line overrides that. Built static so the initramfs needs
 * nothing but us.
 *
 * Kernel command line options honoured:
 *      root=DEV         block device (or LABEL=/UUID= passthrough) to mount
 *      rootfstype=FS    filesystem type (default: try ext4 then auto-detect)
 *      ro | rw          mount the root read-only (default) or writable
 *      init=PATH        what to exec after switching (default /sbin/init)
 *      rootdelay=N      extra seconds to wait for the root device
 *      urverk.stage=N  force stage 1 or 2
 *      urverk.debug    keep going verbosely instead of quietly
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
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/swap.h>
#include <grp.h>
#include <sys/ioctl.h>
#include <linux/fs.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdint.h>

#define NEWROOT "/newroot"

static int debug;

/* ---- console output (we are PID 1; keep it dependency-free) -------------- */
static void logline(const char *a, const char *b);
static void say(const char *s) {
    fputs("  [urverk] ", stderr); fputs(s, stderr); fputc('\n', stderr);
    logline(s, NULL);
}
static void say2(const char *s, const char *t) {
    fputs("  [urverk] ", stderr); fputs(s, stderr); fputs(t, stderr); fputc('\n', stderr);
    logline(s, t);
}
static void warn_errno(const char *what) {
    fputs("  [urverk] ", stderr); fputs(what, stderr);
    fputs(": ", stderr); fputs(strerror(errno), stderr); fputc('\n', stderr);
}
/* Unrecoverable: say why and stop. Panicking as PID 1 would just reboot-loop. */
static void panic(const char *why) {
    fputs("\n  [urverk] PANIC: ", stderr); fputs(why, stderr);
    fputs("\n  [urverk] dropping to a shell if one exists\n", stderr);
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

/* write a short string to a file; used for sysfs/cgroup knobs */
static void write_early(const char *path, const char *val) {
    int fd = open(path, O_WRONLY);
    if (fd < 0) return;
    ssize_t w = write(fd, val, strlen(val)); (void)w;
    close(fd);
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

/* ---- resolving root=LABEL=... / UUID=... --------------------------------
 * Disk order is not stable on real hardware, so an installed system should not
 * name /dev/vda. Both live in the ext2/3/4 superblock at a fixed offset, so we
 * can read them ourselves without blkid. */
static int ext_super(const char *dev, char *label, char *uuid) {
    int fd = open(dev, O_RDONLY);
    if (fd < 0) return -1;
    unsigned char sb[1024];
    int ok = -1;
    if (pread(fd, sb, sizeof sb, 1024) == (ssize_t)sizeof sb &&
        sb[0x38] == 0x53 && sb[0x39] == 0xEF) {          /* ext magic 0xEF53 */
        memcpy(label, sb + 0x78, 16); label[16] = 0;
        static const char *hex = "0123456789abcdef";
        char *u = uuid;
        for (int i = 0; i < 16; i++) {
            if (i == 4 || i == 6 || i == 8 || i == 10) *u++ = '-';
            *u++ = hex[sb[0x68 + i] >> 4];
            *u++ = hex[sb[0x68 + i] & 15];
        }
        *u = 0;
        ok = 0;
    }
    close(fd);
    return ok;
}
/* Turn "LABEL=x" / "UUID=y" into a device path; anything else is passed through. */
static void resolve_root(char *spec, size_t cap) {
    int by_label = !strncmp(spec, "LABEL=", 6);
    int by_uuid  = !strncmp(spec, "UUID=", 5);
    if (!by_label && !by_uuid) return;
    const char *want = spec + (by_label ? 6 : 5);
    say2("looking for root by ", spec);
    for (int attempt = 0; attempt < 100; attempt++) {     /* devices may still appear */
        DIR *d = opendir("/dev");
        struct dirent *e;
        while (d && (e = readdir(d))) {
            /* only the usual block-device names, to avoid opening every node */
            if (strncmp(e->d_name, "sd", 2) && strncmp(e->d_name, "vd", 2) &&
                strncmp(e->d_name, "nvme", 4) && strncmp(e->d_name, "mmcblk", 6)) continue;
            char dev[128], label[24], uuid[40];
            snprintf(dev, sizeof dev, "/dev/%.100s", e->d_name);
            if (ext_super(dev, label, uuid)) continue;
            if ((by_label && !strcmp(label, want)) || (by_uuid && !strcasecmp(uuid, want))) {
                closedir(d);
                say2("root is ", dev);
                snprintf(spec, cap, "%s", dev);
                return;
            }
        }
        if (d) closedir(d);
        usleep(100000);
    }
    say("no device matched — leaving the root spec as it is");
}

/* ---- checking the filesystem -------------------------------------------
 * Best effort: if no fsck is installed (VerkOS does not ship e2fsprogs yet)
 * this is simply skipped rather than being treated as a failure. */
static void run_fsck(const char *dev) {
    struct stat st;
    const char *tools[] = { "/usr/sbin/fsck.ext4", "/sbin/fsck.ext4", "/usr/sbin/e2fsck", NULL };
    const char *tool = NULL;
    for (int i = 0; tools[i] && !tool; i++) if (!stat(tools[i], &st)) tool = tools[i];
    if (!tool) { if (debug) say("no fsck installed — skipping the check"); return; }
    say2("checking ", dev);
    pid_t p = fork();
    if (p == 0) { execl(tool, tool, "-p", dev, (char *)NULL); _exit(127); }
    if (p < 0) return;
    int status = 0;
    while (waitpid(p, &status, 0) < 0 && errno == EINTR) {}
    int rc = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    /* fsck: 0 clean, 1 errors corrected — both fine to continue on */
    if (rc > 1) say("filesystem needs attention (fsck could not fix it automatically)");
}
/* Last resort when the root cannot be used: give a person a shell instead of
 * hanging or rebooting in a loop. */
static void emergency_shell(const char *why) {
    fputs("\n  [urverk] EMERGENCY: ", stderr); fputs(why, stderr);
    fputs("\n  [urverk] starting a shell; exit to retry the boot\n\n", stderr);
    pid_t p = fork();
    if (p == 0) { execl("/bin/sh", "sh", (char *)NULL); execl("/bin/bash", "bash", (char *)NULL); _exit(127); }
    if (p > 0) { int st; while (waitpid(p, &st, 0) < 0 && errno == EINTR) {} }
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
#define SVC_DIR      "/etc/urverk/services"
#define MAX_SVC      32
#define RESPAWN_NO   0
#define RESPAWN_ON_FAILURE 1
#define RESPAWN_ALWAYS     2

#define TYPE_SIMPLE  0          /* long-running; ready as soon as it is started */
#define TYPE_ONESHOT 1          /* runs to completion; ready when it exits 0 */
#define TYPE_TIMER   2          /* runs on a schedule instead of at boot */
#define TYPE_SOCKET  3          /* started the first time someone connects */

typedef struct {
    char name[64];              /* from the file name, minus any NN- prefix */
    char desc[128];
    char pre[256];              /* optional one-shot run before exec */
    char exec[256];
    char needs[256];            /* services that must be ready first */
    char user[32], group[32];   /* drop privileges to these before exec */
    char workdir[128];
    char env[256];              /* space-separated KEY=VALUE pairs */
    int  umask_val;             /* -1 = inherit */
    int  type;
    int  respawn;
    /* timer: every N seconds, or daily at a wall-clock minute */
    int  every_sec, at_hour, at_min;
    time_t next_run;
    /* socket activation: urverk holds the port until the first connection */
    char listen[64];
    int  lfd;
    /* cgroup v2 limits, applied to the service's own cgroup */
    char mem_max[32], cpu_weight[16];
    pid_t pid;                  /* 0 when not running */
    int  ready;                 /* dependents may start */
    int  started;               /* we have tried to start it this boot */
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
    /* cgroup v2, so services can be given memory and CPU limits */
    mkdir("/sys/fs/cgroup", 0755);
    if (mount("cgroup2", "/sys/fs/cgroup", "cgroup2", MS_NOSUID | MS_NOEXEC | MS_NODEV, NULL)
        && errno != EBUSY && debug) warn_errno("mount cgroup2");
    /* let child cgroups control memory and cpu */
    write_early("/sys/fs/cgroup/cgroup.subtree_control", "+memory +cpu");
}
/* ---- /etc/fstab ----------------------------------------------------------
 * urverk mounts the handful of pseudo-filesystems itself, but a real machine
 * has more: a separate /boot or /home, swap, an ESP. Honour fstab so an
 * installed system comes up the way its admin described it. */
#define MAX_MOUNTS 24
static char mounted[MAX_MOUNTS][128];
static int  n_mounted;

/* translate the options field: known flags become mount() bits, the rest is
 * passed through as filesystem-specific data (gid=5,mode=620, …) */
static unsigned long parse_opts(const char *opts, char *data, size_t cap) {
    unsigned long flags = 0;
    data[0] = 0;
    char buf[256]; snprintf(buf, sizeof buf, "%s", opts);
    for (char *tok = strtok(buf, ","); tok; tok = strtok(NULL, ",")) {
        if      (!strcmp(tok, "ro"))          flags |= MS_RDONLY;
        else if (!strcmp(tok, "rw"))          flags &= ~MS_RDONLY;
        else if (!strcmp(tok, "nosuid"))      flags |= MS_NOSUID;
        else if (!strcmp(tok, "nodev"))       flags |= MS_NODEV;
        else if (!strcmp(tok, "noexec"))      flags |= MS_NOEXEC;
        else if (!strcmp(tok, "noatime"))     flags |= MS_NOATIME;
        else if (!strcmp(tok, "nodiratime"))  flags |= MS_NODIRATIME;
        else if (!strcmp(tok, "relatime"))    flags |= MS_RELATIME;
        else if (!strcmp(tok, "sync"))        flags |= MS_SYNCHRONOUS;
        else if (!strcmp(tok, "defaults") || !strcmp(tok, "auto") ||
                 !strcmp(tok, "noauto")   || !strcmp(tok, "nofail")) continue;
        else {                                  /* filesystem-specific */
            if (data[0]) strncat(data, ",", cap - strlen(data) - 1);
            strncat(data, tok, cap - strlen(data) - 1);
        }
    }
    return flags;
}
static int opt_present(const char *opts, const char *want) {
    char buf[256]; snprintf(buf, sizeof buf, "%s", opts);
    for (char *tok = strtok(buf, ","); tok; tok = strtok(NULL, ","))
        if (!strcmp(tok, want)) return 1;
    return 0;
}
/* is this path already a mount point? avoids stacking a second mount on top of
 * the pseudo-filesystems urverk brought up itself */
static int already_mounted(const char *mnt) {
    FILE *m = fopen("/proc/mounts", "r");
    if (!m) return 0;
    char line[512], dev[128], where[128];
    int hit = 0;
    while (fgets(line, sizeof line, m))
        if (sscanf(line, "%127s %127s", dev, where) == 2 && !strcmp(where, mnt)) { hit = 1; break; }
    fclose(m);
    return hit;
}
static void mount_fstab(void) {
    FILE *f = fopen("/etc/fstab", "r");
    if (!f) { if (debug) say("no /etc/fstab"); return; }
    char line[512];
    while (fgets(line, sizeof line, f)) {
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '#' || *p == '\n' || !*p) continue;
        char dev[128], mnt[128], type[64], opts[256];
        if (sscanf(p, "%127s %127s %63s %255s", dev, mnt, type, opts) < 3) continue;
        if (sscanf(p, "%127s %127s %63s %255s", dev, mnt, type, opts) < 4)
            snprintf(opts, sizeof opts, "defaults");
        if (opt_present(opts, "noauto")) continue;

        if (!strcmp(type, "swap")) {
            if (swapon(dev, 0) == 0) say2("swap on ", dev);
            else if (debug) warn_errno(dev);
            continue;
        }
        if (!strcmp(mnt, "/")) continue;            /* already our root */
        if (already_mounted(mnt)) { if (debug) say2("already mounted: ", mnt); continue; }
        char data[256];
        unsigned long flags = parse_opts(opts, data, sizeof data);
        mkdir(mnt, 0755);
        if (mount(dev, mnt, type, flags, data[0] ? data : NULL) == 0) {
            if (n_mounted < MAX_MOUNTS) snprintf(mounted[n_mounted++], 128, "%s", mnt);
            if (debug) say2("mounted ", mnt);
        } else if (errno != EBUSY && debug) {
            warn_errno(mnt);
        }
    }
    fclose(f);
}

/* ---- logging -------------------------------------------------------------
 * Everything urverk says goes to the console; once the root is writable it is
 * also appended to /var/log/urverk.log so the boot can be read back later. */
static int logfd = -1;
static void log_open(void) {
    mkdir("/var", 0755); mkdir("/var/log", 0755);
    logfd = open("/var/log/urverk.log", O_WRONLY | O_CREAT | O_APPEND, 0640);
    if (logfd >= 0) {
        time_t t = time(NULL);
        char hdr[128];
        int n = snprintf(hdr, sizeof hdr, "\n=== urverk boot at %ld ===\n", (long)t);
        ssize_t w = write(logfd, hdr, n); (void)w;
    }
}
/* append one message to the log; a no-op until log_open() has run */
static void logline(const char *a, const char *b) {
    if (logfd < 0) return;
    char buf[512];
    int n = snprintf(buf, sizeof buf, "%s%s\n", a, b ? b : "");
    ssize_t w = write(logfd, buf, n); (void)w;
}

/* Tell vboot this boot actually made it to userspace.
 *
 * vboot marks the entry it is about to start as "trying" in the VbootLast EFI
 * variable; if it still says that on the next boot, the entry never came up and
 * vboot stops the countdown instead of looping into it again. Clearing the flag
 * here is the confirmation. efivarfs files are immutable by default, so the
 * flag has to come off before the write. */
#define EFIVARS "/sys/firmware/efi/efivars"
#define VBOOT_VAR EFIVARS "/VbootLast-5645524b-4f53-4256-4f4f-540000000001"
static void confirm_boot(void) {
    mkdir("/sys/firmware/efi/efivars", 0755);
    if (mount("efivarfs", EFIVARS, "efivarfs", 0, NULL) && errno != EBUSY) {
        warn_errno("mount efivarfs");
        return;                                  /* not a UEFI boot: nothing to do */
    }
    /* efivarfs files are immutable, so O_RDWR is refused outright (EPERM). Read
     * first, take the flag off, then reopen for writing. */
    int fd = open(VBOOT_VAR, O_RDONLY);
    if (fd < 0) { if (debug) warn_errno("open " VBOOT_VAR); return; }
    unsigned char buf[8];
    ssize_t n = read(fd, buf, sizeof buf);
    if (n < 8) { say("vboot variable too short to confirm"); close(fd); return; }
    if (!(buf[6] & 0x01)) { close(fd); return; }   /* already confirmed */
    buf[6] &= (unsigned char)~0x01;                /* this boot reached userspace */

    int flags = 0;
    if (!ioctl(fd, FS_IOC_GETFLAGS, &flags) && (flags & FS_IMMUTABLE_FL)) {
        flags &= ~FS_IMMUTABLE_FL;
        if (ioctl(fd, FS_IOC_SETFLAGS, &flags)) warn_errno("clearing the immutable flag");
    }
    close(fd);

    fd = open(VBOOT_VAR, O_WRONLY);
    if (fd < 0) { warn_errno("reopen " VBOOT_VAR); return; }
    /* efivarfs wants the whole variable — attributes and data — in one write */
    if (write(fd, buf, 8) == 8) say("confirmed this boot to vboot");
    else warn_errno("writing the vboot variable");
    close(fd);
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
    else if (!strcmp(key, "needs") || !strcmp(key, "after")) {
        /* both express ordering; needs also waits for the dependency to be ready */
        if (s->needs[0]) strncat(s->needs, " ", sizeof s->needs - strlen(s->needs) - 1);
        strncat(s->needs, val, sizeof s->needs - strlen(s->needs) - 1);
    }
    else if (!strcmp(key, "user"))    snprintf(s->user,    sizeof s->user,    "%s", val);
    else if (!strcmp(key, "group"))   snprintf(s->group,   sizeof s->group,   "%s", val);
    else if (!strcmp(key, "workdir")) snprintf(s->workdir, sizeof s->workdir, "%s", val);
    else if (!strcmp(key, "env")) {
        if (s->env[0]) strncat(s->env, " ", sizeof s->env - strlen(s->env) - 1);
        strncat(s->env, val, sizeof s->env - strlen(s->env) - 1);
    }
    else if (!strcmp(key, "umask"))   s->umask_val = (int)strtol(val, NULL, 8);
    else if (!strcmp(key, "type")) {
        s->type = !strcmp(val, "oneshot") ? TYPE_ONESHOT
                : !strcmp(val, "timer")   ? TYPE_TIMER
                : !strcmp(val, "socket")  ? TYPE_SOCKET : TYPE_SIMPLE;
    }
    else if (!strcmp(key, "every")) {
        /* 30s / 5m / 2h — a bare number is seconds */
        char *end = NULL;
        long v = strtol(val, &end, 10);
        if (end && *end == 'm') v *= 60;
        else if (end && *end == 'h') v *= 3600;
        else if (end && *end == 'd') v *= 86400;
        s->every_sec = (int)v;
        s->type = TYPE_TIMER;
    }
    else if (!strcmp(key, "at")) {                     /* HH:MM, daily */
        int h = 0, m = 0;
        if (sscanf(val, "%d:%d", &h, &m) == 2) { s->at_hour = h; s->at_min = m; }
        s->type = TYPE_TIMER;
    }
    else if (!strcmp(key, "listen")) {
        snprintf(s->listen, sizeof s->listen, "%s", val);
        s->type = TYPE_SOCKET;
    }
    else if (!strcmp(key, "memory")) snprintf(s->mem_max,    sizeof s->mem_max,    "%s", val);
    else if (!strcmp(key, "cpu"))    snprintf(s->cpu_weight, sizeof s->cpu_weight, "%s", val);
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
    s->umask_val = -1;
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
/* VerkOS' own daemons, known to urverk directly. These are what the OS is made
 * of — the DHCP client, the ssh server and the console login — so urverk brings
 * them up even with no files in SVC_DIR at all (a wiped /etc, a rescue boot, or
 * an installer's freshly-copied root still comes up complete). Anything in
 * SVC_DIR with the same name overrides the built-in. */
static const svc_t builtin[] = {
    { .name = "vdhcp", .desc = "vdhcp DHCP client (VerkOS)",
      .exec = "/usr/sbin/vdhcp", .respawn = RESPAWN_ALWAYS,
      .type = TYPE_SIMPLE, .umask_val = -1 },
    { .name = "sshd", .desc = "OpenSSH server (VerkOS)",
      .pre = "/usr/bin/ssh-keygen -A", .exec = "/usr/sbin/sshd -D -e",
      .respawn = RESPAWN_ON_FAILURE, .type = TYPE_SIMPLE, .umask_val = -1 },
    { .name = "console", .desc = "console login (vgetty)",
      .exec = "/usr/bin/vgetty --autologin root --keep-baud %console% 115200,38400,9600 linux",
      .respawn = RESPAWN_ALWAYS, .type = TYPE_SIMPLE, .umask_val = -1 },
};
static int have_svc(const char *name) {
    for (int i = 0; i < n_svcs; i++) if (!strcmp(svcs[i].name, name)) return 1;
    return 0;
}
static void add_builtins(void) {
    for (unsigned i = 0; i < sizeof builtin / sizeof builtin[0]; i++) {
        if (n_svcs >= MAX_SVC) return;
        if (have_svc(builtin[i].name)) continue;      /* a file already defines it */
        svcs[n_svcs++] = builtin[i];
        if (debug) say2("using built-in service ", builtin[i].name);
    }
}
static void load_services(void) {
    DIR *d = opendir(SVC_DIR);
    if (!d) { say("no " SVC_DIR " — using built-in services"); add_builtins(); return; }
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
    add_builtins();          /* fill in any core service the files did not define */
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
/* Send a service's output to its own log, so the console is not a soup of every
 * daemon at once and `urverk-ctl logs NAME` has something to show. */
#define LOG_DIR "/var/log/urverk"
static void svc_redirect_log(const svc_t *s) {
    char path[256];
    mkdir(LOG_DIR, 0750);
    snprintf(path, sizeof path, LOG_DIR "/%s.log", s->name);
    int fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0640);
    if (fd < 0) return;                       /* keep the console rather than fail */
    dup2(fd, STDOUT_FILENO);
    dup2(fd, STDERR_FILENO);
    if (fd > STDERR_FILENO) close(fd);
}
/* KEY=VALUE pairs from the service file */
static void svc_apply_env(const svc_t *s) {
    if (!s->env[0]) return;
    char buf[256]; snprintf(buf, sizeof buf, "%s", s->env);
    for (char *tok = strtok(buf, " \t"); tok; tok = strtok(NULL, " \t")) {
        char *eq = strchr(tok, '=');
        if (!eq) continue;
        *eq = 0;
        setenv(tok, eq + 1, 1);
    }
}
/* look a name up in /etc/passwd or /etc/group without pulling in NSS */
static int lookup_id(const char *file, const char *name, int field) {
    FILE *f = fopen(file, "r");
    if (!f) return -1;
    char line[512];
    int id = -1;
    while (fgets(line, sizeof line, f)) {
        char *save = NULL, *p = strtok_r(line, ":", &save);
        if (!p || strcmp(p, name)) continue;
        for (int i = 1; i <= field; i++) p = strtok_r(NULL, ":", &save);
        if (p) id = (int)strtol(p, NULL, 10);
        break;
    }
    fclose(f);
    return id;
}
static int svc_drop_privs(const svc_t *s) {
    if (!s->user[0] && !s->group[0]) return 0;
    int gid = -1, uid = -1;
    if (s->group[0]) gid = lookup_id("/etc/group", s->group, 2);
    if (s->user[0])  uid = lookup_id("/etc/passwd", s->user, 2);
    if (s->user[0] && gid < 0) gid = lookup_id("/etc/passwd", s->user, 3);
    if (s->user[0] && uid < 0) return -1;                 /* unknown user: refuse */
    if (gid >= 0) { setgroups(0, NULL); if (setgid((gid_t)gid)) return -1; }
    if (uid >= 0 && setuid((uid_t)uid)) return -1;
    return 0;
}
/* cgroup v2: give the service its own group and write the limits into it. The
 * child puts itself in the group, so everything it forks is accounted too. */
#define CGROOT "/sys/fs/cgroup"
static void write_file(const char *path, const char *val) {
    int fd = open(path, O_WRONLY);
    if (fd < 0) return;
    ssize_t w = write(fd, val, strlen(val)); (void)w;
    close(fd);
}
static void cgroup_prepare(const svc_t *s) {
    if (!s->mem_max[0] && !s->cpu_weight[0]) return;
    char dir[256], path[320];
    snprintf(dir, sizeof dir, CGROOT "/urverk.%.60s", s->name);
    if (mkdir(dir, 0755) && errno != EEXIST) { if (debug) warn_errno(dir); return; }
    if (s->mem_max[0])    { snprintf(path, sizeof path, "%s/memory.max", dir);  write_file(path, s->mem_max); }
    if (s->cpu_weight[0]) { snprintf(path, sizeof path, "%s/cpu.weight", dir);  write_file(path, s->cpu_weight); }
}
static void cgroup_join(const svc_t *s) {
    if (!s->mem_max[0] && !s->cpu_weight[0]) return;
    char path[320], pid[16];
    snprintf(path, sizeof path, CGROOT "/urverk.%.60s/cgroup.procs", s->name);
    snprintf(pid, sizeof pid, "%d", (int)getpid());
    write_file(path, pid);
}

/* ---- timers -------------------------------------------------------------- */
static void timer_schedule(svc_t *s, int first) {
    time_t now = time(NULL);
    if (s->every_sec > 0) {
        s->next_run = now + (first ? s->every_sec : s->every_sec);
        return;
    }
    /* daily at HH:MM: next occurrence, today or tomorrow */
    struct tm tmv;
    localtime_r(&now, &tmv);
    struct tm want = tmv;
    want.tm_hour = s->at_hour; want.tm_min = s->at_min; want.tm_sec = 0;
    time_t t = mktime(&want);
    if (t <= now) t += 86400;
    s->next_run = t;
}

/* ---- socket activation ---------------------------------------------------
 * urverk holds the port and starts the service on the first connection, then
 * hands the port over by closing its own listener. Simple and dependency-free:
 * the service binds the port itself, exactly as it would if started at boot. */
static void socket_listen(svc_t *s) {
    if (!s->listen[0]) return;
    char host[48] = "0.0.0.0"; int port = 0;
    const char *colon = strrchr(s->listen, ':');
    if (colon) {
        size_t hl = (size_t)(colon - s->listen);
        if (hl < sizeof host) { memcpy(host, s->listen, hl); host[hl] = 0; }
        port = atoi(colon + 1);
    } else port = atoi(s->listen);
    if (port <= 0) { say2("bad listen address: ", s->listen); return; }

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { warn_errno("socket"); return; }
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)port);
    a.sin_addr.s_addr = inet_addr(host);
    if (bind(fd, (struct sockaddr *)&a, sizeof a) || listen(fd, 8)) {
        warn_errno("listen"); close(fd); return;
    }
    s->lfd = fd;
    say2("waiting for a connection before starting ", s->name);
}

static void start_service(svc_t *s) {
    if (s->pre[0]) run_sync(s->pre);
    char buf[256]; snprintf(buf, sizeof buf, "%s", s->exec);
    expand_console(buf, sizeof buf);
    char *argv[32]; split_args(buf, argv, 32);
    if (!argv[0]) return;
    cgroup_prepare(s);
    pid_t p = fork();
    if (p < 0) { warn_errno("fork"); return; }
    if (p == 0) {
        /* every service gets its own session so it cannot steal our terminal */
        setsid();
        cgroup_join(s);
        svc_redirect_log(s);
        if (s->workdir[0] && chdir(s->workdir)) _exit(126);
        if (s->umask_val >= 0) umask((mode_t)s->umask_val);
        svc_apply_env(s);
        if (svc_drop_privs(s)) _exit(126);       /* never run as root by mistake */
        execv(argv[0], argv);
        _exit(127);
    }
    s->pid = p;
    s->started = 1;
    /* A simple service is ready as soon as it is running; a oneshot only counts
     * as ready once it has finished successfully (see the reaper). */
    if (s->type == TYPE_SIMPLE) s->ready = 1;
    say2("started ", s->name[0] ? s->name : "(unnamed)");
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
        fputs("  [urverk] ", stderr); fputs(s->name, stderr);
        fputs(" is restarting too fast — giving up on it\n", stderr);
        s->respawn = RESPAWN_NO;
        return 0;
    }
    return 1;
}

/* ---- control socket ------------------------------------------------------
 * PID 1 listens on /run/urverk.sock; the same binary run by a user connects to
 * it (see client_main). One text command per connection, one text reply. */
#define CTL_SOCK "/run/urverk.sock"
static int ctl_fd = -1;

static void ctl_open(void) {
    struct sockaddr_un a;
    unlink(CTL_SOCK);
    ctl_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (ctl_fd < 0) { warn_errno("control socket"); return; }
    memset(&a, 0, sizeof a);
    a.sun_family = AF_UNIX;
    snprintf(a.sun_path, sizeof a.sun_path, "%s", CTL_SOCK);
    if (bind(ctl_fd, (struct sockaddr *)&a, sizeof a) || listen(ctl_fd, 4)) {
        warn_errno("control socket"); close(ctl_fd); ctl_fd = -1; return;
    }
    chmod(CTL_SOCK, 0600);                 /* root only: this stops services */
}
static svc_t *svc_by_name(const char *name) {
    for (int i = 0; i < n_svcs; i++) if (!strcmp(svcs[i].name, name)) return &svcs[i];
    return NULL;
}
static void stop_service(svc_t *s) {
    if (s->pid <= 0) return;
    int was = s->respawn;
    s->respawn = RESPAWN_NO;               /* do not fight the operator */
    kill(s->pid, SIGTERM);
    for (int i = 0; i < 30 && s->pid > 0; i++) {
        int st; pid_t p = waitpid(s->pid, &st, WNOHANG);
        if (p == s->pid) { s->pid = 0; break; }
        usleep(100000);
    }
    if (s->pid > 0) { kill(s->pid, SIGKILL); waitpid(s->pid, NULL, 0); s->pid = 0; }
    s->respawn = was;
}
/* handle one command; writes the reply to fd */
static void ctl_handle(int fd) {
    char buf[256];
    ssize_t n = read(fd, buf, sizeof buf - 1);
    if (n <= 0) return;
    buf[n] = 0;
    char *nl = strpbrk(buf, "\r\n"); if (nl) *nl = 0;
    char *arg = strchr(buf, ' ');
    if (arg) { *arg++ = 0; while (*arg == ' ') arg++; }

    char out[2048]; out[0] = 0;
    if (!strcmp(buf, "status")) {
        for (int i = 0; i < n_svcs; i++) {
            char line[256];
            svc_t *s = &svcs[i];
            const char *state = s->pid > 0 ? "running"
                             : s->type == TYPE_ONESHOT ? (s->ready ? "done" : "failed")
                             : s->respawn == RESPAWN_NO ? "stopped" : "dead";
            snprintf(line, sizeof line, "%-12.63s %-9.9s %.100s\n", s->name, state, s->desc);
            strncat(out, line, sizeof out - strlen(out) - 1);
        }
        if (!n_svcs) snprintf(out, sizeof out, "no services\n");
    } else if (!strcmp(buf, "stop") && arg) {
        svc_t *s = svc_by_name(arg);
        if (!s) snprintf(out, sizeof out, "no such service: %s\n", arg);
        else { stop_service(s); s->respawn = RESPAWN_NO; snprintf(out, sizeof out, "stopped %s\n", arg); }
    } else if ((!strcmp(buf, "start") || !strcmp(buf, "restart")) && arg) {
        svc_t *s = svc_by_name(arg);
        if (!s) snprintf(out, sizeof out, "no such service: %s\n", arg);
        else {
            if (s->pid > 0) stop_service(s);
            s->restarts = 0;
            start_service(s);
            snprintf(out, sizeof out, "%s %s (pid %d)\n", buf, arg, (int)s->pid);
        }
    } else if (!strcmp(buf, "logs") && arg) {
        char path[256];
        snprintf(path, sizeof path, LOG_DIR "/%.64s.log", arg);
        int lf = open(path, O_RDONLY);
        if (lf < 0) { snprintf(out, sizeof out, "no log for %s\n", arg); }
        else {
            /* send the tail; a boot's worth of log is plenty to diagnose with */
            off_t end = lseek(lf, 0, SEEK_END);
            off_t from = end > 65536 ? end - 65536 : 0;
            lseek(lf, from, SEEK_SET);
            char b[4096];
            ssize_t n2;
            while ((n2 = read(lf, b, sizeof b)) > 0) { ssize_t w2 = write(fd, b, n2); (void)w2; }
            close(lf);
            return;                                   /* already written */
        }
    } else if (!strcmp(buf, "reboot"))   { want_shutdown = 2; snprintf(out, sizeof out, "rebooting\n"); }
    else if (!strcmp(buf, "poweroff") || !strcmp(buf, "halt")) {
        want_shutdown = 1; snprintf(out, sizeof out, "powering off\n");
    } else {
        snprintf(out, sizeof out,
                 "urverk: status | start NAME | stop NAME | restart NAME | logs NAME | reboot | poweroff\n");
    }
    ssize_t w = write(fd, out, strlen(out)); (void)w;
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
    /* fstab mounts first, deepest last-mounted first, then our own */
    for (int i = n_mounted - 1; i >= 0; i--) umount(mounted[i]);
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

/* Is every service this one needs ready? Unknown names are ignored rather than
 * deadlocking the boot over a typo. */
static int deps_ready(const svc_t *s) {
    if (!s->needs[0]) return 1;
    char buf[256]; snprintf(buf, sizeof buf, "%s", s->needs);
    for (char *tok = strtok(buf, " \t"); tok; tok = strtok(NULL, " \t")) {
        svc_t *d = svc_by_name(tok);
        if (d && !d->ready) return 0;
    }
    return 1;
}
/* Start services in dependency order: repeatedly start whatever has all its
 * needs satisfied. A oneshot blocks its dependents until it exits, so we reap
 * while we wait. If nothing can progress (a cycle, or a oneshot that never
 * finishes) we say so and start the rest anyway rather than hang the boot. */
static void start_all(void) {
    for (int round = 0; round < MAX_SVC + 2; round++) {
        int started_any = 0, pending = 0;
        for (int i = 0; i < n_svcs; i++) {
            svc_t *s = &svcs[i];
            if (s->started) continue;
            if (!deps_ready(s)) { pending = 1; continue; }
            if (s->type == TYPE_TIMER) {          /* armed, not run, at boot */
                timer_schedule(s, 1);
                s->started = 1; s->ready = 1;
                say2("armed timer ", s->name);
                started_any = 1;
                continue;
            }
            if (s->type == TYPE_SOCKET) {         /* holds the port until someone connects */
                socket_listen(s);
                s->started = 1; s->ready = 1;
                started_any = 1;
                continue;
            }
            start_service(s);
            started_any = 1;
            if (s->type == TYPE_ONESHOT) {
                /* wait for it, with a cap so a hung oneshot cannot stop the boot */
                for (int t = 0; t < 300 && s->pid > 0; t++) {
                    int st;
                    pid_t p = waitpid(s->pid, &st, WNOHANG);
                    if (p == s->pid) {
                        s->pid = 0;
                        s->ready = (WIFEXITED(st) && WEXITSTATUS(st) == 0);
                        if (!s->ready) say2("oneshot failed: ", s->name);
                        break;
                    }
                    usleep(100000);
                }
                if (s->pid > 0) { say2("oneshot still running, continuing: ", s->name); s->ready = 1; }
            }
        }
        if (!pending) return;                    /* everything is up */
        if (!started_any) {                      /* nothing can proceed */
            for (int i = 0; i < n_svcs; i++) {
                if (svcs[i].started) continue;
                say2("unmet dependency, starting anyway: ", svcs[i].name);
                start_service(&svcs[i]);
            }
            return;
        }
    }
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

    say("VerkOS init (stage 2) — Urverk");
    mount_pseudo();
    mount_system();
    log_open();                      /* root is writable from here on */
    mount_fstab();
    set_hostname_from_file();

    load_services();
    say2("services: ", n_svcs ? "starting" : "none configured");
    start_all();

    confirm_boot();                  /* stop vboot from treating this as a failed boot */
    fputs("\n  [urverk] VerkOS is up\n\n", stderr);

    ctl_open();

    /* Supervise: wait on the control socket, and reap children as they die.
     * poll() with a timeout keeps both jobs on one thread — SIGCHLD only needs
     * to interrupt the wait, the reaping happens right after. */
    for (;;) {
        if (want_shutdown) do_shutdown(want_shutdown);

        struct pollfd pfd[1 + MAX_SVC];
        int nfd = 0, idx[1 + MAX_SVC];
        if (ctl_fd >= 0) { pfd[nfd].fd = ctl_fd; pfd[nfd].events = POLLIN; idx[nfd] = -1; nfd++; }
        for (int i = 0; i < n_svcs; i++)
            if (svcs[i].type == TYPE_SOCKET && svcs[i].lfd > 0 && svcs[i].pid <= 0) {
                pfd[nfd].fd = svcs[i].lfd; pfd[nfd].events = POLLIN; idx[nfd] = i; nfd++;
            }
        int pr = nfd ? poll(pfd, nfd, 1000) : (usleep(200000), 0);
        for (int k = 0; k < nfd && pr > 0; k++) {
            if (!(pfd[k].revents & POLLIN)) continue;
            if (idx[k] < 0) {                                  /* the control socket */
                int c = accept(ctl_fd, NULL, NULL);
                if (c >= 0) { ctl_handle(c); close(c); }
            } else {                                           /* someone connected */
                svc_t *s = &svcs[idx[k]];
                say2("connection — starting ", s->name);
                close(s->lfd); s->lfd = 0;   /* hand the port to the service */
                start_service(s);
            }
        }
        /* due timers */
        {
            time_t now = time(NULL);
            for (int i = 0; i < n_svcs; i++) {
                svc_t *s = &svcs[i];
                if (s->type != TYPE_TIMER || s->pid > 0) continue;
                if (!s->next_run || now < s->next_run) continue;
                say2("timer fired: ", s->name);
                int keep = s->type; s->type = TYPE_SIMPLE;   /* run it like a job */
                start_service(s);
                s->type = keep;
                timer_schedule(s, 0);
            }
        }

        /* reap everything that exited while we waited */
        for (;;) {
            int status;
            pid_t p = waitpid(-1, &status, WNOHANG);
            if (p <= 0) break;
            svc_t *s = svc_by_pid(p);
            if (!s) continue;                /* an orphan we just reaped */
            s->pid = 0;
            if (s->type == TYPE_ONESHOT) {   /* completion is what makes it ready */
                s->ready = (WIFEXITED(status) && WEXITSTATUS(status) == 0);
                continue;                    /* oneshots are not respawned */
            }
            if (s->type == TYPE_TIMER) continue;      /* it runs again on schedule */
            if (s->type == TYPE_SOCKET && !want_shutdown) {
                socket_listen(s);            /* take the port back and wait again */
                continue;
            }
            if (should_respawn(s, status)) {
                say2("restarting ", s->name);
                start_service(s);
            } else if (!want_shutdown) {
                say2("stopped ", s->name);
            }
        }
    }
    return 0;                                    /* not reached */
}

/* =========================================================================
 * Client mode — the same binary, run by hand (install it as urverk-ctl too)
 * ========================================================================= */
static int client_main(int argc, char **argv) {
    char cmd[256] = "";
    for (int i = 1; i < argc; i++) {
        if (cmd[0]) strncat(cmd, " ", sizeof cmd - strlen(cmd) - 1);
        strncat(cmd, argv[i], sizeof cmd - strlen(cmd) - 1);
    }
    if (!cmd[0]) snprintf(cmd, sizeof cmd, "status");

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) { perror("urverk: socket"); return 1; }
    struct sockaddr_un a;
    memset(&a, 0, sizeof a);
    a.sun_family = AF_UNIX;
    snprintf(a.sun_path, sizeof a.sun_path, "%s", CTL_SOCK);
    if (connect(fd, (struct sockaddr *)&a, sizeof a)) {
        fprintf(stderr, "urverk: cannot reach init on %s: %s\n"
                        "        (is urverk running as PID 1?)\n", CTL_SOCK, strerror(errno));
        return 1;
    }
    strncat(cmd, "\n", sizeof cmd - strlen(cmd) - 1);
    if (write(fd, cmd, strlen(cmd)) < 0) { perror("urverk: write"); return 1; }
    char buf[2048];
    ssize_t n;
    while ((n = read(fd, buf, sizeof buf)) > 0) { ssize_t w = write(1, buf, n); (void)w; }
    close(fd);
    return 0;
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
    resolve_root(root, sizeof root);          /* LABEL=/UUID= -> /dev/... */
    if (!wait_for_device(root, 10 + extra)) {
        say2("root device never appeared: ", root);
        emergency_shell("the root device never appeared");
        panic("no root device");
    }
    run_fsck(root);                            /* skipped when no fsck is installed */

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
    if (!mounted) {
        warn_errno("mount root");
        emergency_shell("the root filesystem could not be mounted");
        /* the shell may have fixed it — one more try before giving up */
        for (int i = 0; types[i] && !mounted; i++)
            if (mount(root, NEWROOT, types[i], flags, NULL) == 0) mounted = 1;
        if (!mounted) panic("cannot mount the root filesystem");
    }

    /* sanity: does the new root actually contain an init? */
    struct stat st;
    char probe[512];
    snprintf(probe, sizeof probe, NEWROOT "%s", init);
    if (stat(probe, &st) && stat(NEWROOT "/usr/lib/systemd/systemd", &st))
        say("warning: no init found in the new root, trying anyway");

    switch_root(init);
    return 0;                                    /* not reached */
}

int main(int argc, char **argv) {
    /* Only PID 1 is the init; any other invocation is the control client, so
     * `urverk status` (or urverk-ctl, a symlink to this binary) just works. */
    if (getpid() != 1) return client_main(argc, argv);

    mount_pseudo();                              /* /proc first: we need cmdline */
    read_cmdline();
    debug = cmdline_flag("urverk.debug");

    /* Stage 2 is for the real root, stage 1 for the initramfs. The service
     * directory only exists on the real root, which tells them apart; allow an
     * explicit override for odd setups and for testing. */
    char forced[8] = "";
    int stage;
    if (cmdline_get("urverk.stage", forced, sizeof forced))
        stage = (forced[0] == '2') ? 2 : 1;
    else {
        struct stat st;
        stage = (stat(SVC_DIR, &st) == 0 && S_ISDIR(st.st_mode)) ? 2 : 1;
    }
    return stage == 2 ? stage2() : stage1();
}
