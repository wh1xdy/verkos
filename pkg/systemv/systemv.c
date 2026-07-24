/* systemv — VerkOS' own init. Stage 1: early userspace (the initramfs /init).
 *
 * The kernel unpacks the initramfs and executes us as PID 1 before any real
 * filesystem exists. We bring up the pseudo-filesystems, find the root device
 * from the kernel command line, mount it, move our mounts across, and hand off
 * to the real init with switch_root semantics.
 *
 * This is the seed of SystemV, VerkOS' eventual systemd replacement: today it
 * owns the pre-root phase; stage 2 (PID 1 proper — service supervision, targets,
 * sockets) grows from here. Built static so the initramfs needs nothing but us.
 *
 * Kernel command line options honoured:
 *      root=DEV         block device (or LABEL=/UUID= passthrough) to mount
 *      rootfstype=FS    filesystem type (default: try ext4 then auto-detect)
 *      ro | rw          mount the root read-only (default) or writable
 *      init=PATH        what to exec after switching (default /sbin/init)
 *      rootdelay=N      extra seconds to wait for the root device
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
#include <dirent.h>

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

int main(void) {
    char root[256] = "", fstype[64] = "", init[256] = "", delay[16] = "";

    mount_pseudo();
    read_cmdline();
    debug = cmdline_flag("systemv.debug");

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
