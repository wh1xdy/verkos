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

#define CONFIG_PATH   L"\\vboot.conf"
#define MAX_ENTRIES   8
#define TITLE_MAX     64
#define PATH_MAX      128
#define CMDLINE_MAX   512
#define MENU_ROW      8               /* first screen row the entries occupy */

typedef struct {
    CHAR16 title[TITLE_MAX];
    CHAR16 kernel[PATH_MAX];
    CHAR16 cmdline[CMDLINE_MAX];
} entry_t;

static EFI_SYSTEM_TABLE  *ST;
static EFI_BOOT_SERVICES *BS;
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
    UINTN want = (UINTN)size;
    if (EFI_ERROR(f->Read(f, &want, buf))) { BS->FreePool(buf); f->Close(f); return 0; }
    ((unsigned char *)buf)[want] = 0;                 /* NUL-terminate for the parser */
    f->Close(f);
    if (out_size) *out_size = want;
    return buf;
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
                cur->kernel[0] = 0; cur->cmdline[0] = 0;
            } else cur = 0;
        } else if ((off = keyword(p, "kernel"))  && cur) {
            copy_line(p + off, cur->kernel, PATH_MAX);
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

/* ---- menu ---------------------------------------------------------------- */
static void draw_banner(void) {
    ST->ConOut->ClearScreen(ST->ConOut);
    attr(EFI_LIGHTCYAN);
    at(2, 1); print(L"##   ##   VerkOS");
    attr(EFI_CYAN);
    at(2, 2); print(L" ## ##    vboot 0.1");
    at(2, 3); print(L"  ###     our own UEFI bootloader");
    at(2, 4); print(L"   #");
    attr(EFI_DARKGRAY);
    at(2, 6); print(L"select with arrows or a number, Enter boots, Esc cancels the timer");
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

/* ---- boot ---------------------------------------------------------------- */
static EFI_STATUS boot_entry(EFI_HANDLE image, EFI_FILE_PROTOCOL *root, entry_t *e) {
    EFI_GUID li_guid = EFI_LOADED_IMAGE_PROTOCOL_GUID;
    EFI_STATUS st;

    ST->ConOut->ClearScreen(ST->ConOut);
    print(L"\r\n  vboot: "); print(e->title); print(L"\r\n");
    print(L"  [..] loading "); print(e->kernel); print(L"\r\n");

    UINTN ksize = 0;
    void *kbuf = read_file(root, e->kernel, &ksize);
    if (!kbuf) { fail(L"reading the kernel", EFI_NOT_FOUND); return EFI_NOT_FOUND; }
    print(L"  [ok] kernel loaded, "); print_num(ksize); print(L" bytes\r\n");

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
    ST = systab; BS = systab->BootServices;

    EFI_FILE_PROTOCOL *root = volume_root(image);
    if (root) {
        unsigned char *cfg = read_file(root, CONFIG_PATH, 0);
        if (cfg) { parse_config(cfg); BS->FreePool(cfg); }
    }
    /* validate whatever the config gave us */
    for (int i = 0; i < n_entries; i++)
        if (!entries[i].kernel[0]) { scopy(entries[i].kernel, L"\\vmlinuz", PATH_MAX); }
    if (n_entries == 0) builtin_defaults();
    if (cfg_default < 0 || cfg_default >= n_entries) cfg_default = 0;

    int sel = cfg_default, cancelled = 0, remain = cfg_timeout;
    draw_banner();
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
    boot_entry(image, root, &entries[sel]);

hang:
    print(L"\r\n  vboot halted.\r\n");
    for (;;) BS->Stall(1000000);
    return EFI_LOAD_ERROR;
}
