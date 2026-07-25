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

#define MNT      "/run/vinstall"
#define ESP_MB_DEFAULT 128

static int dry_run, assume_yes, verbose;
/* Where the system being installed is read from. "/" is the running system;
 * an ISO or a mounted image is just as valid a source, and is how this gets
 * tested without a live medium. */
static const char *source = "/";
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

/* ---- confirmation --------------------------------------------------------- */
static void confirm(const char *disk, unsigned esp_mb, const char *hostname) {
    printf("\n\033[1mAbout to install VerkOS onto %s\033[0m\n\n", disk);
    printf("  %-22s %s\n", "target disk", disk);
    printf("  %-22s GPT: %u MB EFI system partition + ext4 root on the rest\n",
           "layout", esp_mb);
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

static void write_fstab(const char *root, const char *root_uuid, const char *esp_uuid) {
    if (dry_run) return;
    char path[512];
    snprintf(path, sizeof path, "%s/etc/fstab", root);
    FILE *f = fopen(path, "w");
    if (!f) die("cannot write %s: %s", path, strerror(errno));
    fprintf(f,
        "# Written by vinstall. urverk mounts these at boot.\n"
        "# device                                  mount   type  options              dump fsck\n"
        "UUID=%-36s /       ext4  defaults,noatime     0    1\n"
        "UUID=%-36s /boot   vfat  defaults,noatime     0    2\n",
        root_uuid, esp_uuid);
    fclose(f);
}

static void install_bootloader(const char *root, const char *arch, const char *root_uuid) {
    char esp[512];
    snprintf(esp, sizeof esp, "%s/boot", root);
    run("mkdir -p %s/EFI/BOOT", esp);

    /* vboot itself, under the name the firmware looks for with no NVRAM entry. */
    const char *efi_name = !strcmp(arch, "aarch64") ? "BOOTAA64.EFI" : "BOOTX64.EFI";
    const char *sroot = strcmp(source, "/") ? source : "";
    char src[768];
    snprintf(src, sizeof src, "%s/boot/EFI/BOOT/%s", sroot, efi_name);
    if (access(src, R_OK) != 0 && !dry_run)
        die("cannot find %s — the EFI partition must be mounted at %s/boot", src, sroot);
    run("cp %s %s/EFI/BOOT/%s", src, esp, efi_name);

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
    fprintf(f,
        "# Written by vinstall.\n"
        "timeout 5\n"
        "default 0\n"
        "\n"
        "entry VerkOS\n"
        "  kernel  \\vmlinuz\n"
        "  initrd  \\initramfs.img\n"
        "  cmdline root=UUID=%s rw console=%s init=/sbin/urverk\n"
        "\n"
        "entry VerkOS (verbose boot)\n"
        "  kernel  \\vmlinuz\n"
        "  initrd  \\initramfs.img\n"
        "  cmdline root=UUID=%s rw console=%s init=/sbin/urverk urverk.debug\n"
        "\n"
        "entry VerkOS (rescue shell)\n"
        "  kernel  \\vmlinuz\n"
        "  initrd  \\initramfs.img\n"
        "  cmdline root=UUID=%s rw console=%s init=/sbin/urverk urverk.rescue\n",
        root_uuid, console, root_uuid, console, root_uuid, console);
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
           "  -n           dry run: print what would happen, change nothing\n"
           "  -v           show every command\n", ESP_MB_DEFAULT);
}

int main(int argc, char **argv) {
    const char *disk = NULL, *hostname = "verkos", *user = NULL;
    unsigned esp_mb = ESP_MB_DEFAULT;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-t") && i + 1 < argc) disk = argv[++i];
        else if (!strcmp(argv[i], "-y")) assume_yes = 1;
        else if (!strcmp(argv[i], "-n")) dry_run = 1;
        else if (!strcmp(argv[i], "-v")) verbose = 1;
        else if (!strcmp(argv[i], "-H") && i + 1 < argc) hostname = argv[++i];
        else if (!strcmp(argv[i], "-u") && i + 1 < argc) user = argv[++i];
        else if (!strcmp(argv[i], "-s") && i + 1 < argc) source = argv[++i];
        else if (!strcmp(argv[i], "--esp") && i + 1 < argc) esp_mb = (unsigned)atoi(argv[++i]);
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

    if (!disk) {
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
    if (stat(disk, &st)) die("%s: %s", disk, strerror(errno));
    if (!S_ISBLK(st.st_mode)) die("%s is not a block device", disk);

    char running[256];
    running_root_disk(running, sizeof running);
    if (running[0] && !strcmp(running, disk))
        die("%s is the disk this system is running from", disk);
    if (device_is_mounted(disk))
        die("%s has mounted partitions — unmount them first", disk);

    if (!dry_run) check_source(arch);
    confirm(disk, esp_mb, hostname);

    char esp_part[256], root_part[256];
    part_name(disk, 1, esp_part, sizeof esp_part);
    part_name(disk, 2, root_part, sizeof root_part);

    step("partitioning %s", disk);
    run("wipefs -a %s >/dev/null 2>&1 || true", disk);
    /* One ESP and one root, both carrying the standard GPT type GUIDs so other
     * systems (and the firmware) recognise what they are looking at. The root
     * partition takes whatever is left. */
    if (!dry_run) {
        FILE *sf = fopen("/run/vinstall.sfdisk", "w");
        if (!sf) die("cannot write the partition script: %s", strerror(errno));
        fprintf(sf,
            "label: gpt\n"
            "start=1MiB, size=%uMiB, type=C12A7328-F81F-11D2-BA4B-00A0C93EC93B, name=\"EFI System\"\n"
            "type=0FC63DAF-8483-4772-8E79-3D69D8477DE4, name=\"VerkOS\"\n", esp_mb);
        fclose(sf);
    }
    run("sfdisk --quiet --wipe always %s < /run/vinstall.sfdisk", disk);
    run("partx -u %s >/dev/null 2>&1 || partprobe %s >/dev/null 2>&1 || true", disk, disk);
    unlink("/run/vinstall.sfdisk");

    step("creating filesystems");
    wait_for_partition(esp_part);
    wait_for_partition(root_part);
    run("%s -n VERKESP %s", mkvfat, esp_part);
    run("mkfs.ext4 -q -F -L VERKOS %s", root_part);

    char root_uuid[128], esp_uuid[128];
    get_uuid(root_part, root_uuid, sizeof root_uuid);
    get_uuid(esp_part, esp_uuid, sizeof esp_uuid);

    step("mounting the new system on %s", MNT);
    run("mkdir -p %s", MNT);
    run("mount %s %s", root_part, MNT);
    run("mkdir -p %s/boot", MNT);
    run("mount %s %s/boot", esp_part, MNT);

    step("copying the system");
    copy_system(MNT);

    step("installing vboot and writing the boot configuration");
    install_bootloader(MNT, arch, root_uuid);

    step("configuring the installed system");
    write_fstab(MNT, root_uuid, esp_uuid);
    set_hostname(MNT, hostname);
    if (user) run("chroot %s /usr/bin/useradd -m -s /bin/bash %s", MNT, user);

    step("unmounting");
    run("sync");
    run("umount %s/boot", MNT);
    run("umount %s", MNT);

    printf("\n\033[1;32mVerkOS is installed on %s.\033[0m\n", disk);
    printf("Root filesystem UUID %s. Remove the install medium and reboot.\n\n", root_uuid);
    if (user) printf("The account '%s' has no password yet — set one with: passwd %s\n\n", user, user);
    return 0;
}
