/* vinstall — the VerkOS installer.
 *
 * Takes a running VerkOS (live image or an installed one) and writes it onto a
 * disk it can boot from on its own: GPT, an EFI system partition holding vboot
 * and the kernel, and an ext4 root. Nothing here is specific to being "live";
 * installing from an installed system works and is how it gets tested.
 *
 *   vinstall [-t /dev/sda] [-y] [-H HOSTNAME] [-u USER] [--esp MB] [-n]
 *
 *   -t  target disk (asks if omitted)      -y  do not ask for confirmation
 *   -H  hostname for the installed system  -u  also create this user
 *   -s  install from this tree instead of "/"
 *   -n  dry run: say what would happen, touch nothing
 *
 * This is the one tool in VerkOS that destroys data on purpose, so it is built
 * to refuse rather than guess: it will not touch a disk that is mounted, that
 * holds the filesystem it is running from, or that the operator has not named
 * out loud.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/mount.h>
#include <sys/ioctl.h>
#include <linux/fs.h>
#include <termios.h>

#define MNT      "/run/vinstall"
#define ESP_MB_DEFAULT 128

static int dry_run, assume_yes, verbose;
/* Where the system being installed is read from. "/" is the running system;
 * an ISO or a mounted image is just as valid a source, and is how this gets
 * tested without a live medium. */
static const char *source = "/";
static const char *passwd_file;      /* chpasswd-format lines for the new system */
static int encrypt_root;
/* Two ways to run a system that forgets itself. --immutable keeps the root on
 * disk but mounts it read-only under a tmpfs overlay, so only the changes live
 * in RAM. --ramdisk goes further: there is no root partition at all, the whole
 * system is an image on the EFI partition, and rd.toram lifts it into RAM so
 * the disk stops mattering once boot finishes. */
static int immutable_root, ramdisk_root, toram;
static const char *luks_file;        /* passphrase for an unattended install */
#define LUKS_NAME "cryptroot"
static int lock_root;
static const char *prog = "vinstall";

static void die(const char *fmt, ...) __attribute__((format(printf, 1, 2), noreturn));
static void die(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    fprintf(stderr, "\n%s: ", prog);
    vfprintf(stderr, fmt, ap);
    fputs("\n", stderr);
    va_end(ap);
    exit(1);
}
static void step(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void step(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    fputs("\033[1;36m==>\033[0m ", stdout);
    vprintf(fmt, ap);
    putchar('\n');
    fflush(stdout);
    va_end(ap);
}

/* ---- running a command, with its failure treated as ours ------------------ */
static int run_quiet(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static int run_quiet(const char *fmt, ...) {
    char cmd[4096];
    va_list ap; va_start(ap, fmt);
    vsnprintf(cmd, sizeof cmd, fmt, ap);
    va_end(ap);
    if (verbose) fprintf(stderr, "    + %s\n", cmd);
    if (dry_run) return 0;
    int rc = system(cmd);
    return (rc == -1) ? -1 : WEXITSTATUS(rc);
}
static void run(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void run(const char *fmt, ...) {
    char cmd[4096];
    va_list ap; va_start(ap, fmt);
    vsnprintf(cmd, sizeof cmd, fmt, ap);
    va_end(ap);
    if (verbose) fprintf(stderr, "    + %s\n", cmd);
    if (dry_run) return;
    int rc = system(cmd);
    if (rc == -1 || WEXITSTATUS(rc) != 0)
        die("command failed (exit %d): %s", rc == -1 ? -1 : WEXITSTATUS(rc), cmd);
}
/* Capture the first line of a command's output. */
static int capture(char *out, size_t cap, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static int capture(char *out, size_t cap, const char *fmt, ...) {
    char cmd[4096];
    va_list ap; va_start(ap, fmt);
    vsnprintf(cmd, sizeof cmd, fmt, ap);
    va_end(ap);
    FILE *f = popen(cmd, "r");
    if (!f) return -1;
    out[0] = 0;
    if (fgets(out, (int)cap, f)) out[strcspn(out, "\n")] = 0;
    int rc = pclose(f);
    return WEXITSTATUS(rc);
}

static int have_tool(const char *name) {
    char p[256];
    return capture(p, sizeof p, "command -v %s 2>/dev/null", name) == 0 && p[0];
}

/* ---- working out what we must not destroy --------------------------------- */
/* The disk a partition belongs to: /dev/sda2 -> /dev/sda, /dev/nvme0n1p2 ->
 * /dev/nvme0n1. sysfs knows the answer without us parsing names. */
static void parent_disk(const char *part, char *out, size_t cap) {
    const char *base = strrchr(part, '/');
    base = base ? base + 1 : part;
    char link[512];
    if (capture(link, sizeof link,
                "readlink -f /sys/class/block/%s/.. 2>/dev/null", base) == 0 && link[0]) {
        const char *d = strrchr(link, '/');
        if (d && strcmp(d + 1, "block")) { snprintf(out, cap, "/dev/%s", d + 1); return; }
    }
    snprintf(out, cap, "%s", part);
}

/* The device currently mounted on / — installing onto it would delete the
 * system doing the installing, halfway through. */
static void running_root_disk(char *out, size_t cap) {
    char dev[256];
    out[0] = 0;
    if (capture(dev, sizeof dev, "findmnt -no SOURCE / 2>/dev/null") != 0 || !dev[0]) {
        /* findmnt is util-linux; fall back to /proc/mounts */
        FILE *f = fopen("/proc/mounts", "r");
        if (!f) return;
        char src[256], mp[256];
        while (fscanf(f, "%255s %255s %*s %*s %*d %*d\n", src, mp) == 2)
            if (!strcmp(mp, "/")) { snprintf(dev, sizeof dev, "%s", src); break; }
        fclose(f);
    }
    if (dev[0] == '/') parent_disk(dev, out, cap);
}

static int device_is_mounted(const char *disk) {
    /* Any partition of the disk being mounted counts. */
    char p[256];
    return capture(p, sizeof p,
                   "grep -c '^%s' /proc/mounts 2>/dev/null", disk) == 0 && atoi(p) > 0;
}

static unsigned long disk_mb(const char *dev) {
    int fd = open(dev, O_RDONLY);
    if (fd < 0) return 0;
    unsigned long long bytes = 0;
    if (ioctl(fd, BLKGETSIZE64, &bytes)) bytes = 0;
    close(fd);
    return (unsigned long)(bytes / (1024 * 1024));
}

/* ---- the disk menu -------------------------------------------------------- */
static void list_disks(void) {
    printf("\nDisks this machine can see:\n\n");
    fflush(stdout);
    if (have_tool("lsblk"))
        run_quiet("lsblk -dno NAME,SIZE,MODEL 2>/dev/null | sed 's|^|  /dev/|'");
    else
        run_quiet("ls /sys/block | grep -vE '^(loop|ram|sr)' | sed 's|^|  /dev/|'");
    putchar('\n');
}

/* Partition names differ between /dev/sda2 and /dev/nvme0n1p2. */
static void part_name(const char *disk, int n, char *out, size_t cap) {
    size_t len = strlen(disk);
    int needs_p = len && (disk[len - 1] >= '0' && disk[len - 1] <= '9');
    snprintf(out, cap, "%s%s%d", disk, needs_p ? "p" : "", n);
}

static void wait_for_partition(const char *part) {
    if (dry_run) return;
    for (int i = 0; i < 50; i++) {          /* the kernel needs a moment */
        if (access(part, F_OK) == 0) return;
        usleep(100000);
    }
    die("%s never appeared after partitioning", part);
}

static void get_uuid(const char *dev, char *out, size_t cap) {
    if (dry_run) { snprintf(out, cap, "<uuid-of-%s>", dev); return; }
    if (capture(out, cap, "blkid -o value -s UUID %s 2>/dev/null", dev) != 0 || !out[0])
        die("cannot read the UUID of %s", dev);
}

/* ---- telling the firmware we exist --------------------------------------
 * Writing \EFI\BOOT\BOOTAA64.EFI is only correct on a disk we own: that path
 * is the removable-media fallback, and on a machine that already boots
 * something else it belongs to that something else. The right way to be
 * bootable alongside another system is a Boot#### variable, which is what
 * efibootmgr does and what we do here through efivarfs.
 *
 * The variable holds: attributes, the length of the device path, a UTF-16
 * description, then a device path naming the partition (by its GPT GUID, so it
 * survives being renumbered) and the file on it. */
#define EFI_GLOBAL_GUID "8be4df61-93ca-11d2-aa0d-00e098032b8c"
#define EFIVARS "/sys/firmware/efi/efivars"

static int efi_available(void) {
    struct stat st;
    return stat(EFIVARS, &st) == 0 && S_ISDIR(st.st_mode);
}

/* sysfs knows where a partition starts and how long it is, in 512-byte units. */
static unsigned long long sysfs_num(const char *part, const char *attr) {
    const char *base = strrchr(part, '/');
    base = base ? base + 1 : part;
    char out[64];
    if (capture(out, sizeof out, "cat /sys/class/block/%s/%s 2>/dev/null", base, attr) != 0)
        return 0;
    return strtoull(out, NULL, 10);
}

static void put_le16(unsigned char *p, unsigned v) { p[0] = v & 0xff; p[1] = (v >> 8) & 0xff; }
static void put_le32(unsigned char *p, unsigned long v) {
    p[0] = v & 0xff; p[1] = (v >> 8) & 0xff; p[2] = (v >> 16) & 0xff; p[3] = (v >> 24) & 0xff;
}
static void put_le64(unsigned char *p, unsigned long long v) {
    for (int i = 0; i < 8; i++) p[i] = (v >> (8 * i)) & 0xff;
}
/* ASCII to UTF-16LE, NUL terminated. Returns bytes written. */
static size_t put_utf16(unsigned char *p, const char *s) {
    size_t n = 0;
    for (; *s; s++) { put_le16(p + n, (unsigned char)*s); n += 2; }
    put_le16(p + n, 0); n += 2;
    return n;
}
/* A GPT PARTUUID is printed with the first three fields byte-swapped; the
 * device path wants the raw little-endian bytes back. */
static int guid_to_bytes(const char *uuid, unsigned char out[16]) {
    unsigned b[16];
    if (sscanf(uuid, "%2x%2x%2x%2x-%2x%2x-%2x%2x-%2x%2x-%2x%2x%2x%2x%2x%2x",
               &b[0],&b[1],&b[2],&b[3],&b[4],&b[5],&b[6],&b[7],
               &b[8],&b[9],&b[10],&b[11],&b[12],&b[13],&b[14],&b[15]) != 16) return -1;
    out[0]=b[3]; out[1]=b[2]; out[2]=b[1]; out[3]=b[0];
    out[4]=b[5]; out[5]=b[4];
    out[6]=b[7]; out[7]=b[6];
    for (int i = 8; i < 16; i++) out[i] = b[i];
    return 0;
}

/* efivarfs marks variables immutable; the flag has to come off before writing,
 * and the file has to be written in a single call including the attributes. */
static int efivar_write(const char *name, const unsigned char *data, size_t len) {
    char path[512];
    snprintf(path, sizeof path, "%s/%s-%s", EFIVARS, name, EFI_GLOBAL_GUID);
    int fd = open(path, O_RDONLY);
    if (fd >= 0) {
        int flags = 0;
        if (ioctl(fd, FS_IOC_GETFLAGS, &flags) == 0 && (flags & FS_IMMUTABLE_FL)) {
            flags &= ~FS_IMMUTABLE_FL;
            ioctl(fd, FS_IOC_SETFLAGS, &flags);
        }
        close(fd);
    }
    fd = open(path, O_WRONLY | O_CREAT, 0644);
    if (fd < 0) return -1;
    unsigned char buf[2048];
    if (len + 4 > sizeof buf) { close(fd); return -1; }
    /* NON_VOLATILE | BOOTSERVICE_ACCESS | RUNTIME_ACCESS */
    put_le32(buf, 0x00000007);
    memcpy(buf + 4, data, len);
    ssize_t w = write(fd, buf, len + 4);
    close(fd);
    return w == (ssize_t)(len + 4) ? 0 : -1;
}
static int efivar_read(const char *name, unsigned char *out, size_t cap) {
    char path[512];
    snprintf(path, sizeof path, "%s/%s-%s", EFIVARS, name, EFI_GLOBAL_GUID);
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    ssize_t n = read(fd, out, cap);
    close(fd);
    return n < 4 ? -1 : (int)(n - 4);        /* skip the attribute word */
}

/* Create a Boot#### entry for \EFI\verkos\<efi_name> on <esp_part> and put it
 * first in BootOrder. Returns 0 on success. */
static int efi_add_boot_entry(const char *esp_part, const char *efi_name, const char *label) {
    char partuuid[128];
    if (capture(partuuid, sizeof partuuid, "blkid -o value -s PARTUUID %s 2>/dev/null", esp_part) != 0
        || !partuuid[0]) return -1;
    unsigned char guid[16];
    if (guid_to_bytes(partuuid, guid)) return -1;

    unsigned long long start = sysfs_num(esp_part, "start");
    unsigned long long size  = sysfs_num(esp_part, "size");
    char pnum[32];
    if (capture(pnum, sizeof pnum, "cat /sys/class/block/%s/partition 2>/dev/null",
                strrchr(esp_part, '/') ? strrchr(esp_part, '/') + 1 : esp_part) != 0 || !pnum[0])
        return -1;
    if (!size) return -1;

    /* --- device path: HD() node, then the file, then the end marker --- */
    unsigned char dp[256];
    size_t n = 0;
    dp[n++] = 0x04; dp[n++] = 0x01;                  /* MEDIA_DEVICE_PATH, HARDDRIVE_DP */
    put_le16(dp + n, 42); n += 2;                    /* node length */
    put_le32(dp + n, (unsigned long)atoi(pnum)); n += 4;
    put_le64(dp + n, start); n += 8;
    put_le64(dp + n, size);  n += 8;
    memcpy(dp + n, guid, 16); n += 16;
    dp[n++] = 0x02;                                  /* MBRType: GPT */
    dp[n++] = 0x02;                                  /* SignatureType: GUID */

    char file[128];
    snprintf(file, sizeof file, "\\EFI\\verkos\\%s", efi_name);
    size_t flen = (strlen(file) + 1) * 2;
    dp[n++] = 0x04; dp[n++] = 0x04;                  /* MEDIA_DEVICE_PATH, FILEPATH_DP */
    put_le16(dp + n, (unsigned)(4 + flen)); n += 2;
    n += put_utf16(dp + n, file);

    dp[n++] = 0x7f; dp[n++] = 0xff;                  /* END_DEVICE_PATH */
    put_le16(dp + n, 4); n += 2;

    /* --- the load option itself --- */
    unsigned char opt[1024];
    size_t o = 0;
    put_le32(opt + o, 1); o += 4;                    /* LOAD_OPTION_ACTIVE */
    put_le16(opt + o, (unsigned)n); o += 2;          /* device path length */
    o += put_utf16(opt + o, label);
    memcpy(opt + o, dp, n); o += n;

    /* Find a free Boot#### slot rather than assuming one is spare. */
    char name[16] = "";
    for (int i = 0; i < 0x100; i++) {
        char cand[16], path[512];
        snprintf(cand, sizeof cand, "Boot%04X", i);
        snprintf(path, sizeof path, "%s/%s-%s", EFIVARS, cand, EFI_GLOBAL_GUID);
        if (access(path, F_OK) != 0) { snprintf(name, sizeof name, "%s", cand); break; }
    }
    if (!name[0]) return -1;
    if (efivar_write(name, opt, o)) return -1;

    /* Put it first in BootOrder, keeping whatever was there. */
    unsigned char order[512];
    int have = efivar_read("BootOrder", order, sizeof order);
    if (have < 0) have = 0;
    unsigned char neworder[512];
    unsigned slot = (unsigned)strtoul(name + 4, NULL, 16);
    put_le16(neworder, slot);
    size_t on = 2;
    for (int i = 0; i + 1 < have && on + 2 <= sizeof neworder; i += 2) {
        unsigned existing = order[4 + i] | (order[5 + i] << 8);
        if (existing == slot) continue;              /* no duplicates */
        put_le16(neworder + on, existing); on += 2;
    }
    return efivar_write("BootOrder", neworder, on);
}

/* ---- passwords -----------------------------------------------------------
 * The source system's /etc/shadow is copied with everything else, so without
 * this the installed machine silently inherits whatever root password the image
 * was built with — which for VerkOS is the documented development default. */
static char *ask_hidden(const char *prompt) {
    static char buf[256];
    struct termios old, new;
    int tty = isatty(STDIN_FILENO);
    fputs(prompt, stdout); fflush(stdout);
    if (tty && tcgetattr(STDIN_FILENO, &old) == 0) {
        new = old; new.c_lflag &= ~(unsigned)ECHO; new.c_lflag |= ECHONL;
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &new);
    }
    char *r = fgets(buf, sizeof buf, stdin);
    if (tty) tcsetattr(STDIN_FILENO, TCSAFLUSH, &old);
    if (!r) return NULL;
    buf[strcspn(buf, "\n")] = 0;
    return buf;
}

/* Ask twice and only accept a match — a mistyped password on a machine you are
 * about to reboot into is not recoverable without a rescue medium. */
static int ask_password_twice(const char *who, char *out, size_t cap) {
    for (int try = 0; try < 3; try++) {
        char first[256];
        char prompt[128];
        snprintf(prompt, sizeof prompt, "Password for %s: ", who);
        char *a = ask_hidden(prompt);
        if (!a) return -1;
        if (strlen(a) < 6) { printf("  at least 6 characters, please\n"); continue; }
        snprintf(first, sizeof first, "%s", a);
        char *b = ask_hidden("Repeat it: ");
        if (!b) return -1;
        if (strcmp(first, b)) { printf("  they do not match\n"); continue; }
        snprintf(out, cap, "%s", first);
        memset(first, 0, sizeof first);
        return 0;
    }
    return -1;
}

/* Applied with the target's own chpasswd, so the hashing is whatever the
 * installed system itself would produce. */
static void set_passwords(const char *root, const char *user) {
    char path[512];
    snprintf(path, sizeof path, "%s/run-vinstall-pw", root);

    if (passwd_file) {
        run("cp %s %s", passwd_file, path);
    } else if (!dry_run) {
        FILE *f = fopen(path, "w");
        if (!f) die("cannot write %s: %s", path, strerror(errno));
        if (!lock_root) {
            char pw[256];
            if (ask_password_twice("root on the installed system", pw, sizeof pw))
                { fclose(f); unlink(path); die("no root password set — refusing to install one that is already public"); }
            fprintf(f, "root:%s\n", pw);
            memset(pw, 0, sizeof pw);
        }
        if (user) {
            char pw[256];
            if (ask_password_twice(user, pw, sizeof pw)) { fclose(f); unlink(path); die("no password for %s", user); }
            fprintf(f, "%s:%s\n", user, pw);
            memset(pw, 0, sizeof pw);
        }
        fclose(f);
        chmod(path, 0600);
    }
    if (dry_run) return;
    /* The redirection is performed by the shell on this side of the chroot, so
     * the path has to be the one this side can see. */
    run("chroot %s /usr/bin/chpasswd < %s/run-vinstall-pw", root, root);
    if (lock_root) run("chroot %s /usr/bin/passwd -l root >/dev/null", root);
    unlink(path);
}

/* Everything the installed system needs to boot must be readable before the
 * target disk is touched. Discovering a missing bootloader after partitioning,
 * formatting and copying leaves the operator with a wiped disk and half an
 * install — which is exactly what the first end-to-end run did. */
static void check_source(const char *arch) {
    const char *sroot = strcmp(source, "/") ? source : "";
    const char *efi_name = !strcmp(arch, "aarch64") ? "BOOTAA64.EFI" : "BOOTX64.EFI";
    char p[768];

    snprintf(p, sizeof p, "%s/sbin/urverk", sroot);
    if (access(p, R_OK) != 0) die("%s is not a VerkOS system (no %s)", sroot[0] ? sroot : "/", p);

    snprintf(p, sizeof p, "%s/boot/EFI/BOOT/%s", sroot, efi_name);
    if (access(p, R_OK) != 0)
        die("no bootloader at %s\n"
            "       The EFI system partition has to be mounted at %s/boot.\n"
            "       On the live medium: mount /dev/vdb1 %s/boot",
            p, sroot[0] ? sroot : "", sroot[0] ? sroot : "");

    snprintf(p, sizeof p, "%s/boot/vmlinuz", sroot);
    if (access(p, R_OK) != 0) die("no kernel at %s", p);

    snprintf(p, sizeof p, "%s/boot/initramfs.img", sroot);
    if (access(p, R_OK) != 0) die("no initramfs at %s", p);
}

/* A device we were handed must be a partition, not a whole disk, not mounted,
 * and not the one this system is running from. Naming the wrong thing here
 * destroys somebody's data, so check rather than assume. */
static void check_existing_part(const char *dev, const char *role) {
    struct stat st;
    if (stat(dev, &st)) die("%s (%s): %s", dev, role, strerror(errno));
    if (!S_ISBLK(st.st_mode)) die("%s (%s) is not a block device", dev, role);
    const char *base = strrchr(dev, '/');
    base = base ? base + 1 : dev;
    char out[64];
    if (capture(out, sizeof out, "cat /sys/class/block/%s/partition 2>/dev/null", base) != 0 || !out[0])
        die("%s (%s) is a whole disk, not a partition", dev, role);
    if (capture(out, sizeof out, "grep -c '^%s ' /proc/mounts 2>/dev/null", dev) == 0 && atoi(out) > 0)
        die("%s (%s) is mounted — unmount it first", dev, role);
    char running[256];
    if (capture(running, sizeof running, "findmnt -no SOURCE / 2>/dev/null") == 0 &&
        !strcmp(running, dev))
        die("%s (%s) is the filesystem this system is running from", dev, role);
}

/* ---- confirmation --------------------------------------------------------- */
static void confirm(const char *disk, unsigned esp_mb, unsigned swap_mb,
                    unsigned home_mb, const char *hostname) {
    printf("\n\033[1mAbout to install VerkOS onto %s\033[0m\n\n", disk);
    printf("  %-22s %s\n", "target disk", disk);
    printf("  %-22s GPT: %u MB EFI system partition", "layout", esp_mb);
    if (home_mb) printf(", %u MB /home", home_mb);
    if (swap_mb) printf(", %u MB swap", swap_mb);
    printf(", ext4 root on the rest\n");
    printf("  %-22s %s\n", "hostname", hostname);
    printf("  %-22s vboot (ours), booting urverk\n", "bootloader");
    printf("\nWhat is on that disk now:\n\n");
    fflush(stdout);
    if (have_tool("lsblk")) run_quiet("lsblk -no NAME,SIZE,FSTYPE,LABEL %s 2>/dev/null | sed 's/^/  /'", disk);
    else run_quiet("blkid | grep '^%s' | sed 's/^/  /'", disk);

    printf("\n\033[1;31mEverything on %s will be destroyed.\033[0m\n", disk);
    if (assume_yes) { printf("Proceeding (-y given).\n\n"); return; }
    /* Typing the device name, not "y": the point is to be sure it is the disk
     * they meant, and "y" answers a question nobody misreads carefully. */
    printf("Type the disk name to confirm (%s), or anything else to abort: ", disk);
    fflush(stdout);
    char answer[256] = "";
    if (!fgets(answer, sizeof answer, stdin)) die("aborted");
    answer[strcspn(answer, "\n")] = 0;
    if (strcmp(answer, disk)) die("aborted — nothing has been changed");
    putchar('\n');
}

/* The same warning as confirm(), for the case where we were handed partitions
 * rather than a disk: what gets erased is different, so say so precisely. */
static void confirm_parts(const char *root_part, const char *esp_part,
                          const char *home_part, const char *swap_part,
                          const char *hostname) {
    printf("\n\033[1mAbout to install VerkOS onto existing partitions\033[0m\n\n");
    printf("  %-22s %s\n", "hostname", hostname);
    printf("\n\033[1;31mThese will be erased:\033[0m\n");
    printf("    %s   (root filesystem)\n", root_part);
    if (home_part) printf("    %s   (/home)\n", home_part);
    if (swap_part) printf("    %s   (swap)\n", swap_part);
    printf("\n\033[1mThese will be kept, and only added to:\033[0m\n");
    printf("    %s   (EFI system partition — its filesystem and any other\n", esp_part);
    printf("        %*s bootloader on it are left as they are)\n", (int)strlen(esp_part), "");
    printf("\nNothing else on the disk is touched: the partition table is not rewritten.\n");
    if (assume_yes) { printf("Proceeding (-y given).\n\n"); return; }
    printf("\nType the root partition to confirm (%s), or anything else to abort: ", root_part);
    fflush(stdout);
    char answer[256] = "";
    if (!fgets(answer, sizeof answer, stdin)) die("aborted");
    answer[strcspn(answer, "\n")] = 0;
    if (strcmp(answer, root_part)) die("aborted — nothing has been changed");
    putchar('\n');
}

/* ---- the install ---------------------------------------------------------- */
static void copy_system(const char *root) {
    /* Copy the source system, minus everything that is generated at boot or
     * belongs to the installer. The target itself is under /run, which is
     * excluded, but be explicit rather than rely on that. */
    /* "boot" is deliberate: the target's EFI partition is mounted there, and
     * install_bootloader fills it on purpose. Copying the source's /boot over
     * it drops the live medium's vboot.conf onto the new ESP, and vboot reads
     * that in preference to the one written for this install — the installed
     * system then boots with the live system's root= and lands in the
     * emergency shell. */
    static const char *skip[] = { "proc", "sys", "dev", "run", "tmp", "mnt", "media",
                                  "boot", "lost+found", NULL };
    DIR *d = opendir(source);
    if (!d) die("cannot read %s: %s", source, strerror(errno));
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        int skipped = 0;
        for (const char **s = skip; *s; s++) if (!strcmp(e->d_name, *s)) skipped = 1;
        if (skipped) continue;
        if (verbose) printf("    copying /%s\n", e->d_name);
        /* cp -a keeps ownership, timestamps, symlinks and hard links; a plain
         * recursive copy would silently flatten all four. */
        run("cp -a %s/%s %s/ 2>/dev/null || true", strcmp(source, "/") ? source : "",
            e->d_name, root);
    }
    closedir(d);
    /* The mount points we skipped still have to exist on the target. */
    for (const char **s = skip; *s; s++)
        run("mkdir -p %s/%s", root, *s);
    run("chmod 1777 %s/tmp", root);
}

static void write_fstab(const char *root, const char *root_uuid, const char *esp_uuid,
                        const char *home_uuid, const char *swap_uuid, int encrypted) {
    if (dry_run) return;
    char path[512];
    snprintf(path, sizeof path, "%s/etc/fstab", root);
    FILE *f = fopen(path, "w");
    if (!f) die("cannot write %s: %s", path, strerror(errno));
    /* UUID= would be resolved by scanning raw block devices, and the mapper
     * device is not one; name it directly. */
    char rootline[192];
    if (encrypted) snprintf(rootline, sizeof rootline, "/dev/mapper/%s", LUKS_NAME);
    else snprintf(rootline, sizeof rootline, "UUID=%s", root_uuid);
    /* With an overlay the root urverk hands over is the overlay, not the disk;
     * saying "ro" here documents that the medium underneath is never written. */
    const char *rootopts = immutable_root ? "ro,noatime" : "defaults,noatime";
    fprintf(f,
        "# Written by vinstall. urverk mounts these at boot.\n"
        "# device                                  mount   type  options              dump fsck\n"
        "%-41s /       ext4  %-20s 0    1\n"
        "UUID=%-36s /boot   vfat  defaults,noatime     0    2\n",
        rootline, rootopts, esp_uuid);
    if (home_uuid && *home_uuid)
        fprintf(f, "UUID=%-36s /home   ext4  defaults,noatime     0    2\n", home_uuid);
    if (swap_uuid && *swap_uuid)
        fprintf(f, "UUID=%-36s none    swap  sw                   0    0\n", swap_uuid);
    fclose(f);
}

/* Where the EFI partition is mounted. Normally inside the new root, but a
 * diskless install has no root partition — the ESP is the only filesystem
 * there is, and the system lives on it as a file. */
static char esp_dir[512];

static void install_bootloader(const char *root, const char *arch, const char *root_uuid,
                               int own_disk, const char *luks_uuid, const char *extra_cmdline) {
    char esp[512];
    if (esp_dir[0]) snprintf(esp, sizeof esp, "%s", esp_dir);
    else snprintf(esp, sizeof esp, "%s/boot", root);
    run("mkdir -p %s/EFI/BOOT %s/EFI/verkos", esp, esp);

    /* vboot itself, under the name the firmware looks for with no NVRAM entry. */
    const char *efi_name = !strcmp(arch, "aarch64") ? "BOOTAA64.EFI" : "BOOTX64.EFI";
    const char *sroot = strcmp(source, "/") ? source : "";
    char src[768];
    snprintf(src, sizeof src, "%s/boot/EFI/BOOT/%s", sroot, efi_name);
    if (access(src, R_OK) != 0 && !dry_run)
        die("cannot find %s — the EFI partition must be mounted at %s/boot", src, sroot);

    /* Our own copy always goes here; the firmware entry points at it. */
    run("cp %s %s/EFI/verkos/%s", src, esp, efi_name);
    /* \EFI\BOOT\ is the removable-media fallback path. On a disk we
     * partitioned it is ours. On a disk we were only handed a partition of, it
     * probably belongs to whatever else boots from here — overwriting it would
     * make that system unbootable, which is not ours to do. */
    char fallback[1024];
    snprintf(fallback, sizeof fallback, "%s/EFI/BOOT/%s", esp, efi_name);
    if (own_disk || access(fallback, F_OK) != 0)
        run("cp %s %s/EFI/BOOT/%s", src, esp, efi_name);
    else
        printf("    leaving EFI/BOOT/%s alone — another system installed it\n", efi_name);

    /* Kernel and initramfs. Both live on the source ESP; without them the
     * installed system has nothing for vboot to load. */
    if (!dry_run) {
        char probe[768];
        snprintf(probe, sizeof probe, "%s/boot/vmlinuz", sroot);
        if (access(probe, R_OK) != 0)
            die("no kernel at %s — is the EFI partition mounted at %s/boot?", probe, sroot);
    }
    run("cp %s/boot/vmlinuz* %s/ 2>/dev/null || true", sroot, esp);
    run("cp %s/boot/initramfs* %s/ 2>/dev/null || true", sroot, esp);

    if (dry_run) return;
    /* vboot reads \vboot.conf from the root of the ESP it was loaded from
     * (pkg/vboot/vboot.c, CONFIG_PATH). Anywhere else is ignored. */
    char conf[1024];
    snprintf(conf, sizeof conf, "%s/vboot.conf", esp);
    FILE *f = fopen(conf, "w");
    if (!f) die("cannot write %s: %s", conf, strerror(errno));
    /* vboot's own syntax, not systemd-boot's: the keys are kernel/initrd/cmdline
     * and the paths are EFI paths with backslashes. Getting this wrong is quiet
     * — vboot falls back to a kernel it finds on the ESP, so the machine still
     * boots, with no initrd and an empty command line, and panics mounting root.
     * Keep it matching pkg/vboot/vboot.c and config/esp/vboot.conf. */
    const char *console = !strcmp(arch, "aarch64") ? "ttyAMA0" : "ttyS0";
    /* With an encrypted root the kernel cannot be told to mount it directly:
     * the initramfs has to find the container, ask for the passphrase and open
     * it first, and only then is there a device to mount. */
    char rootspec[256], luksarg[192] = "";
    const char *extra = extra_cmdline ? extra_cmdline : "";
    if (luks_uuid && *luks_uuid) {
        snprintf(rootspec, sizeof rootspec, "/dev/mapper/%s", LUKS_NAME);
        snprintf(luksarg, sizeof luksarg, "rd.luks.uuid=%s rd.luks.name=%s ", luks_uuid, LUKS_NAME);
    } else {
        snprintf(rootspec, sizeof rootspec, "UUID=%s", root_uuid);
    }
    /* Diskless: root= names the EFI partition, and the real system is the
     * image file sitting on it. */
    if (ramdisk_root) snprintf(rootspec, sizeof rootspec, "UUID=%s", root_uuid);
    fprintf(f,
        "# Written by vinstall.\n"
        "timeout 5\n"
        "default 0\n"
        "\n"
        "entry VerkOS\n"
        "  kernel  \\vmlinuz\n"
        "  initrd  \\initramfs.img\n"
        "  cmdline %sroot=%s rw console=%s init=/sbin/urverk%s\n"
        "\n"
        "entry VerkOS (verbose boot)\n"
        "  kernel  \\vmlinuz\n"
        "  initrd  \\initramfs.img\n"
        "  cmdline %sroot=%s rw console=%s init=/sbin/urverk%s urverk.debug\n"
        "\n"
        "entry VerkOS (rescue shell)\n"
        "  kernel  \\vmlinuz\n"
        "  initrd  \\initramfs.img\n"
        "  cmdline %sroot=%s rw console=%s init=/sbin/urverk%s urverk.rescue\n",
        luksarg, rootspec, console, extra,
        luksarg, rootspec, console, extra,
        luksarg, rootspec, console, extra);
    fclose(f);
}

static void set_hostname(const char *root, const char *hostname) {
    if (dry_run) return;
    char path[512];
    snprintf(path, sizeof path, "%s/etc/hostname", root);
    FILE *f = fopen(path, "w");
    if (!f) return;
    fprintf(f, "%s\n", hostname);
    fclose(f);
}

static void usage(void) {
    printf("vinstall — install VerkOS onto a disk\n\n"
           "  -t DISK      target disk (e.g. /dev/sda); asks if omitted\n"
           "  -y           do not ask for confirmation\n"
           "  -H NAME      hostname for the installed system (default: verkos)\n"
           "  -u NAME      create this user on the installed system\n"
           "  -s DIR       install from this tree instead of the running system\n"
           "  --esp MB     size of the EFI system partition (default: %d)\n"
           "  --swap MB    also create a swap partition\n"
           "  --home MB    put /home on its own partition\n"
           "  --passwd-file F  set passwords from USER:PASSWORD lines (for -y)\n"
           "  --lock-root  install with the root account locked (needs -u)\n"
           "\n"
           "  Installing alongside another system, without repartitioning:\n"
           "  --root-part DEV  format this partition and install onto it\n"
           "  --esp-part DEV   use this existing EFI partition (never formatted)\n"
           "  --home-part DEV  --swap-part DEV   optional, formatted\n"
           "  --no-efi-entry   do not add a boot entry to the firmware\n"
           "\n"
           "  --immutable      root read-only under a RAM overlay; changes are\n"
           "                   forgotten at reboot, only the changes cost RAM\n"
           "  --ramdisk        no root partition: the system is an image on the\n"
           "                   EFI partition, copied into RAM at boot\n"
           "  --ramdisk-from-disk   as --ramdisk, but read the image in place\n"
           "                   rather than copying it (far less RAM)\n"
           "  --encrypt        put the root filesystem inside a LUKS2 container\n"
           "  --luks-passphrase-file F   read the passphrase from F (for -y)\n"
           "  -n           dry run: print what would happen, change nothing\n"
           "  -v           show every command\n", ESP_MB_DEFAULT);
}

int main(int argc, char **argv) {
    const char *disk = NULL, *hostname = "verkos", *user = NULL;
    const char *given_root = NULL, *given_esp = NULL, *given_home = NULL, *given_swap = NULL;
    int no_efi_entry = 0;
    unsigned esp_mb = ESP_MB_DEFAULT, swap_mb = 0, home_mb = 0;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-t") && i + 1 < argc) disk = argv[++i];
        else if (!strcmp(argv[i], "-y")) assume_yes = 1;
        else if (!strcmp(argv[i], "-n")) dry_run = 1;
        else if (!strcmp(argv[i], "-v")) verbose = 1;
        else if (!strcmp(argv[i], "-H") && i + 1 < argc) hostname = argv[++i];
        else if (!strcmp(argv[i], "-u") && i + 1 < argc) user = argv[++i];
        else if (!strcmp(argv[i], "-s") && i + 1 < argc) source = argv[++i];
        else if (!strcmp(argv[i], "--esp") && i + 1 < argc) esp_mb = (unsigned)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--swap") && i + 1 < argc) swap_mb = (unsigned)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--home") && i + 1 < argc) home_mb = (unsigned)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--passwd-file") && i + 1 < argc) passwd_file = argv[++i];
        else if (!strcmp(argv[i], "--lock-root")) lock_root = 1;
        else if (!strcmp(argv[i], "--root-part") && i + 1 < argc) given_root = argv[++i];
        else if (!strcmp(argv[i], "--esp-part") && i + 1 < argc) given_esp = argv[++i];
        else if (!strcmp(argv[i], "--home-part") && i + 1 < argc) given_home = argv[++i];
        else if (!strcmp(argv[i], "--swap-part") && i + 1 < argc) given_swap = argv[++i];
        else if (!strcmp(argv[i], "--no-efi-entry")) no_efi_entry = 1;
        else if (!strcmp(argv[i], "--encrypt")) encrypt_root = 1;
        else if (!strcmp(argv[i], "--immutable")) immutable_root = 1;
        else if (!strcmp(argv[i], "--ramdisk")) { ramdisk_root = 1; toram = 1; }
        else if (!strcmp(argv[i], "--ramdisk-from-disk")) ramdisk_root = 1;
        else if (!strcmp(argv[i], "--luks-passphrase-file") && i + 1 < argc) luks_file = argv[++i];
        else { usage(); return strcmp(argv[i], "-h") && strcmp(argv[i], "--help"); }
    }
    if (getuid() != 0) die("must be run as root");

    /* Everything this needs must exist before anything is destroyed. */
    const char *needed[] = { "sfdisk", "blkid", "mkfs.ext4", NULL };
    for (const char **t = needed; *t; t++)
        if (!have_tool(*t)) die("%s is not installed — cannot continue", *t);
    const char *mkvfat = have_tool("vmkfs.vfat") ? "vmkfs.vfat"
                       : have_tool("mkfs.vfat")  ? "mkfs.vfat" : NULL;
    if (!mkvfat) die("no FAT formatter (vmkfs.vfat) — cannot create an EFI partition");

    char arch[64] = "aarch64";
    capture(arch, sizeof arch, "uname -m");

    /* Unattended and with nothing said about passwords, the installed system
     * would keep the source image's root password. Refuse rather than ship a
     * machine whose password is in a public repository. */
    if (assume_yes && !passwd_file && !lock_root && !isatty(STDIN_FILENO))
        die("no way to set a password: pass --passwd-file FILE, or --lock-root with -u.\n"
            "       Without one of those the installed system keeps the source system's\n"
            "       root password, which for a VerkOS image is the development default.");
    if (ramdisk_root && immutable_root)
        die("--ramdisk already forgets everything; --immutable adds nothing");
    if (ramdisk_root && encrypt_root)
        die("--ramdisk has no root partition to encrypt.\n"
            "       The system image would sit unencrypted on the EFI partition,\n"
            "       which is not what asking for encryption means.");
    if (ramdisk_root && (swap_mb || home_mb || given_home || given_swap))
        die("--ramdisk keeps nothing between boots, so swap and /home have nowhere to live");
    if (lock_root && !user)
        die("--lock-root leaves no way in: create an account with -u NAME as well");

    /* Two ways in: take a whole disk, or be handed partitions to use. */
    int own_disk = (given_root == NULL);
    if (!own_disk) {
        if (!given_esp) die("--root-part needs --esp-part as well (the firmware has to find us)");
        if (swap_mb || home_mb)
            die("--swap/--home make partitions; with --root-part pass --swap-part/--home-part instead");
        disk = given_root;                 /* only used for messages from here on */
    }

    if (own_disk && !disk) {
        list_disks();
        printf("Install onto which disk? ");
        fflush(stdout);
        static char buf[256];
        if (!fgets(buf, sizeof buf, stdin)) return 1;
        buf[strcspn(buf, "\n")] = 0;
        if (!buf[0]) die("no disk given");
        disk = buf;
    }

    struct stat st;
    if (own_disk) {
        if (stat(disk, &st)) die("%s: %s", disk, strerror(errno));
        if (!S_ISBLK(st.st_mode)) die("%s is not a block device", disk);
        char running[256];
        running_root_disk(running, sizeof running);
        if (running[0] && !strcmp(running, disk))
            die("%s is the disk this system is running from", disk);
        if (device_is_mounted(disk))
            die("%s has mounted partitions — unmount them first", disk);
    } else {
        check_existing_part(given_root, "root");
        check_existing_part(given_esp, "EFI system partition");
        if (given_home) check_existing_part(given_home, "home");
        if (given_swap) check_existing_part(given_swap, "swap");
        /* Using a partition that is not FAT as an ESP produces a system the
         * firmware cannot boot, discovered only at the next reboot. */
        char t[64];
        if (capture(t, sizeof t, "blkid -o value -s TYPE %s 2>/dev/null", given_esp) != 0 ||
            strcmp(t, "vfat"))
            die("%s is %s, not a FAT filesystem — that cannot be an EFI system partition",
                given_esp, t[0] ? t : "unformatted");
    }

    if (!dry_run) check_source(arch);
    if (own_disk) confirm(disk, esp_mb, swap_mb, home_mb, hostname);
    else confirm_parts(given_root, given_esp, given_home, given_swap, hostname);

    char esp_part[256], root_part[256], home_part[256] = "", swap_part[256] = "";
    if (own_disk) {
        part_name(disk, 1, esp_part, sizeof esp_part);
        part_name(disk, 2, root_part, sizeof root_part);
        /* /home and swap are numbered in the order they are written above. */
        if (home_mb) part_name(disk, 3, home_part, sizeof home_part);
        if (swap_mb) part_name(disk, home_mb ? 4 : 3, swap_part, sizeof swap_part);
    } else {
        snprintf(esp_part, sizeof esp_part, "%s", given_esp);
        snprintf(root_part, sizeof root_part, "%s", given_root);
        if (given_home) snprintf(home_part, sizeof home_part, "%s", given_home);
        if (given_swap) snprintf(swap_part, sizeof swap_part, "%s", given_swap);
    }

    if (own_disk) {
    step("partitioning %s", disk);
    run("wipefs -a %s >/dev/null 2>&1 || true", disk);
    /* One ESP and one root, both carrying the standard GPT type GUIDs so other
     * systems (and the firmware) recognise what they are looking at. The root
     * partition takes whatever is left. */
    if (!dry_run) {
        /* Sizes are worked out here rather than left to sfdisk, because only the
         * last partition can say "the rest" and root is not last once /home or
         * swap exist. */
        unsigned long total = disk_mb(disk);
        if (!total) die("cannot determine the size of %s", disk);
        unsigned long fixed = esp_mb + swap_mb + home_mb + 2;   /* 2 MiB alignment slack */
        if (total <= fixed + 512)
            die("%s is %lu MB — too small for a %u MB ESP%s%s plus a root filesystem",
                disk, total, esp_mb,
                swap_mb ? " , swap" : "", home_mb ? " and /home" : "");
        unsigned long root_mb = total - fixed;

        FILE *sf = fopen("/run/vinstall.sfdisk", "w");
        if (!sf) die("cannot write the partition script: %s", strerror(errno));
        fprintf(sf, "label: gpt\n");
        if (ramdisk_root) {
            /* One partition. There is nothing to put in a second one: the whole
             * system is a file on this one, and nothing is kept between boots. */
            fprintf(sf, "start=1MiB, type=C12A7328-F81F-11D2-BA4B-00A0C93EC93B, name=\"EFI System\"\n");
        } else {
            fprintf(sf, "start=1MiB, size=%uMiB, type=C12A7328-F81F-11D2-BA4B-00A0C93EC93B, name=\"EFI System\"\n",
                    esp_mb);
            fprintf(sf, "size=%luMiB, type=0FC63DAF-8483-4772-8E79-3D69D8477DE4, name=\"VerkOS\"\n", root_mb);
        }
        if (home_mb)
            fprintf(sf, "size=%uMiB, type=933AC7E1-2EB4-4F13-B844-0E14E2AEF915, name=\"home\"\n", home_mb);
        if (swap_mb)
            fprintf(sf, "size=%uMiB, type=0657FD6D-A4AB-43C4-84E5-0933C84B4F4F, name=\"swap\"\n", swap_mb);
        fclose(sf);
    }
    run("sfdisk --quiet --wipe always %s < /run/vinstall.sfdisk", disk);
    run("partx -u %s >/dev/null 2>&1 || partprobe %s >/dev/null 2>&1 || true", disk, disk);
    unlink("/run/vinstall.sfdisk");
    wait_for_partition(esp_part);
    if (!ramdisk_root) wait_for_partition(root_part);
    }

    step("creating filesystems");
    /* An ESP we were given may hold another system's bootloader; formatting it
     * would delete it. Only the one we just created is ours to format. */
    if (own_disk) run("%s -n VERKESP %s", mkvfat, esp_part);
    else printf("    keeping the filesystem on %s\n", esp_part);
    /* The container has to exist before there is a filesystem to put in it, so
     * this sits between partitioning and mkfs. Everything downstream then uses
     * the mapper device and never touches the raw partition again. */
    char fs_dev[256];
    snprintf(fs_dev, sizeof fs_dev, "%s", root_part);
    char luks_uuid[128] = "";
    if (encrypt_root) {
        if (!have_tool("cryptsetup"))
            die("cryptsetup is not installed — cannot create an encrypted root");
        step("encrypting %s", root_part);
        if (luks_file) {
            run("cryptsetup luksFormat --type luks2 -q --key-file %s %s", luks_file, root_part);
            run("cryptsetup luksOpen --key-file %s %s %s", luks_file, root_part, LUKS_NAME);
        } else if (!dry_run) {
            char pass[256];
            if (ask_password_twice("the encrypted disk", pass, sizeof pass))
                die("no passphrase given — nothing has been written to the filesystem yet");
            char pf[256];
            snprintf(pf, sizeof pf, "/run/vinstall-luks");
            FILE *f = fopen(pf, "w");
            if (!f) die("cannot write %s: %s", pf, strerror(errno));
            chmod(pf, 0600);
            fputs(pass, f);
            fclose(f);
            memset(pass, 0, sizeof pass);
            run("cryptsetup luksFormat --type luks2 -q --key-file %s %s", pf, root_part);
            run("cryptsetup luksOpen --key-file %s %s %s", pf, root_part, LUKS_NAME);
            unlink(pf);
        }
        snprintf(fs_dev, sizeof fs_dev, "/dev/mapper/%s", LUKS_NAME);
        /* The container's own UUID is what the initramfs looks for; the
         * filesystem inside has a different one. */
        if (!dry_run &&
            (capture(luks_uuid, sizeof luks_uuid, "cryptsetup luksUUID %s 2>/dev/null", root_part) != 0
             || !luks_uuid[0]))
            die("could not read the LUKS UUID of %s", root_part);
    }

    /* Diskless: there is no root partition to format. The system image is
     * created later, on the EFI partition itself. */
    if (!ramdisk_root) run("mkfs.ext4 -q -F -L VERKOS %s", fs_dev);
    if (home_part[0]) { wait_for_partition(home_part); run("mkfs.ext4 -q -F -L VERKHOME %s", home_part); }
    if (swap_part[0]) { wait_for_partition(swap_part); run("mkswap -L VERKSWAP %s >/dev/null", swap_part); }

    char root_uuid[128], esp_uuid[128];
    if (!ramdisk_root) get_uuid(fs_dev, root_uuid, sizeof root_uuid);
    get_uuid(esp_part, esp_uuid, sizeof esp_uuid);
    char home_uuid[128] = "", swap_uuid[128] = "";
    if (home_part[0]) get_uuid(home_part, home_uuid, sizeof home_uuid);
    if (swap_part[0]) get_uuid(swap_part, swap_uuid, sizeof swap_uuid);

    if (ramdisk_root) {
        /* No root partition to mount. Build the system as one image on the EFI
         * partition instead: mkfs.ext4 -d copies the tree in as it creates the
         * filesystem, so there is no separate copy step at all. */
        step("building the system image on the EFI partition");
        snprintf(esp_dir, sizeof esp_dir, "%s-esp", MNT);
        run("mkdir -p %s %s", esp_dir, MNT);
        run("mount %s %s", esp_part, esp_dir);

        const char *sroot = strcmp(source, "/") ? source : "";
        char sz[64] = "";
        if (!dry_run) {
            if (capture(sz, sizeof sz, "du -sxm %s 2>/dev/null | cut -f1", sroot[0] ? sroot : "/") != 0 || !sz[0])
                die("cannot measure the system to be imaged");
        }
        unsigned long content = (unsigned long)atol(sz[0] ? sz : "1024");
        unsigned long img_mb = content + content / 4 + 256;   /* a quarter of slack */
        printf("    %lu MB of system, image sized at %lu MB\n", content, img_mb);
        run("truncate -s %luM %s/rootfs.img", img_mb, esp_dir);
        /* Empty, then filled by copy_system below. "mkfs.ext4 -d" would be
         * neater but has no way to exclude anything, and the image lives on a
         * filesystem mounted inside the tree being copied — so it would copy
         * itself into itself and run out of blocks. copy_system already knows
         * what to skip, and /run is on its list. */
        run("mkfs.ext4 -q -F -m 0 -L VERKOS %s/rootfs.img", esp_dir);
        run("mount -o loop %s/rootfs.img %s", esp_dir, MNT);
    } else {
    step("mounting the new system on %s", MNT);
    run("mkdir -p %s", MNT);
    run("mount %s %s", fs_dev, MNT);
    run("mkdir -p %s/boot", MNT);
    run("mount %s %s/boot", esp_part, MNT);
    if (home_part[0]) { run("mkdir -p %s/home", MNT); run("mount %s %s/home", home_part, MNT); }
    }

    step("copying the system");
    copy_system(MNT);

    step("installing vboot and writing the boot configuration");
    /* An immutable install differs from an ordinary one by exactly one word on
     * the kernel command line: urverk stacks a tmpfs over the root it just
     * mounted, and every write lands there instead of on the disk. */
    char extra[128] = "";
    if (immutable_root) snprintf(extra, sizeof extra, " rd.overlay");
    if (ramdisk_root)
        snprintf(extra, sizeof extra, " rootfstype=vfat rd.image=/rootfs.img rd.overlay%s",
                 toram ? " rd.toram" : "");
    install_bootloader(MNT, arch, ramdisk_root ? esp_uuid : root_uuid, own_disk, luks_uuid, extra);
    if (!no_efi_entry && efi_available() && !dry_run) {
        const char *efi_name = !strcmp(arch, "aarch64") ? "BOOTAA64.EFI" : "BOOTX64.EFI";
        if (efi_add_boot_entry(esp_part, efi_name, "VerkOS") == 0)
            printf("    added a VerkOS entry to the firmware boot menu\n");
        else
            printf("    could not add a firmware boot entry — this disk boots only "
                   "if the firmware tries \\EFI\\BOOT\n");
    }

    step("configuring the installed system");
    if (ramdisk_root) {
        /* Nothing in this system is on a partition of its own: the root is a
         * file that urverk loop-mounts, and /boot is where that file lives. */
        if (!dry_run) {
            char path[512];
            snprintf(path, sizeof path, "%s/etc/fstab", MNT);
            FILE *f = fopen(path, "w");
            if (f) {
                fprintf(f,
                    "# Written by vinstall for a diskless install.\n"
                    "# The root is an image on the EFI partition, mounted by urverk\n"
                    "# before this file is read, with a RAM overlay on top. Nothing\n"
                    "# written here survives a reboot.\n"
                    "UUID=%-36s /boot   vfat  defaults,noatime     0    2\n", esp_uuid);
                fclose(f);
            }
        }
    } else {
        write_fstab(MNT, root_uuid, esp_uuid, home_uuid, swap_uuid, luks_uuid[0] != 0);
    }
    set_hostname(MNT, hostname);
    if (user) run("chroot %s /usr/bin/useradd -m -s /bin/bash %s", MNT, user);
    step("setting passwords");
    set_passwords(MNT, user);

    step("unmounting");
    run("sync");
    if (ramdisk_root) {
        run("umount %s", MNT);
        run("umount %s", esp_dir);
    } else
    run("umount %s/boot", MNT);
    if (!ramdisk_root) {
        if (home_part[0]) run("umount %s/home", MNT);
        run("umount %s", MNT);
    }
    if (luks_uuid[0]) run("cryptsetup luksClose %s || true", LUKS_NAME);

    if (immutable_root)
        printf("\n\033[1mThis install is immutable:\033[0m the root is mounted read-only with a\n"
               "RAM overlay on top. Anything written is gone at the next reboot.\n");
    if (ramdisk_root)
        printf("\n\033[1mThis install is diskless:\033[0m the whole system is one image on the EFI\n"
               "partition%s, with a RAM overlay on top. There is no root partition,\n"
               "and nothing written survives a reboot.\n",
               toram ? ", copied into RAM at boot" : ", read from the disk as it runs");
    printf("\n\033[1;32mVerkOS is installed on %s.\033[0m\n", disk);
    if (ramdisk_root)
        printf("The system image is %s/rootfs.img on the EFI partition (UUID %s).\n"
               "Remove the install medium and reboot.\n\n", "", esp_uuid);
    else
        printf("Root filesystem UUID %s. Remove the install medium and reboot.\n\n", root_uuid);
    if (user) printf("An account for '%s' is ready to log in.\n\n", user);
    if (lock_root) printf("The root account is locked; use '%s' and su.\n\n", user ? user : "your account");
    return 0;
}
