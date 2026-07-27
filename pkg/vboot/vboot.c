/* vboot — VerkOS' own UEFI bootloader.
 *
 * The firmware loads us from \EFI\BOOT\BOOTAA64.EFI. We read \vboot.conf off
 * the same volume, draw a boot menu, and start the chosen kernel. Linux is
 * built with CONFIG_EFI_STUB, so vmlinuz is itself an EFI application —
 * LoadImage/StartImage is the whole handoff, no boot protocol to reimplement.
 *
 * No gnu-efi, no GRUB: hand-written EFI headers (efi.h) + plain gcc/binutils.
 * See build.sh for the two PE quirks a strict EDK2 loader requires.
 *
 * \vboot.conf (plain ASCII, on the ESP):
 *      timeout 5                 # seconds before the default entry boots
 *      default 0                 # which entry that is
 *      entry VerkOS 0.1          # begins an entry; the rest of the line is its title
 *        kernel  \vmlinuz
 *        cmdline root=/dev/vda rw console=ttyAMA0 init=/usr/lib/systemd/systemd
 */
#include "efi.h"
#include "ed25519.h"
/* Angle brackets, not quotes: a quoted include is resolved from this file's own
 * directory first, which would make the checked-in all-zero key impossible to
 * override. With -I the build can point at a generated header instead, and the
 * repository keeps a key nobody holds the private half of. */
#include <vboot-key.h>

#define CONFIG_PATH   L"\\vboot.conf"
#define MAX_ENTRIES   8
#define TITLE_MAX     64
#define PATH_MAX      128
#define CMDLINE_MAX   512
#define MENU_ROW      8               /* first screen row the entries occupy */

typedef struct {
    CHAR16 title[TITLE_MAX];
    CHAR16 kernel[PATH_MAX];
    CHAR16 initrd[PATH_MAX];
    CHAR16 cmdline[CMDLINE_MAX];
} entry_t;

static EFI_SYSTEM_TABLE  *ST;
static EFI_BOOT_SERVICES *BS;
static EFI_RUNTIME_SERVICES *RT;
static entry_t  entries[MAX_ENTRIES];
static int      n_entries, cfg_default, cfg_timeout = 5;

/* ---- tiny console helpers ------------------------------------------------ */
static void print(CHAR16 *s) { ST->ConOut->OutputString(ST->ConOut, s); }
static void attr(UINTN a)    { ST->ConOut->SetAttribute(ST->ConOut, a); }
static void at(UINTN col, UINTN row) { ST->ConOut->SetCursorPosition(ST->ConOut, col, row); }
static void print_num(UINTN v) {
    CHAR16 b[24]; int i = 23; b[i--] = 0;
    if (!v) b[i--] = L'0';
    while (v && i >= 0) { b[i--] = L'0' + (v % 10); v /= 10; }
    print(&b[i + 1]);
}
static void print_status(EFI_STATUS s) {
    CHAR16 b[20]; int i = 18; b[19] = 0;
    if (!s) b[i--] = L'0';
    while (s && i >= 1) { UINTN d = s & 0xf; b[i--] = d < 10 ? L'0' + d : L'a' + (d - 10); s >>= 4; }
    b[i--] = L'x'; b[i] = L'0';
    print(&b[i]);
}
static void fail(CHAR16 *what, EFI_STATUS st) {
    attr(EFI_WHITE); print(L"  [!!] "); print(what); print(L" failed: ");
    print_status(st); print(L"\r\n"); attr(EFI_LIGHTGRAY);
}
static UINTN slen(CHAR16 *s) { UINTN n = 0; while (s[n]) n++; return n; }
static void scopy(CHAR16 *dst, CHAR16 *src, UINTN cap) {
    UINTN i = 0; for (; src[i] && i < cap - 1; i++) dst[i] = src[i]; dst[i] = 0;
}

/* ---- volume + file helpers ---------------------------------------------- */
static EFI_FILE_PROTOCOL *volume_root(EFI_HANDLE image) {
    EFI_GUID li_guid = EFI_LOADED_IMAGE_PROTOCOL_GUID;
    EFI_GUID fs_guid = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
    EFI_LOADED_IMAGE_PROTOCOL *li = 0;
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs = 0;
    EFI_FILE_PROTOCOL *root = 0;
    if (EFI_ERROR(BS->HandleProtocol(image, &li_guid, (void **)&li))) return 0;
    if (EFI_ERROR(BS->HandleProtocol(li->DeviceHandle, &fs_guid, (void **)&fs))) return 0;
    if (EFI_ERROR(fs->OpenVolume(fs, &root))) return 0;
    return root;
}
#include "kernelname.h"

/* read a whole file into pool memory; returns 0 on failure */
static void *read_file(EFI_FILE_PROTOCOL *root, CHAR16 *path, UINTN *out_size) {
    EFI_FILE_PROTOCOL *f = 0;
    if (EFI_ERROR(root->Open(root, &f, path, EFI_FILE_MODE_READ, 0))) return 0;
    UINT64 size = 0;
    /* seek to EOF to size the file — avoids needing the EFI_FILE_INFO struct */
    if (EFI_ERROR(f->SetPosition(f, 0xFFFFFFFFFFFFFFFFULL)) ||
        EFI_ERROR(f->GetPosition(f, &size)) ||
        EFI_ERROR(f->SetPosition(f, 0))) { f->Close(f); return 0; }
    void *buf = 0;
    if (EFI_ERROR(BS->AllocatePool(EfiLoaderData, (UINTN)size + 2, &buf))) { f->Close(f); return 0; }
    /* Read in a loop. EFI lets a filesystem driver return fewer bytes than
     * asked for — it sets the count to what it actually gave — and a single
     * Read that came up short would silently produce a truncated kernel or a
     * half-read config with no error anywhere. FAT happens to satisfy the
     * whole request; nothing guarantees another driver will. */
    UINTN want = 0;
    while (want < (UINTN)size) {
        UINTN chunk = (UINTN)size - want;
        if (EFI_ERROR(f->Read(f, &chunk, (unsigned char *)buf + want))) {
            BS->FreePool(buf); f->Close(f); return 0;
        }
        if (chunk == 0) break;                        /* end of file, earlier than expected */
        want += chunk;
    }
    if (want < (UINTN)size) {
        print(L"  [!!] short read: ");
        print_num(want); print(L" of "); print_num((UINTN)size); print(L" bytes\r\n");
    }
    ((unsigned char *)buf)[want] = 0;                 /* NUL-terminate for the parser */
    f->Close(f);
    if (out_size) *out_size = want;
    return buf;
}

/* ---- initrd handoff ------------------------------------------------------
 * Linux >= 5.8 does not read initrd= from the command line in its EFI stub. It
 * looks up a handle carrying the Linux initrd media device path and calls
 * LoadFile2 on it. So we read the initrd into memory, then publish it through
 * exactly that protocol and let the stub pull it across. */
static void  *initrd_buf;
static UINTN  initrd_size;

static VENDOR_DEVICE_PATH initrd_dp_node = {
    { MEDIA_DEVICE_PATH, MEDIA_VENDOR_DP, { sizeof(VENDOR_DEVICE_PATH), 0 } },
    LINUX_EFI_INITRD_MEDIA_GUID
};
static EFI_DEVICE_PATH_PROTOCOL initrd_dp_end = {
    END_DEVICE_PATH_TYPE, END_ENTIRE_DEVICE_PATH, { sizeof(EFI_DEVICE_PATH_PROTOCOL), 0 }
};
/* the two nodes must be adjacent in memory: node then end-of-path */
static struct { VENDOR_DEVICE_PATH v; EFI_DEVICE_PATH_PROTOCOL e; } __attribute__((packed))
initrd_path;

static EFI_STATUS EFIAPI initrd_load_file(EFI_LOAD_FILE2_PROTOCOL *self,
                                          EFI_DEVICE_PATH_PROTOCOL *path,
                                          BOOLEAN boot_policy,
                                          UINTN *bufsize, void *buf) {
    (void)self; (void)path;
    if (boot_policy) return EFI_UNSUPPORTED;            /* LoadFile2 semantics */
    if (!bufsize) return EFI_INVALID_PARAMETER;
    if (!initrd_buf || !initrd_size) return EFI_NOT_FOUND;
    if (!buf || *bufsize < initrd_size) { *bufsize = initrd_size; return EFI_BUFFER_TOO_SMALL; }
    unsigned char *d = buf, *s = initrd_buf;
    for (UINTN i = 0; i < initrd_size; i++) d[i] = s[i];
    *bufsize = initrd_size;
    return EFI_SUCCESS;
}
/* NOTE: deliberately left uninitialised. A static initialiser holding a function
 * pointer would be stored as its link-time address and need a load-time
 * relocation, and vboot is built with none (see build.sh) — the firmware would
 * hand the kernel a bogus callback. Filled in at runtime below, where the
 * compiler computes the address PC-relative. */
static EFI_LOAD_FILE2_PROTOCOL initrd_lf2;

/* publish initrd_buf to the kernel; returns the handle we created (0 on error) */
static EFI_HANDLE publish_initrd(void) {
    EFI_GUID dp_guid  = EFI_DEVICE_PATH_PROTOCOL_GUID;
    EFI_GUID lf2_guid = EFI_LOAD_FILE2_PROTOCOL_GUID;
    EFI_HANDLE h = 0;
    EFI_STATUS st;
    initrd_lf2.LoadFile = initrd_load_file;      /* PC-relative, no relocation */
    initrd_path.v = initrd_dp_node;
    initrd_path.e = initrd_dp_end;
    st = BS->InstallProtocolInterface(&h, &dp_guid, 0, &initrd_path);
    if (EFI_ERROR(st)) { fail(L"InstallProtocolInterface(DevicePath)", st); return 0; }
    st = BS->InstallProtocolInterface(&h, &lf2_guid, 0, &initrd_lf2);
    if (EFI_ERROR(st)) { fail(L"InstallProtocolInterface(LoadFile2)", st); return 0; }

    return h;
}

/* ---- config parsing (plain ASCII, no libc) ------------------------------- */
static int is_space(unsigned char c) { return c == ' ' || c == '\t'; }
/* does line start with word `w`? if so return the offset just past it */
static int keyword(unsigned char *p, const char *w) {
    int i = 0;
    while (w[i]) { if (p[i] != (unsigned char)w[i]) return 0; i++; }
    if (!is_space(p[i])) return 0;
    while (is_space(p[i])) i++;
    return i;
}
static UINTN parse_num(unsigned char *p) {
    UINTN v = 0; while (*p >= '0' && *p <= '9') { v = v * 10 + (*p - '0'); p++; }
    return v;
}
/* copy the rest of an ASCII line into a CHAR16 buffer, trimming trailing space */
static void copy_line(unsigned char *p, CHAR16 *dst, UINTN cap) {
    UINTN i = 0;
    while (p[i] && p[i] != '\n' && p[i] != '\r' && i < cap - 1) { dst[i] = (CHAR16)p[i]; i++; }
    while (i && (dst[i-1] == L' ' || dst[i-1] == L'\t')) i--;
    dst[i] = 0;
}
static void parse_config(unsigned char *buf) {
    unsigned char *p = buf;
    entry_t *cur = 0;
    while (*p) {
        while (is_space(*p)) p++;
        if (*p == '#' || *p == '\n' || *p == '\r') {          /* comment / blank */
            while (*p && *p != '\n') p++;
            if (*p) p++;
            continue;
        }
        int off;
        if ((off = keyword(p, "entry"))) {
            if (n_entries < MAX_ENTRIES) {
                cur = &entries[n_entries++];
                copy_line(p + off, cur->title, TITLE_MAX);
                cur->kernel[0] = 0; cur->initrd[0] = 0; cur->cmdline[0] = 0;
            } else cur = 0;
        } else if ((off = keyword(p, "kernel"))  && cur) {
            copy_line(p + off, cur->kernel, PATH_MAX);
        } else if ((off = keyword(p, "initrd"))  && cur) {
            copy_line(p + off, cur->initrd, PATH_MAX);
        } else if ((off = keyword(p, "cmdline")) && cur) {
            copy_line(p + off, cur->cmdline, CMDLINE_MAX);
        } else if ((off = keyword(p, "timeout"))) {
            cfg_timeout = (int)parse_num(p + off);
        } else if ((off = keyword(p, "default"))) {
            cfg_default = (int)parse_num(p + off);
        }
        while (*p && *p != '\n') p++;                          /* next line */
        if (*p) p++;
    }
}
/* what we boot when there is no (usable) vboot.conf */
static void builtin_defaults(void) {
    n_entries = 1; cfg_default = 0; cfg_timeout = 3;
    scopy(entries[0].title,   L"VerkOS (built-in default)", TITLE_MAX);
    scopy(entries[0].kernel,  L"\\vmlinuz", PATH_MAX);
    scopy(entries[0].cmdline,
          L"root=/dev/vda rw console=ttyAMA0 init=/usr/lib/systemd/systemd", CMDLINE_MAX);
}

/* Scan the root of the ESP for kernels and add an entry for any that the config
 * did not already mention, so dropping in a new vmlinuz is enough to boot it. */
static int have_kernel(CHAR16 *name) {
    for (int i = 0; i < n_entries; i++) {
        CHAR16 *k = entries[i].kernel;
        if (k[0] == L'\\') k++;                       /* compare without the leading \ */
        UINTN a = 0;
        while (k[a] && name[a] && k[a] == name[a]) a++;
        if (!k[a] && !name[a]) return 1;
    }
    return 0;
}
static void scan_kernels(EFI_FILE_PROTOCOL *root) {
    if (!root) return;
    EFI_FILE_PROTOCOL *dir = 0;
    if (EFI_ERROR(root->Open(root, &dir, L"\\", EFI_FILE_MODE_READ, 0))) return;
    unsigned char buf[512];
    for (;;) {
        UINTN sz = sizeof buf;
        if (EFI_ERROR(dir->Read(dir, &sz, buf)) || sz == 0) break;   /* 0 = end of dir */
        EFI_FILE_INFO *fi = (EFI_FILE_INFO *)buf;
        if (fi->Attribute & EFI_FILE_DIRECTORY) continue;
        CHAR16 *nm = fi->FileName;
        /* anything called vmlinuz... is a kernel. Compared without case:
         * an ISO 9660 volume with no Rock Ridge presents VMLINUZ.;1, and a
         * byte-for-byte lowercase match would find no kernel at all there. */
        if (!name_is_kernel(nm)) continue;
        if (have_kernel(nm)) continue;
        if (n_entries >= MAX_ENTRIES) break;
        entry_t *e = &entries[n_entries++];
        scopy(e->title, L"VerkOS (found: ", TITLE_MAX);
        UINTN t = slen(e->title);
        for (UINTN i = 0; nm[i] && t < TITLE_MAX - 2; i++) e->title[t++] = nm[i];
        e->title[t++] = L')'; e->title[t] = 0;
        e->kernel[0] = L'\\';
        scopy(e->kernel + 1, nm, PATH_MAX - 1);
        e->initrd[0] = 0;
        scopy(e->cmdline, L"root=/dev/vda rw console=ttyAMA0 init=/sbin/urverk", CMDLINE_MAX);
    }
    dir->Close(dir);
}

/* ---- graphical logo (optional; text still drives the menu) --------------- */
static EFI_GRAPHICS_OUTPUT_PROTOCOL *gop;
static void gop_fill(UINTN x, UINTN y, UINTN w, UINTN h, UINT8 r, UINT8 g, UINT8 b) {
    EFI_GRAPHICS_OUTPUT_BLT_PIXEL px;
    px.Red = r; px.Green = g; px.Blue = b; px.Reserved = 0;
    gop->Blt(gop, &px, EfiBltVideoFill, 0, 0, x, y, w, h, 0);
}
/* The VerkOS V, drawn as two converging arms in the cyan->purple gradient. */
static void draw_logo(void) {
    EFI_GUID gop_guid = EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID;
    if (!gop && EFI_ERROR(BS->LocateProtocol(&gop_guid, 0, (void **)&gop))) { gop = 0; return; }
    if (!gop || !gop->Mode || !gop->Mode->Info) return;
    UINTN W = gop->Mode->Info->HorizontalResolution;
    UINTN H = gop->Mode->Info->VerticalResolution;
    if (!W || !H) return;

    gop_fill(0, 0, W, H, 0x0a, 0x0e, 0x16);            /* deep background */
    UINTN top = H / 12, height = H / 4, half = height * 3 / 5, thick = height / 7;
    for (UINTN i = 0; i < height; i++) {
        /* cyan (0x5ef1ff) -> purple (0xb026d3) down the stroke */
        UINT8 r = (UINT8)(0x5e + (0xb0 - 0x5e) * i / height);
        UINT8 g = (UINT8)(0xf1 - (0xf1 - 0x26) * i / height);
        UINT8 b = (UINT8)(0xff - (0xff - 0xd3) * i / height);
        UINTN off = half * i / height;
        gop_fill(W / 2 - half + off,         top + i, thick, 1, r, g, b);
        gop_fill(W / 2 + half - off - thick, top + i, thick, 1, r, g, b);
    }
}

/* ---- menu ---------------------------------------------------------------- */
static void draw_banner(void) {
    ST->ConOut->ClearScreen(ST->ConOut);
    draw_logo();          /* no-op when the firmware has no GOP */
    attr(EFI_LIGHTCYAN);
    at(2, 1); print(L"##   ##   VerkOS");
    attr(EFI_CYAN);
    at(2, 2); print(L" ## ##    vboot 0.1");
    at(2, 3); print(L"  ###     our own UEFI bootloader");
    at(2, 4); print(L"   #");
    attr(EFI_DARKGRAY);
    at(2, 6); print(L"arrows/number select  Enter boot  e edit cmdline  r reboot  s shutdown  f firmware");
    attr(EFI_LIGHTGRAY);
}
static void draw_entries(int sel) {
    for (int i = 0; i < n_entries; i++) {
        at(4, MENU_ROW + i);
        if (i == sel) { attr(EFI_BLACK | EFI_BG(EFI_LIGHTCYAN)); print(L" > "); }
        else          { attr(EFI_LIGHTGRAY);                     print(L"   "); }
        print(entries[i].title);
        print(L"                                        ");   /* pad the highlight */
        attr(EFI_LIGHTGRAY);
    }
}
static void draw_timer(int secs, int cancelled) {
    at(4, MENU_ROW + n_entries + 1);
    attr(EFI_DARKGRAY);
    if (cancelled) print(L"timer cancelled — press Enter to boot          ");
    else { print(L"booting in "); print_num(secs); print(L"s...            "); }
    attr(EFI_LIGHTGRAY);
}

/* Edit a CHAR16 string in place on the bottom line: printable keys insert,
 * backspace deletes, Enter accepts, Esc cancels. Enough to fix a boot without
 * mounting the ESP from another machine. */
static int edit_line(CHAR16 *buf, UINTN cap, CHAR16 *label) {
    UINTN len = slen(buf);
    for (;;) {
        at(0, MENU_ROW + n_entries + 3);
        attr(EFI_LIGHTGRAY);
        print(L"\r"); print(label); print(buf);
        print(L"                                                            ");
        at(slen(label) + len, MENU_ROW + n_entries + 3);
        EFI_INPUT_KEY k;
        if (!ST->ConIn || EFI_ERROR(ST->ConIn->ReadKeyStroke(ST->ConIn, &k))) { BS->Stall(20000); continue; }
        if (k.UnicodeChar == L'\r') return 1;
        if (k.ScanCode == SCAN_ESC)  return 0;
        if (k.UnicodeChar == 8 || k.UnicodeChar == 127) { if (len) buf[--len] = 0; continue; }
        if (k.UnicodeChar >= 32 && len + 1 < cap) { buf[len++] = k.UnicodeChar; buf[len] = 0; }
    }
}

/* ---- firmware actions ---------------------------------------------------- */
static void do_reset(EFI_RESET_TYPE type) {
    if (RT && RT->ResetSystem) RT->ResetSystem(type, EFI_SUCCESS, 0, 0);
    for (;;) BS->Stall(1000000);
}
/* Ask the firmware to come up in its setup UI on the next boot, then reboot. */
static void boot_to_firmware(void) {
    EFI_GUID gv = EFI_GLOBAL_VARIABLE_GUID;
    UINT64 ind = EFI_OS_INDICATIONS_BOOT_TO_FW_UI;
    if (RT && RT->SetVariable)
        RT->SetVariable(L"OsIndications", &gv,
                        EFI_VARIABLE_NON_VOLATILE | EFI_VARIABLE_BOOTSERVICE_ACCESS |
                        EFI_VARIABLE_RUNTIME_ACCESS, sizeof ind, &ind);
    do_reset(EfiResetCold);
}

/* ---- remembering the last choice ------------------------------------------
 * Written to an EFI variable rather than the ESP so it also works when the
 * volume is read-only. The same variable doubles as the failed-boot marker:
 * it is set to "trying N" before starting an entry and cleared by the running
 * system (urverk), so an entry that never reaches userspace is not retried. */
static void remember_choice(int idx, int trying) {
    EFI_GUID gv = VERKOS_VBOOT_GUID;
    UINT32 v = ((UINT32)idx & 0xffff) | (trying ? 0x10000u : 0u);
    if (!RT || !RT->SetVariable) { print(L"  [!!] no runtime services for VbootLast\r\n"); return; }
    EFI_STATUS st = RT->SetVariable(L"VbootLast", &gv,
                        EFI_VARIABLE_NON_VOLATILE | EFI_VARIABLE_BOOTSERVICE_ACCESS |
                        EFI_VARIABLE_RUNTIME_ACCESS, sizeof v, &v);
    if (EFI_ERROR(st)) fail(L"SetVariable(VbootLast)", st);
}
/* Returns the remembered entry, or -1. *failed is set when the last boot was
 * still marked as in progress, i.e. it never came up. */
static int recall_choice(int *failed) {
    EFI_GUID gv = VERKOS_VBOOT_GUID;
    UINT32 v = 0; UINTN sz = sizeof v;
    *failed = 0;
    if (!RT || !RT->GetVariable) return -1;
    if (EFI_ERROR(RT->GetVariable(L"VbootLast", &gv, 0, &sz, &v))) return -1;
    *failed = (v & 0x10000u) ? 1 : 0;
    return (int)(v & 0xffff);
}

/* ---- signature verification ---------------------------------------------
 * Secure boot, VerkOS style: the public key is compiled into vboot, and a
 * payload is only started if <name>.sig verifies against it. The trust root is
 * therefore vboot itself, which the firmware loads.
 *
 * Enforcement is deliberately conditional on a key being configured: an image
 * built without one boots exactly as before, so signing can be adopted without
 * every existing install refusing to start. Once a key IS embedded, a missing
 * or bad signature stops the boot. */
static int key_configured(void) {
    for (int i = 0; i < 32; i++) if (VBOOT_PUBKEY[i]) return 1;
    return 0;
}
/* Verify buf against PATH.sig from the same volume. 0 = trusted. */
static int verify_payload(EFI_FILE_PROTOCOL *root, CHAR16 *path,
                          unsigned char *buf, UINTN len) {
    if (!key_configured()) return 0;                  /* unsigned build: allow */

    CHAR16 sigpath[PATH_MAX + 8];
    UINTN i = 0;
    for (; path[i] && i < PATH_MAX; i++) sigpath[i] = path[i];
    sigpath[i++] = L'.'; sigpath[i++] = L's'; sigpath[i++] = L'i'; sigpath[i++] = L'g';
    sigpath[i] = 0;

    UINTN slen_ = 0;
    unsigned char *sig = read_file(root, sigpath, &slen_);
    if (!sig) {
        attr(EFI_WHITE);
        print(L"  [!!] no signature for "); print(path); print(L"\r\n");
        attr(EFI_LIGHTGRAY);
        return -1;
    }
    if (slen_ < 64) { BS->FreePool(sig); return -1; }
    int rc = ed25519_verify(sig, buf, len, VBOOT_PUBKEY);
    BS->FreePool(sig);
    if (rc) {
        attr(EFI_WHITE);
        print(L"  [!!] SIGNATURE INVALID for "); print(path); print(L"\r\n");
        attr(EFI_LIGHTGRAY);
        return -1;
    }
    print(L"  [ok] signature verified\r\n");
    return 0;
}

/* ---- boot ---------------------------------------------------------------- */

/* ---- building a device tree when the firmware has none -------------------
 *
 * The asymmetry this solves: aarch64 firmware normally publishes a device tree
 * in the configuration table, and x86_64 firmware never does — it describes the
 * machine with ACPI. Without this, the kernel would need two ways to learn
 * about hardware and every driver would be written twice.
 *
 * So vboot builds a tree and installs it under the GUID the firmware would
 * have used. The kernel looks in exactly one place, on every machine.
 *
 * The tree is deliberately thin. It carries what cannot be discovered without
 * firmware help — where memory is, where the console is, where ACPI's root
 * pointer is — and nothing else. Everything a driver can probe for itself is
 * the driver's business.
 */
#include "fdtgen.h"

static EFI_GUID acpi20_guid =
    { 0x8868e871, 0xe4f1, 0x11d3, { 0xbc,0x22,0x00,0x80,0xc7,0x3c,0x88,0x81 } };
static EFI_GUID dtb_guid =
    { 0xb1b621d5, 0xf19c, 0x41a5, { 0x83,0x0b,0xd9,0x15,0x2c,0x69,0xaa,0xe0 } };

typedef struct { EFI_GUID VendorGuid; void *VendorTable; } vb_cfg_t;
typedef EFI_STATUS (EFIAPI *vb_getmmap_t)(UINTN *, void *, UINTN *, UINTN *, UINT32 *);
typedef EFI_STATUS (EFIAPI *vb_instcfg_t)(EFI_GUID *, void *);

static unsigned char dtb_area[16 * 1024];
static unsigned char mmap_area[32 * 1024];

static int vb_guid_eq(const EFI_GUID *a, const EFI_GUID *b) {
    if (a->Data1 != b->Data1 || a->Data2 != b->Data2 || a->Data3 != b->Data3) return 0;
    for (int i = 0; i < 8; i++) if (a->Data4[i] != b->Data4[i]) return 0;
    return 1;
}

static void *vb_find_cfg(EFI_GUID *want) {
    vb_cfg_t *ct = (vb_cfg_t *)ST->ConfigurationTable;
    for (UINTN i = 0; i < ST->NumberOfTableEntries; i++)
        if (vb_guid_eq(&ct[i].VendorGuid, want)) return ct[i].VendorTable;
    return 0;
}

/* Walk the XSDT for a table by signature. Every ACPI reader below needs this,
 * and doing it once means one place to be wrong about pointer widths. */
static unsigned char *vb_acpi_table(const char *sig) {
    unsigned char *rsdp = vb_find_cfg(&acpi20_guid);
    if (!rsdp) return 0;
    UINT64 xsdt = 0;
    for (int i = 0; i < 8; i++) xsdt |= (UINT64)rsdp[24 + i] << (8 * i);
    if (!xsdt) return 0;
    unsigned char *x = (unsigned char *)(UINTN)xsdt;
    UINT32 len = (UINT32)x[4] | ((UINT32)x[5] << 8) | ((UINT32)x[6] << 16) | ((UINT32)x[7] << 24);
    if (len < 36) return 0;
    for (UINT32 o = 36; o + 8 <= len; o += 8) {
        UINT64 t = 0;
        for (int i = 0; i < 8; i++) t |= (UINT64)x[o + i] << (8 * i);
        unsigned char *tab = (unsigned char *)(UINTN)t;
        if (!tab) continue;
        if (tab[0] == sig[0] && tab[1] == sig[1] && tab[2] == sig[2] && tab[3] == sig[3])
            return tab;
    }
    return 0;
}

static UINT32 vb_le32(const unsigned char *p) {
    return (UINT32)p[0] | ((UINT32)p[1] << 8) | ((UINT32)p[2] << 16) | ((UINT32)p[3] << 24);
}
static UINT64 vb_le64(const unsigned char *p) {
    UINT64 v = 0; for (int i = 0; i < 8; i++) v |= (UINT64)p[i] << (8 * i); return v;
}

/* MADT ("APIC") lists the interrupt controllers and the processors. On ARM the
 * entries we want are the GIC distributor (type 0x0C) and one GIC CPU
 * interface (type 0x0B) per processor — which is also how the CPUs are
 * counted, since ACPI has no separate list of them. */
static int vb_madt(UINT64 *dist, UINT64 *cpuif, unsigned *ncpu, unsigned *gicver,
                   UINT64 *redist) {
    unsigned char *t = vb_acpi_table("APIC");
    if (!t) return 0;
    UINT32 len = vb_le32(t + 4);
    *dist = 0; *cpuif = 0; *ncpu = 0; *gicver = 0; *redist = 0;
    for (UINT32 o = 44; o + 2 <= len; ) {
        unsigned type = t[o], elen = t[o + 1];
        if (elen < 2 || o + elen > len) break;
        if (type == 0x0b && elen >= 40) {            /* GIC CPU interface */
            if (vb_le32(t + o + 12) & 1) {           /* enabled           */
                (*ncpu)++;
                if (!*cpuif) *cpuif = vb_le64(t + o + 32);
            }
        } else if (type == 0x0c && elen >= 24) {     /* GIC distributor   */
            *dist = vb_le64(t + o + 8);
            *gicver = t[o + 20];
        } else if (type == 0x0e && elen >= 16) {     /* GIC redistributor */
            if (!*redist) *redist = vb_le64(t + o + 4);
        } else if (type == 0x00 && elen >= 8) {      /* x86 local APIC    */
            if (vb_le32(t + o + 4) & 1) (*ncpu)++;
        }
        o += elen;
    }
    return (*ncpu > 0);
}

/* GTDT says which interrupt the generic timer uses. The number it gives is a
 * global interrupt id; a device tree wants it relative to the start of the
 * per-processor range, which is 16. Getting that subtraction wrong produces a
 * kernel that boots and never ticks. */
static int vb_gtdt_timer_ppi(unsigned *ppi) {
    unsigned char *t = vb_acpi_table("GTDT");
    if (!t || vb_le32(t + 4) < 64) return 0;
    UINT32 gsiv = vb_le32(t + 56);               /* non-secure EL1 timer */
    if (gsiv < 16 || gsiv > 31) return 0;
    *ppi = gsiv - 16;
    return 1;
}

/* ACPI's SPCR table says where the console serial port is and what kind it is.
 * That is the only reason we look at ACPI at all here: on a machine described
 * purely by ACPI there is no other way to find the console, and a kernel with
 * no console is a kernel that cannot tell you why it stopped. */
static int vb_spcr(UINT64 *base, const char **compat) {
    unsigned char *tab = vb_acpi_table("SPCR");
    if (!tab) return 0;
    unsigned iface = tab[36];
    unsigned space = tab[40];              /* 0 = memory-mapped, 1 = port I/O */
    UINT64 addr = vb_le64(tab + 44);
    /* Port I/O cannot be expressed as a device-tree "reg", and pretending
     * otherwise would give the kernel an address it would happily write to.
     * x86 will need its console described another way. */
    if (space != 0 || !addr) return 0;
    *base = addr;
    *compat = (iface == 3) ? "arm,pl011" : "ns16550a";
    return 1;
}

/* The extent of usable RAM, as one range. Deliberately coarse: the UEFI map is
 * only valid until ExitBootServices, which the kernel calls after we are gone,
 * so a detailed map baked in here would be describing a machine that no longer
 * exists. The kernel refines this from the map it takes itself. */
static int vb_ram_extent(UINT64 *lo, UINT64 *hi) {
    vb_getmmap_t get = (vb_getmmap_t)BS->GetMemoryMap;
    UINTN size = sizeof mmap_area, key = 0, dsize = 0; UINT32 dver = 0;
    if (EFI_ERROR(get(&size, mmap_area, &key, &dsize, &dver)) || !dsize) return 0;
    UINT64 l = ~0ull, h = 0;
    for (UINTN o = 0; o + dsize <= size; o += dsize) {
        unsigned char *d = mmap_area + o;
        UINT32 type = (UINT32)d[0] | ((UINT32)d[1] << 8) | ((UINT32)d[2] << 16) | ((UINT32)d[3] << 24);
        UINT64 start = 0, pages = 0;
        for (int i = 0; i < 8; i++) start |= (UINT64)d[8 + i] << (8 * i);
        for (int i = 0; i < 8; i++) pages |= (UINT64)d[24 + i] << (8 * i);
        /* conventional, loader and boot-services memory all become ours */
        if (type != 1 && type != 2 && type != 3 && type != 4 && type != 7) continue;
        if (start < l) l = start;
        if (start + pages * 4096 > h) h = start + pages * 4096;
    }
    if (l == ~0ull || h <= l) return 0;
    *lo = l; *hi = h;
    return 1;
}

/* Returns 1 if it installed a tree, 0 if the firmware already had one, -1 on
 * failure. */
static int vb_install_dtb(void) {
    if (vb_find_cfg(&dtb_guid)) return 0;

    UINT64 lo = 0, hi = 0;
    if (!vb_ram_extent(&lo, &hi)) return -1;

    UINT64 uart = 0; const char *uart_compat = 0;
    int have_uart = vb_spcr(&uart, &uart_compat);

    fdtgen_t g;
    fg_init(&g, dtb_area, sizeof dtb_area);
    fg_node(&g, "");
        fg_prop_u32(&g, "#address-cells", 2);
        fg_prop_u32(&g, "#size-cells", 2);
        fg_prop_str(&g, "model", "VerkOS machine described by vboot");
        fg_prop_str(&g, "compatible", "verkos,synthetic");

        fg_node(&g, "chosen");
            void *rsdp = vb_find_cfg(&acpi20_guid);
            if (rsdp) fg_prop_u64(&g, "verkos,acpi-rsdp", (unsigned long long)(UINTN)rsdp);
            if (have_uart) {
                CHAR16 wpath[64]; char path[64]; int n = 0;
                path[n++] = '/'; 
                for (const char *q = "serial@"; *q; q++) path[n++] = *q;
                UINT64 v = uart; char hex[17]; int hn = 0;
                if (!v) hex[hn++] = '0';
                while (v) { unsigned d = (unsigned)(v & 15); hex[hn++] = (char)(d < 10 ? '0'+d : 'a'+d-10); v >>= 4; }
                while (hn--) path[n++] = hex[hn];
                path[n] = 0; (void)wpath;
                fg_prop_str(&g, "stdout-path", path);
            }
        fg_end_node(&g);

        fg_node(&g, "memory@0");
            fg_prop_str(&g, "device_type", "memory");
            fg_prop_reg(&g, (unsigned long long)lo, (unsigned long long)(hi - lo));
        fg_end_node(&g);

        /* Processors and the interrupt controller, out of ACPI's MADT. */
        UINT64 gicd = 0, gicc = 0, gicr = 0; unsigned ncpu = 0, gicver = 0;
        int have_madt = vb_madt(&gicd, &gicc, &ncpu, &gicver, &gicr);
        /* A GICv3 redistributor may be described by its own MADT entry or,
         * on some firmware, only by the per-processor GICR field. Neither is
         * guaranteed, so a v3 without one is reported rather than guessed at. */
        if (have_madt && ncpu) {
            fg_node(&g, "cpus");
                fg_prop_u32(&g, "#address-cells", 1);
                fg_prop_u32(&g, "#size-cells", 0);
                for (unsigned c = 0; c < ncpu && c < 256; c++) {
                    char nm[16]; int n = 0;
                    nm[n++] = 'c'; nm[n++] = 'p'; nm[n++] = 'u'; nm[n++] = '@';
                    if (c >= 16) nm[n++] = (char)("0123456789abcdef"[c >> 4]);
                    nm[n++] = (char)("0123456789abcdef"[c & 15]);
                    nm[n] = 0;
                    fg_node(&g, nm);
                        fg_prop_str(&g, "device_type", "cpu");
                        fg_prop_str(&g, "compatible", "arm,armv8");
                        fg_prop_u32(&g, "reg", c);
                    fg_end_node(&g);
                }
            fg_end_node(&g);
        }
        /* Only a v2 controller is described, because only a v2 controller is
         * supported. Emitting a v3 node the kernel would then misconfigure is
         * worse than emitting none: it would look like a working machine that
         * silently takes no interrupts. */
        int v2_ok = have_madt && gicd && gicc && gicver <= 2;
        int v3_ok = have_madt && gicd && gicr && gicver >= 3;
        if (v2_ok || v3_ok) {
            char nm[32]; int n = 0;
            for (const char *q = "intc@"; *q; q++) nm[n++] = *q;
            UINT64 v = gicd; char hex[17]; int hn = 0;
            if (!v) hex[hn++] = '0';
            while (v) { unsigned d = (unsigned)(v & 15); hex[hn++] = (char)(d < 10 ? '0'+d : 'a'+d-10); v >>= 4; }
            while (hn--) nm[n++] = hex[hn];
            nm[n] = 0;
            UINT64 second     = v3_ok ? gicr   : gicc;
            UINT64 first_size = v3_ok ? 0x10000 : 0x1000;
            UINT64 second_size= v3_ok ? 0x20000 : 0x2000;
            fg_node(&g, nm);
                fg_prop_str(&g, "compatible", v3_ok ? "arm,gic-v3" : "arm,cortex-a15-gic");
                fg_prop_u32(&g, "#interrupt-cells", 3);
                fg_prop(&g, "interrupt-controller", 0, 0);
                /* v2: distributor then CPU interface. v3: distributor then
                 * redistributor — the CPU interface is not memory-mapped. */
                { unsigned char b[32];
                  fg_be32(b,      (unsigned)(gicd >> 32));   fg_be32(b + 4,  (unsigned)gicd);
                  fg_be32(b + 8,  0);                        fg_be32(b + 12, (unsigned)first_size);
                  fg_be32(b + 16, (unsigned)(second >> 32)); fg_be32(b + 20, (unsigned)second);
                  fg_be32(b + 24, 0);                        fg_be32(b + 28, (unsigned)second_size);
                  fg_prop(&g, "reg", b, 32); }
            fg_end_node(&g);

            unsigned ppi = 0;
            if (vb_gtdt_timer_ppi(&ppi)) {
                fg_node(&g, "timer");
                    fg_prop_str(&g, "compatible", "arm,armv8-timer");
                    /* four triples, as the binding specifies: secure,
                     * non-secure, virtual, hypervisor. Only the second is
                     * read, but a short property is a malformed one. */
                    { unsigned char b[48];
                      for (int t = 0; t < 4; t++) {
                          fg_be32(b + t*12,     1);          /* PPI          */
                          fg_be32(b + t*12 + 4, ppi);
                          fg_be32(b + t*12 + 8, 0x08);       /* level, low   */
                      }
                      fg_prop(&g, "interrupts", b, 48); }
                fg_end_node(&g);
            }
        }

        if (have_uart) {
            char nodename[64]; int n = 0;
            for (const char *q = "serial@"; *q; q++) nodename[n++] = *q;
            UINT64 v = uart; char hex[17]; int hn = 0;
            if (!v) hex[hn++] = '0';
            while (v) { unsigned d = (unsigned)(v & 15); hex[hn++] = (char)(d < 10 ? '0'+d : 'a'+d-10); v >>= 4; }
            while (hn--) nodename[n++] = hex[hn];
            nodename[n] = 0;
            fg_node(&g, nodename);
                fg_prop_str(&g, "compatible", uart_compat);
                fg_prop_reg(&g, (unsigned long long)uart, 0x1000ull);
            fg_end_node(&g);
        }
    fg_end_node(&g);

    if (!fg_finish(&g)) return -1;
    vb_instcfg_t inst = (vb_instcfg_t)BS->InstallConfigurationTable;
    if (EFI_ERROR(inst(&dtb_guid, dtb_area))) return -1;
    return 1;
}

static EFI_STATUS boot_entry(EFI_HANDLE image, EFI_FILE_PROTOCOL *root, entry_t *e) {
    EFI_GUID li_guid = EFI_LOADED_IMAGE_PROTOCOL_GUID;
    EFI_STATUS st;

    ST->ConOut->ClearScreen(ST->ConOut);
    print(L"\r\n  vboot: "); print(e->title); print(L"\r\n");
    print(L"  [..] loading "); print(e->kernel); print(L"\r\n");

    /* A kernel started here may be ours, which expects a device tree. If the
     * firmware did not publish one, build it — this is the whole reason vboot
     * exists on a machine whose firmware could have loaded the kernel itself. */
    switch (vb_install_dtb()) {
    case 1:  print(L"  [ok] device tree built and installed (firmware had none)\r\n"); break;
    case 0:  print(L"  [ok] firmware provides a device tree\r\n"); break;
    default: print(L"  [!!] could not build a device tree\r\n"); break;
    }

    UINTN ksize = 0;
    void *kbuf = read_file(root, e->kernel, &ksize);
    if (!kbuf) { fail(L"reading the kernel", EFI_NOT_FOUND); return EFI_NOT_FOUND; }
    print(L"  [ok] kernel loaded, "); print_num(ksize); print(L" bytes\r\n");
    if (verify_payload(root, e->kernel, kbuf, ksize)) {
        print(L"\r\n  refusing to boot an unverified kernel.\r\n");
        return EFI_LOAD_ERROR;
    }

    /* optional initrd — published via LoadFile2 for the EFI stub to collect */
    EFI_HANDLE ih = 0;
    if (e->initrd[0]) {
        print(L"  [..] loading "); print(e->initrd); print(L"\r\n");
        initrd_buf = read_file(root, e->initrd, &initrd_size);
        if (!initrd_buf) {
            fail(L"reading the initrd", EFI_NOT_FOUND);
        } else if (!(ih = publish_initrd())) {
            fail(L"publishing the initrd", EFI_LOAD_ERROR);
            initrd_buf = 0; initrd_size = 0;
        } else if (verify_payload(root, e->initrd, initrd_buf, initrd_size)) {
            print(L"\r\n  refusing to boot an unverified initramfs.\r\n");
            return EFI_LOAD_ERROR;
        } else {
            print(L"  [ok] initrd loaded, "); print_num(initrd_size); print(L" bytes\r\n");
        }
    }

    EFI_HANDLE kimg = 0;
    st = BS->LoadImage(0, image, 0, kbuf, ksize, &kimg);
    if (EFI_ERROR(st)) { fail(L"LoadImage(kernel)", st); return st; }

    EFI_LOADED_IMAGE_PROTOCOL *kli = 0;
    st = BS->HandleProtocol(kimg, &li_guid, (void **)&kli);
    if (EFI_ERROR(st)) { fail(L"HandleProtocol(kernel)", st); return st; }
    kli->LoadOptions     = e->cmdline;
    kli->LoadOptionsSize = (UINT32)((slen(e->cmdline) + 1) * sizeof(CHAR16));

    print(L"  [ok] cmdline: "); print(e->cmdline); print(L"\r\n");
    print(L"  [->] starting VerkOS\r\n\r\n");

    st = BS->StartImage(kimg, 0, 0);        /* on success, never returns */
    fail(L"StartImage", st);
    return st;
}

EFI_STATUS EFIAPI efi_main(EFI_HANDLE image, EFI_SYSTEM_TABLE *systab) {
    ST = systab; BS = systab->BootServices; RT = systab->RuntimeServices;

    EFI_FILE_PROTOCOL *root = volume_root(image);
    if (root) {
        unsigned char *cfg = read_file(root, CONFIG_PATH, 0);
        if (cfg) { parse_config(cfg); BS->FreePool(cfg); }
        scan_kernels(root);      /* add any kernel the config did not mention */
    }
    /* validate whatever the config gave us */
    for (int i = 0; i < n_entries; i++)
        if (!entries[i].kernel[0]) { scopy(entries[i].kernel, L"\\vmlinuz", PATH_MAX); }
    if (n_entries == 0) builtin_defaults();
    if (cfg_default < 0 || cfg_default >= n_entries) cfg_default = 0;

    /* Prefer whatever booted successfully last time. If the previous attempt was
     * never confirmed by the running system, do not silently retry it — select
     * it but stop the countdown so a person can choose. */
    int last_failed = 0, last = recall_choice(&last_failed);
    int sel = (last >= 0 && last < n_entries) ? last : cfg_default;
    int cancelled = 0, remain = cfg_timeout;
    /* Give a person time to intervene, but still boot on its own: a headless
     * machine must never be stranded at the menu because a confirmation was
     * missed. */
    if (last_failed && remain < 15) remain = 15;
    draw_banner();
    if (last_failed) {
        at(4, MENU_ROW + n_entries + 2);
        attr(EFI_WHITE);
        print(L"the last boot did not finish starting — pick an entry");
        attr(EFI_LIGHTGRAY);
    }
    draw_entries(sel);
    draw_timer(remain, cancelled);

    /* poll for keys in 100ms ticks; ten ticks make a second of countdown */
    for (int tick = 0; ; tick++) {
        EFI_INPUT_KEY key;
        if (ST->ConIn && !EFI_ERROR(ST->ConIn->ReadKeyStroke(ST->ConIn, &key))) {
            if (key.ScanCode == SCAN_UP)   { sel = sel ? sel - 1 : n_entries - 1; cancelled = 1; }
            else if (key.ScanCode == SCAN_DOWN) { sel = (sel + 1) % n_entries;    cancelled = 1; }
            else if (key.ScanCode == SCAN_ESC)  { cancelled = 1; }
            else if (key.UnicodeChar == L'\r')  break;              /* boot it */
            else if (key.UnicodeChar == L'e' || key.UnicodeChar == L'E') {
                /* edit this entry's command line, then redraw */
                cancelled = 1;
                edit_line(entries[sel].cmdline, CMDLINE_MAX, L"cmdline: ");
                draw_banner(); draw_entries(sel);
            }
            else if (key.UnicodeChar == L'r' || key.UnicodeChar == L'R') do_reset(EfiResetCold);
            else if (key.UnicodeChar == L's' || key.UnicodeChar == L'S') do_reset(EfiResetShutdown);
            else if (key.UnicodeChar == L'f' || key.UnicodeChar == L'F') boot_to_firmware();
            else if (key.UnicodeChar >= L'1' && key.UnicodeChar < L'1' + n_entries) {
                sel = key.UnicodeChar - L'1'; break;                /* number = pick + boot */
            } else cancelled = 1;
            draw_entries(sel);
            draw_timer(remain, cancelled);
        }
        if (!cancelled && tick && tick % 10 == 0) {
            if (--remain <= 0) break;                               /* timeout: boot default */
            draw_timer(remain, cancelled);
        }
        BS->Stall(100000);                                          /* 100 ms */
    }

    if (!root) { fail(L"opening the boot volume", EFI_NOT_FOUND); goto hang; }
    remember_choice(sel, 1);            /* "trying"; urverk clears it once up */
    boot_entry(image, root, &entries[sel]);

hang:
    print(L"\r\n  vboot halted.\r\n");
    for (;;) BS->Stall(1000000);
    return EFI_LOAD_ERROR;
}
