/* vfb — VerkOS' first graphical program: a framebuffer splash.
 *
 * Opens the kernel framebuffer (/dev/fb0, backed by virtio-gpu via DRM fbdev
 * emulation), mmaps it, and paints the VerkOS identity in real pixels: a
 * cyan->purple vertical gradient, the block-V logo, a "VERKOS" wordmark, and
 * the palette swatches. No libraries — just ioctls + mmap, matching the
 * "write our own" ethos. This is VerkOS' first pixels on a screen.
 *
 * Usage: vfb [/dev/fbN]   (defaults to /dev/fb0)
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdint.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/fb.h>

static struct fb_var_screeninfo vi;
static struct fb_fix_screeninfo fx;
static uint8_t *fb;
static long fbsize;
static int W, H, BPP, STRIDE;
static int roff, goff, boff;         /* channel bit offsets from the fb */

/* ---- pixel plotting (honours the fb's actual channel layout) ------------ */
static inline uint32_t pack(uint8_t r, uint8_t g, uint8_t b) {
    if (BPP == 16) return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
    return ((uint32_t)r << roff) | ((uint32_t)g << goff) | ((uint32_t)b << boff);
}
static inline void px(int x, int y, uint8_t r, uint8_t g, uint8_t b) {
    if (x < 0 || y < 0 || x >= W || y >= H) return;
    uint8_t *p = fb + (long)y * STRIDE + (long)x * (BPP / 8);
    uint32_t v = pack(r, g, b);
    if (BPP == 32) *(uint32_t *)p = v;
    else if (BPP == 16) *(uint16_t *)p = (uint16_t)v;
    else if (BPP == 24) { p[0] = v & 0xff; p[1] = (v >> 8) & 0xff; p[2] = (v >> 16) & 0xff; }
}
static void fill(int x0, int y0, int w, int h, uint8_t r, uint8_t g, uint8_t b) {
    for (int y = y0; y < y0 + h; y++) for (int x = x0; x < x0 + w; x++) px(x, y, r, g, b);
}

/* ---- palette (matches verkfetch: cyan -> purple) ------------------------ */
static void lerp(int t, int n, const uint8_t a[3], const uint8_t b[3], uint8_t out[3]) {
    for (int i = 0; i < 3; i++) out[i] = a[i] + (b[i] - a[i]) * t / (n > 1 ? n - 1 : 1);
}
static const uint8_t C_CYAN[3]  = { 0x5e, 0xf1, 0xff };
static const uint8_t C_BLUE[3]  = { 0x6a, 0xa8, 0xff };
static const uint8_t C_MAG[3]   = { 0xe8, 0x79, 0xf9 };
static const uint8_t C_PURP[3]  = { 0xb0, 0x26, 0xd3 };
static const uint8_t BG_TOP[3]  = { 0x0a, 0x0e, 0x16 };   /* deep near-black blue */
static const uint8_t BG_BOT[3]  = { 0x1a, 0x0f, 0x2e };   /* dark purple */

/* thick V: two arms from the top corners converging to a bottom point */
static void draw_V(int cx, int y0, int y1, int halfw, int thick) {
    int span = y1 - y0;
    for (int y = y0; y <= y1; y++) {
        int f = y - y0;                                    /* 0..span */
        uint8_t col[3];
        lerp(f, span, C_CYAN, C_PURP, col);                /* per-row gradient */
        int xl = (cx - halfw) + halfw * f / span;          /* left arm x */
        int xr = (cx + halfw) - halfw * f / span;          /* right arm x */
        for (int x = xl; x < xl + thick; x++) px(x, y, col[0], col[1], col[2]);
        for (int x = xr - thick; x < xr; x++) px(x, y, col[0], col[1], col[2]);
    }
}

/* ---- 8x8 bitmap font (just what the wordmark needs: V E R K O S 0-9 . space) */
/* each glyph: 8 bytes, MSB = leftmost pixel */
static int glyph(int ch, unsigned char out[8]) {
    static const struct { char c; unsigned char rows[8]; } F[] = {
        {'V', {0x82,0x82,0x82,0x82,0x44,0x44,0x28,0x10}},
        {'E', {0xFE,0x80,0x80,0xF8,0x80,0x80,0x80,0xFE}},
        {'R', {0xFC,0x82,0x82,0xFC,0x90,0x88,0x84,0x82}},
        {'K', {0x84,0x88,0x90,0xE0,0x90,0x88,0x84,0x82}},
        {'O', {0x7C,0x82,0x82,0x82,0x82,0x82,0x82,0x7C}},
        {'S', {0x7E,0x80,0x80,0x7C,0x02,0x02,0x02,0xFC}},
        {'0', {0x7C,0x86,0x8A,0x92,0xA2,0xC2,0x82,0x7C}},
        {'1', {0x10,0x30,0x10,0x10,0x10,0x10,0x10,0x7C}},
        {'.', {0x00,0x00,0x00,0x00,0x00,0x00,0x18,0x18}},
        {' ', {0,0,0,0,0,0,0,0}},
    };
    for (unsigned i = 0; i < sizeof F / sizeof *F; i++)
        if (F[i].c == ch) { memcpy(out, F[i].rows, 8); return 1; }
    return 0;
}
/* draw a string with an integer pixel scale */
static int text(int x, int y, int scale, const char *s, uint8_t r, uint8_t g, uint8_t b) {
    for (; *s; s++) {
        unsigned char gl[8];
        if (glyph(*s, gl)) {
            for (int ry = 0; ry < 8; ry++)
                for (int rx = 0; rx < 8; rx++)
                    if (gl[ry] & (0x80 >> rx))
                        fill(x + rx * scale, y + ry * scale, scale, scale, r, g, b);
        }
        x += 9 * scale;                                    /* 8px glyph + 1px gap */
    }
    return x;
}

int main(int argc, char **argv) {
    const char *dev = argc > 1 ? argv[1] : "/dev/fb0";
    int fd = open(dev, O_RDWR);
    if (fd < 0) { fprintf(stderr, "vfb: %s: %s\n", dev, strerror(errno)); return 1; }
    if (ioctl(fd, FBIOGET_VSCREENINFO, &vi) < 0 || ioctl(fd, FBIOGET_FSCREENINFO, &fx) < 0) {
        fprintf(stderr, "vfb: FBIOGET_*SCREENINFO: %s\n", strerror(errno)); return 1;
    }
    W = vi.xres; H = vi.yres; BPP = vi.bits_per_pixel; STRIDE = fx.line_length;
    roff = vi.red.offset; goff = vi.green.offset; boff = vi.blue.offset;
    fbsize = (long)STRIDE * H;
    fb = mmap(NULL, fbsize, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (fb == MAP_FAILED) { fprintf(stderr, "vfb: mmap: %s\n", strerror(errno)); return 1; }
    fprintf(stderr, "vfb: %s %dx%d %dbpp stride=%d\n", dev, W, H, BPP, STRIDE);

    /* 1. vertical background gradient (deep blue -> dark purple) */
    for (int y = 0; y < H; y++) {
        uint8_t c[3]; lerp(y, H, BG_TOP, BG_BOT, c);
        for (int x = 0; x < W; x++) px(x, y, c[0], c[1], c[2]);
    }
    /* 2. the block-V logo, centred, upper-middle */
    int vh = H * 42 / 100, vy = H * 14 / 100;
    int halfw = vh * 55 / 100, thick = vh / 9;
    draw_V(W / 2, vy, vy + vh, halfw, thick);
    /* 3. "VERKOS" wordmark, centred under the V */
    int sc = W >= 1024 ? 6 : (W >= 640 ? 4 : 2);
    const char *word = "VERKOS";
    int tw = (int)strlen(word) * 9 * sc - sc;
    text(W / 2 - tw / 2, vy + vh + H / 12, sc, word, 0xe6, 0xed, 0xf3);
    /* 4. version line, smaller */
    int sc2 = sc > 2 ? sc / 2 : 1;
    const char *ver = "0.1";
    int vw = (int)strlen(ver) * 9 * sc2 - sc2;
    text(W / 2 - vw / 2, vy + vh + H / 12 + 10 * sc, sc2, ver, 0x7f, 0xf0, 0xff);
    /* 5. palette swatches along the bottom */
    const uint8_t *pal[] = { C_CYAN, C_BLUE, C_MAG, C_PURP };
    int sw = W / 12, sh = H / 22, gap = sw / 6;
    int total = 4 * sw + 3 * gap, sx = W / 2 - total / 2, sy = H - sh - H / 20;
    for (int i = 0; i < 4; i++)
        fill(sx + i * (sw + gap), sy, sw, sh, pal[i][0], pal[i][1], pal[i][2]);

    munmap(fb, fbsize);
    close(fd);
    return 0;
}
