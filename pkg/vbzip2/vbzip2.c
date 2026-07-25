/* vbzip2 — VerkOS' own bzip2/bunzip2.
 *
 * A from-scratch bzip2 codec: the full decode path (Huffman with the reference
 * limit/base/perm tables, MTF+RLE2 inverse, inverse Burrows-Wheeler, RLE1), and
 * an encode path (RLE1, BWT by rotation sort, MTF+RLE2, canonical Huffman with
 * two tables). Interoperates with standard bzip2. Multi-call: bzip2/bunzip2/bzcat.
 *
 *   bzip2 [-c] [-k] [-d] FILE...     compress (or -d decompress)
 *   bunzip2 [-c] [-k] FILE... | bzcat FILE...
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ---- bzip2 CRC (MSB-first) ---------------------------------------------- */
static unsigned crctab[256];
static void crc_init(void) { for (unsigned i = 0; i < 256; i++) { unsigned c = i << 24; for (int k = 0; k < 8; k++) c = (c & 0x80000000u) ? (c << 1) ^ 0x04c11db7u : c << 1; crctab[i] = c; } }
static unsigned crc_upd(unsigned crc, const unsigned char *p, size_t n) { while (n--) crc = (crc << 8) ^ crctab[((crc >> 24) ^ *p++) & 0xff]; return crc; }

struct buf { unsigned char *d; size_t n, cap; };
static void bput(struct buf *b, int c) { if (b->n >= b->cap) { b->cap = b->cap ? b->cap * 2 : 1 << 16; b->d = realloc(b->d, b->cap); } b->d[b->n++] = (unsigned char)c; }

/* ---- MSB-first bit I/O -------------------------------------------------- */
struct bitr { const unsigned char *d; size_t n, pos; unsigned buf, cnt; };
static unsigned rbits(struct bitr *r, int k) { unsigned v = 0; while (k--) { if (r->cnt == 0) { r->buf = r->pos < r->n ? r->d[r->pos++] : 0; r->cnt = 8; } r->cnt--; v = (v << 1) | ((r->buf >> r->cnt) & 1); } return v; }
struct bitw { struct buf *b; unsigned buf, cnt; };
static void wbits(struct bitw *w, unsigned v, int k) { while (k--) { w->buf = (w->buf << 1) | ((v >> k) & 1); if (++w->cnt == 8) { bput(w->b, w->buf & 0xff); w->cnt = 0; w->buf = 0; } } }
static void wflush(struct bitw *w) { while (w->cnt) { w->buf = (w->buf << 1); if (++w->cnt == 8) { bput(w->b, w->buf & 0xff); w->cnt = 0; w->buf = 0; } } }

/* ============================ DECODE ===================================== */
#define MAXLEN 23
static int decode_block(struct bitr *r, struct buf *out, unsigned *crc_out) {
    unsigned blockcrc = rbits(r, 32);
    if (rbits(r, 1)) return -1;                    /* randomized: unsupported (obsolete) */
    unsigned origPtr = rbits(r, 24);
    /* symbol map */
    unsigned inUse16 = rbits(r, 16); unsigned char seq[256]; int nInUse = 0;
    for (int i = 0; i < 16; i++) if (inUse16 & (0x8000 >> i)) { unsigned bits = rbits(r, 16); for (int j = 0; j < 16; j++) if (bits & (0x8000 >> j)) seq[nInUse++] = i * 16 + j; }
    int alpha = nInUse + 2;
    int nGroups = rbits(r, 3), nSel = rbits(r, 15);
    /* selectors (MTF over group indices) */
    unsigned char selmtf[32768];
    for (int i = 0; i < nSel; i++) { int j = 0; while (rbits(r, 1)) j++; selmtf[i] = j; }
    unsigned char pos[6], sel[32768];
    for (int i = 0; i < nGroups; i++) pos[i] = i;
    for (int i = 0; i < nSel; i++) { int v = selmtf[i]; unsigned char tmp = pos[v]; while (v > 0) { pos[v] = pos[v-1]; v--; } pos[0] = tmp; sel[i] = tmp; }
    /* huffman tables */
    static int limit[6][MAXLEN+2], base[6][MAXLEN+2], perm[6][258]; int minL[6];
    for (int g = 0; g < nGroups; g++) {
        unsigned char len[258]; int c = rbits(r, 5);
        for (int s = 0; s < alpha; s++) { while (rbits(r, 1)) { if (rbits(r, 1)) c--; else c++; } len[s] = c; }
        int mn = 32, mx = 0; for (int s = 0; s < alpha; s++) { if (len[s] > mx) mx = len[s]; if (len[s] < mn) mn = len[s]; }
        int pp = 0; for (int l = mn; l <= mx; l++) for (int s = 0; s < alpha; s++) if (len[s] == l) perm[g][pp++] = s;
        for (int i = 0; i <= MAXLEN; i++) base[g][i] = 0;
        for (int s = 0; s < alpha; s++) base[g][len[s] + 1]++;
        for (int i = 1; i <= MAXLEN; i++) base[g][i] += base[g][i-1];
        for (int i = 0; i <= MAXLEN; i++) limit[g][i] = 0;
        int vec = 0; for (int l = mn; l <= mx; l++) { vec += base[g][l+1] - base[g][l]; limit[g][l] = vec - 1; vec <<= 1; }
        for (int l = mn + 1; l <= mx; l++) base[g][l] = ((limit[g][l-1] + 1) << 1) - base[g][l];
        minL[g] = mn;
    }
    /* decode MTF/RLE2 into the BWT last column (tt) with per-byte counts */
    int EOB = alpha - 1; unsigned *tt = malloc((size_t)(9 << 20) * sizeof(unsigned)); if (!tt) return -1;
    int cftab[257]; memset(cftab, 0, sizeof cftab);
    unsigned char mtf[256]; for (int i = 0; i < nInUse; i++) mtf[i] = i;   /* MTF holds symbol indices */
    long nblock = 0; int grpPos = 0, grpIdx = -1, gsel = 0, gminlen = 0; int *glim = NULL, *gbase = NULL, *gperm = NULL;
    long run = 0; int runbit = 0;
    #define GETSYM(SYM) do { if (grpPos == 0) { grpIdx++; grpPos = 50; gsel = sel[grpIdx]; glim = limit[gsel]; gbase = base[gsel]; gperm = perm[gsel]; gminlen = minL[gsel]; } grpPos--; int zn = gminlen, zv = rbits(r, zn); while (zv > glim[zn]) { zn++; zv = (zv << 1) | rbits(r, 1); } SYM = gperm[zv - gbase[zn]]; } while (0)
    for (;;) {
        int sym; GETSYM(sym);
        if (sym == EOB) break;
        if (sym == 0 || sym == 1) { run += (sym + 1) * (1L << runbit); runbit++; continue; }
        if (run) { int b = seq[mtf[0]]; cftab[b + 1] += run; while (run--) tt[nblock++] = b; run = 0; runbit = 0; }
        /* MTF value = sym-1 */
        int v = sym - 1; unsigned char tmp = mtf[v]; while (v > 0) { mtf[v] = mtf[v-1]; v--; } mtf[0] = tmp;
        int b = seq[tmp]; cftab[b + 1]++; tt[nblock++] = b;
    }
    if (run) { int b = seq[mtf[0]]; cftab[b + 1] += run; while (run--) tt[nblock++] = b; }
    /* inverse BWT */
    for (int i = 1; i <= 256; i++) cftab[i] += cftab[i-1];
    for (long i = 0; i < nblock; i++) { int b = tt[i] & 0xff; tt[cftab[b]] |= (i << 8); cftab[b]++; }
    unsigned tpos = tt[origPtr] >> 8;
    /* RLE1 decode */
    unsigned bcrc = 0xffffffff; int prev = -1, rl = 0;
    struct buf blk = {0};
    for (long i = 0; i < nblock; i++) {
        int b = tt[tpos] & 0xff; tpos = tt[tpos] >> 8;
        bput(&blk, b);
        if (b == prev) { rl++; if (rl == 4) { unsigned cnt = 0; if (i + 1 < nblock) { cnt = tt[tpos] & 0xff; tpos = tt[tpos] >> 8; i++; } for (unsigned k = 0; k < cnt; k++) bput(&blk, b); rl = 0; prev = -1; continue; } }
        else { rl = 1; prev = b; }
    }
    free(tt);
    bcrc = crc_upd(0xffffffff, blk.d, blk.n) ^ 0xffffffff;
    if (bcrc != blockcrc) { fprintf(stderr, "bzip2: block CRC mismatch\n"); free(blk.d); return -1; }
    for (size_t i = 0; i < blk.n; i++) bput(out, blk.d[i]);
    *crc_out = (*crc_out << 1 | *crc_out >> 31) ^ blockcrc;
    free(blk.d);
    return 0;
}
static int decompress(const unsigned char *in, size_t n, struct buf *out) {
    if (n < 4 || in[0] != 'B' || in[1] != 'Z' || in[2] != 'h') { fprintf(stderr, "bzip2: bad magic\n"); return -1; }
    struct bitr r = { in, n, 4, 0, 0 };
    unsigned combined = 0;
    for (;;) {
        unsigned hi = rbits(&r, 24), lo = rbits(&r, 24);
        if (hi == 0x314159 && lo == 0x265359) { if (decode_block(&r, out, &combined) < 0) return -1; }
        else if (hi == 0x177245 && lo == 0x385090) { rbits(&r, 32); break; }   /* EOS + combined CRC */
        else { fprintf(stderr, "bzip2: bad block magic\n"); return -1; }
    }
    return 0;
}

/* ============================ ENCODE ===================================== */
/* length-limited canonical Huffman (bzip2-style), maxlen 17 */
static void mk_lengths(int *freq, int alpha, unsigned char *len, int maxlen) {
    /* simple: build via repeated merge (package-merge-ish approximation), then clamp */
    int H[516]; int par[516]; for (int i = 0; i < alpha; i++) { H[i] = (freq[i] ? freq[i] : 1); }
    /* weight array with parent tracking (classic Huffman) */
    typedef struct { long w; int id; } Nd;
    Nd heap[520]; int nh = 0;
    int nodes = alpha; long wt[520]; int lft[520], rgt[520];
    for (int i = 0; i < alpha; i++) { wt[i] = H[i]; lft[i] = rgt[i] = -1; heap[nh].w = wt[i]; heap[nh].id = i; nh++; }
    /* selection sort based combine (alpha<=258, fine) */
    while (nh > 1) {
        int a = 0; for (int i = 1; i < nh; i++) if (heap[i].w < heap[a].w) a = i; Nd na = heap[a]; heap[a] = heap[--nh];
        int b = 0; for (int i = 1; i < nh; i++) if (heap[i].w < heap[b].w) b = i; Nd nb = heap[b]; heap[b] = heap[--nh];
        int id = nodes++; wt[id] = na.w + nb.w; lft[id] = na.id; rgt[id] = nb.id; heap[nh].w = wt[id]; heap[nh].id = id; nh++;
    }
    int root = heap[0].id;
    /* depth = code length */
    int stackn[1040], stackd[1040], sp = 0; stackn[sp] = root; stackd[sp] = 0; sp++;
    while (sp) { sp--; int id = stackn[sp], d = stackd[sp]; if (lft[id] < 0) { len[id] = d < 1 ? 1 : (d > maxlen ? maxlen : d); } else { stackn[sp] = lft[id]; stackd[sp] = d+1; sp++; stackn[sp] = rgt[id]; stackd[sp] = d+1; sp++; } }
    (void)par;
}
static void gen_codes(unsigned char *len, int alpha, unsigned *code) {
    int mn = 32, mx = 0; for (int i = 0; i < alpha; i++) { if (len[i] > mx) mx = len[i]; if (len[i] < mn) mn = len[i]; }
    unsigned vec = 0;
    for (int l = mn; l <= mx; l++) { for (int i = 0; i < alpha; i++) if (len[i] == l) code[i] = vec++; vec <<= 1; }
}
static int rle1(const unsigned char *in, size_t n, unsigned char *out) {
    int oi = 0, rl = 0, prev = -1;
    for (size_t i = 0; i < n; i++) { int b = in[i]; out[oi++] = b; if (b == prev) { rl++; if (rl == 4) { size_t cnt = 0; while (i + 1 < n && in[i+1] == b && cnt < 255) { i++; cnt++; } out[oi++] = cnt; rl = 0; prev = -1; } } else { rl = 1; prev = b; } }
    return oi;
}
static const unsigned char *bwt_src; static int bwt_n;
static int bwt_cmp(const void *pa, const void *pb) {
    int a = *(const int *)pa, b = *(const int *)pb;
    for (int i = 0; i < bwt_n; i++) { unsigned char ca = bwt_src[(a+i)%bwt_n], cb = bwt_src[(b+i)%bwt_n]; if (ca != cb) return ca - cb; }
    return a - b;
}
static int compress_block(struct bitw *wp, const unsigned char *raw, size_t rawn, unsigned *combined) {
    unsigned bcrc = crc_upd(0xffffffff, raw, rawn) ^ 0xffffffff;
    *combined = (*combined << 1 | *combined >> 31) ^ bcrc;
    unsigned char *r1 = malloc(rawn * 2 + 16);
    int n = rle1(raw, rawn, r1);
    if (n <= 0) { free(r1); return 0; }              /* empty block: nothing to emit */
    /* BWT */
    int *idx = malloc((size_t)n * sizeof(int)); for (int i = 0; i < n; i++) idx[i] = i;
    bwt_src = r1; bwt_n = n; qsort(idx, n, sizeof(int), bwt_cmp);
    unsigned char *L = malloc((size_t)n); int origPtr = 0;
    for (int i = 0; i < n; i++) { L[i] = r1[(idx[i] + n - 1) % n]; if (idx[i] == 0) origPtr = i; }
    free(idx);
    /* used map */
    int used[256] = {0}; for (int i = 0; i < n; i++) used[L[i]] = 1;
    unsigned char seq[256]; int nInUse = 0; for (int i = 0; i < 256; i++) if (used[i]) seq[nInUse++] = i;
    unsigned char unseq[256]; for (int i = 0; i < nInUse; i++) unseq[seq[i]] = i;
    int EOB = nInUse + 1, alpha = nInUse + 2;
    /* MTF + RLE2 -> symbol stream */
    int *sym = malloc((n + 1) * sizeof(int)); int ns = 0;
    unsigned char mtf[256]; for (int i = 0; i < nInUse; i++) mtf[i] = i;
    long zrun = 0;
    #define FLUSHZ do { while (zrun > 0) { zrun--; sym[ns++] = (zrun & 1) ? 1 : 0; zrun >>= 1; } } while (0)
    for (int i = 0; i < n; i++) {
        int v = unseq[L[i]]; int j = 0; while (mtf[j] != v) j++;
        int mtfval = j;                                  /* save before the move loop clobbers j */
        unsigned char tmp = mtf[j]; while (j > 0) { mtf[j] = mtf[j-1]; j--; } mtf[0] = tmp;
        if (mtfval == 0) { zrun++; } else { FLUSHZ; zrun = 0; sym[ns++] = mtfval + 1; }
    }
    { long z = zrun; while (z > 0) { z--; sym[ns++] = (z & 1) ? 1 : 0; z >>= 1; } }
    sym[ns++] = EOB;
    /* frequencies + one Huffman table (used twice, per format's >=2 tables) */
    int freq[258] = {0}; for (int i = 0; i < ns; i++) freq[sym[i]]++;
    unsigned char len[258]; mk_lengths(freq, alpha, len, 17);
    unsigned code[258]; gen_codes(len, alpha, code);
    /* write block into the shared, continuous bit stream */
    struct bitw w = *wp;
    wbits(&w, 0x314159, 24); wbits(&w, 0x265359, 24);
    wbits(&w, bcrc, 32); wbits(&w, 0, 1); wbits(&w, origPtr, 24);
    /* symbol map */
    unsigned in16 = 0; for (int i = 0; i < nInUse; i++) in16 |= 0x8000 >> (seq[i] >> 4);
    wbits(&w, in16, 16);
    for (int i = 0; i < 16; i++) if (in16 & (0x8000 >> i)) { unsigned bits = 0; for (int b = 0; b < 16; b++) if (used[i*16 + b]) bits |= 0x8000 >> b; wbits(&w, bits, 16); }
    int nGroups = 2, nSel = (ns + 49) / 50;
    wbits(&w, nGroups, 3); wbits(&w, nSel, 15);
    for (int i = 0; i < nSel; i++) wbits(&w, 0, 1);      /* selector MTF: all table 0 -> single 0-bit */
    for (int g = 0; g < nGroups; g++) {                 /* two identical tables */
        int c = len[0]; wbits(&w, c, 5);
        for (int s = 0; s < alpha; s++) { int t = len[s]; while (c < t) { wbits(&w, 2, 2); c++; } while (c > t) { wbits(&w, 3, 2); c--; } wbits(&w, 0, 1); }
    }
    for (int i = 0; i < ns; i++) wbits(&w, code[sym[i]], len[sym[i]]);
    *wp = w;                                          /* keep the bit position; do NOT flush here */
    free(r1); free(L); free(sym);
    return 0;
}
static int compress(const unsigned char *in, size_t n, struct buf *out, int level) {
    bput(out, 'B'); bput(out, 'Z'); bput(out, 'h'); bput(out, '0' + level);
    size_t bs = (size_t)level * 100000;
    unsigned combined = 0;
    struct bitw w = { out, 0, 0 };                     /* one continuous bit stream after the header */
    for (size_t off = 0; off < n; off += bs) {
        size_t chunk = n - off < bs ? n - off : bs;
        compress_block(&w, in + off, chunk, &combined);
    }
    wbits(&w, 0x177245, 24); wbits(&w, 0x385090, 24); wbits(&w, combined, 32);
    wflush(&w);                                        /* flush only at the very end */
    return 0;
}

/* ---- file glue ---------------------------------------------------------- */
static unsigned char *slurp(FILE *f, size_t *n) { struct buf b = {0}; unsigned char t[65536]; size_t r; while ((r = fread(t, 1, sizeof t, f)) > 0) for (size_t i = 0; i < r; i++) bput(&b, t[i]); *n = b.n; return b.d; }

int main(int argc, char **argv) {
    crc_init();
    const char *me = strrchr(argv[0], '/'); me = me ? me + 1 : argv[0];
    int dec = !strcmp(me, "bunzip2") || !strcmp(me, "bzcat");
    int tostdout = !strcmp(me, "bzcat"), keep = 0, level = 9;
    const char *files[256]; int nf = 0;
    for (int i = 1; i < argc; i++) {
        if (argv[i][0] == '-' && argv[i][1]) {
            /* Combined short flags, as bzip2 accepts them — "-dc" is the usual
             * way to write "decompress to stdout". An unrecognised flag is an
             * error rather than something to ignore: falling through used to
             * leave the tool in compress mode, which then replaced the named
             * file with a .bz2 of itself and deleted the original. */
            for (const char *f = argv[i] + 1; *f; f++) {
                if      (*f == 'd') dec = 1;
                else if (*f == 'c') tostdout = 1;
                else if (*f == 'k') keep = 1;
                else if (*f == 'z') dec = 0;
                else if (*f >= '1' && *f <= '9') level = *f - '0';
                else { fprintf(stderr, "%s: unknown option -%c\n", me, *f); return 1; }
            }
            continue;
        }
        if (argv[i][0] != '-') files[nf++] = argv[i];
    }
    if (nf == 0) {
        size_t n; unsigned char *in = slurp(stdin, &n); struct buf out = {0};
        if (dec) { if (decompress(in, n, &out) < 0) return 1; } else compress(in, n, &out, level);
        fwrite(out.d, 1, out.n, stdout); free(in); free(out.d); return 0;
    }
    int ret = 0;
    for (int i = 0; i < nf; i++) {
        FILE *f = fopen(files[i], "rb"); if (!f) { fprintf(stderr, "bzip2: %s: no such file\n", files[i]); ret = 1; continue; }
        size_t n; unsigned char *in = slurp(f, &n); fclose(f); struct buf out = {0};
        if (dec) { if (decompress(in, n, &out) < 0) { free(in); ret = 1; continue; } } else compress(in, n, &out, level);
        if (tostdout) fwrite(out.d, 1, out.n, stdout);
        else { char on[1024]; size_t l = strlen(files[i]);
            if (dec) { if (l > 4 && !strcmp(files[i]+l-4, ".bz2")) snprintf(on, sizeof on, "%.*s", (int)(l-4), files[i]); else snprintf(on, sizeof on, "%s.out", files[i]); }
            else snprintf(on, sizeof on, "%s.bz2", files[i]);
            FILE *of = fopen(on, "wb"); if (of) { fwrite(out.d, 1, out.n, of); fclose(of); if (!keep) unlink(files[i]); } }
        free(in); free(out.d);
    }
    return ret;
}
