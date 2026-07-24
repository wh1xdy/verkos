/* vgzip — VerkOS' own gzip/gunzip/zcat.
 *
 * A from-scratch DEFLATE (RFC 1951) codec — full inflate, and a deflate using
 * LZ77 (hash-chain matching) with fixed Huffman codes — wrapped in the gzip
 * container (RFC 1952) with a CRC-32. Interoperates with standard gzip both
 * ways. Multi-call: installed as gzip, gunzip, and zcat.
 *
 *   gzip [-c] [-k] [-d] FILE...     compress (or -d decompress)
 *   gunzip [-c] [-k] FILE...        decompress
 *   zcat FILE...                    decompress to stdout
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ---- CRC-32 ------------------------------------------------------------- */
static unsigned crc_tab[256];
static void crc_init(void) {
    for (unsigned i = 0; i < 256; i++) {
        unsigned c = i;
        for (int k = 0; k < 8; k++) c = (c & 1) ? 0xedb88320u ^ (c >> 1) : c >> 1;
        crc_tab[i] = c;
    }
}
static unsigned crc32(unsigned crc, const unsigned char *p, size_t n) {
    crc = ~crc;
    while (n--) crc = crc_tab[(crc ^ *p++) & 0xff] ^ (crc >> 8);
    return ~crc;
}

/* ---- growable byte buffer ----------------------------------------------- */
struct buf { unsigned char *d; size_t n, cap; };
static void bput(struct buf *b, int c) {
    if (b->n >= b->cap) { b->cap = b->cap ? b->cap * 2 : 65536; b->d = realloc(b->d, b->cap); }
    b->d[b->n++] = (unsigned char)c;
}
static void bputmem(struct buf *b, const void *p, size_t n) {
    while (b->n + n > b->cap) { b->cap = b->cap ? b->cap * 2 : 65536; b->d = realloc(b->d, b->cap); }
    memcpy(b->d + b->n, p, n); b->n += n;
}

/* ==== INFLATE ============================================================ */
struct bitr { const unsigned char *d; size_t n, pos; unsigned bitbuf, bitcnt; };
static int getbit(struct bitr *r) {
    if (r->bitcnt == 0) { if (r->pos >= r->n) return -1; r->bitbuf = r->d[r->pos++]; r->bitcnt = 8; }
    int b = r->bitbuf & 1; r->bitbuf >>= 1; r->bitcnt--; return b;
}
static int getbits(struct bitr *r, int n) {
    int v = 0; for (int i = 0; i < n; i++) { int b = getbit(r); if (b < 0) return -1; v |= b << i; }
    return v;
}
/* canonical Huffman decode table */
struct huff { unsigned short count[16]; unsigned short sym[288]; };
static void huff_build(struct huff *h, const unsigned char *lens, int n) {
    for (int i = 0; i < 16; i++) h->count[i] = 0;
    for (int i = 0; i < n; i++) h->count[lens[i]]++;
    h->count[0] = 0;
    unsigned short offs[16]; offs[1] = 0;
    for (int i = 1; i < 15; i++) offs[i + 1] = offs[i] + h->count[i];
    for (int i = 0; i < n; i++) if (lens[i]) h->sym[offs[lens[i]]++] = i;
}
static int huff_decode(struct bitr *r, struct huff *h) {
    int code = 0, first = 0, index = 0;
    for (int len = 1; len <= 15; len++) {
        int b = getbit(r); if (b < 0) return -1;
        code |= b;
        int count = h->count[len];
        if (code - first < count) return h->sym[index + (code - first)];
        index += count; first += count; first <<= 1; code <<= 1;
    }
    return -1;
}
static const unsigned short len_base[] = {3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258};
static const unsigned char  len_extra[] = {0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0};
static const unsigned short dist_base[] = {1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577};
static const unsigned char  dist_extra[] = {0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13};

static int inflate_block(struct bitr *r, struct huff *lh, struct huff *dh, struct buf *out) {
    for (;;) {
        int sym = huff_decode(r, lh);
        if (sym < 0) return -1;
        if (sym == 256) return 0;
        if (sym < 256) { bput(out, sym); continue; }
        sym -= 257; if (sym >= 29) return -1;
        int len = len_base[sym] + getbits(r, len_extra[sym]);
        int dsym = huff_decode(r, dh); if (dsym < 0 || dsym >= 30) return -1;
        int dist = dist_base[dsym] + getbits(r, dist_extra[dsym]);
        if ((size_t)dist > out->n) return -1;
        for (int i = 0; i < len; i++) { bput(out, out->d[out->n - dist]); }
    }
}
static int inflate(const unsigned char *data, size_t n, struct buf *out) {
    struct bitr r = { data, n, 0, 0, 0 };
    for (;;) {
        int final = getbit(&r); if (final < 0) return -1;
        int type = getbits(&r, 2); if (type < 0) return -1;
        if (type == 0) {                             /* stored */
            r.bitcnt = 0;
            if (r.pos + 4 > r.n) return -1;
            int len = r.d[r.pos] | (r.d[r.pos + 1] << 8); r.pos += 4;
            if (r.pos + len > r.n) return -1;
            bputmem(out, r.d + r.pos, len); r.pos += len;
        } else if (type == 1) {                      /* fixed Huffman */
            struct huff lh, dh;
            unsigned char ll[288], dl[30];
            for (int i = 0; i < 144; i++) ll[i] = 8;
            for (int i = 144; i < 256; i++) ll[i] = 9;
            for (int i = 256; i < 280; i++) ll[i] = 7;
            for (int i = 280; i < 288; i++) ll[i] = 8;
            for (int i = 0; i < 30; i++) dl[i] = 5;
            huff_build(&lh, ll, 288); huff_build(&dh, dl, 30);
            if (inflate_block(&r, &lh, &dh, out) < 0) return -1;
        } else if (type == 2) {                      /* dynamic Huffman */
            int hlit = getbits(&r, 5) + 257, hdist = getbits(&r, 5) + 1, hclen = getbits(&r, 4) + 4;
            static const int ord[19] = {16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15};
            unsigned char cl[19] = {0};
            for (int i = 0; i < hclen; i++) cl[ord[i]] = getbits(&r, 3);
            struct huff ch; huff_build(&ch, cl, 19);
            unsigned char lens[288 + 30] = {0}; int i = 0;
            while (i < hlit + hdist) {
                int s = huff_decode(&r, &ch); if (s < 0) return -1;
                if (s < 16) lens[i++] = s;
                else if (s == 16) { int rep = getbits(&r, 2) + 3; if (i == 0) return -1; unsigned char prev = lens[i-1]; while (rep-- && i < hlit+hdist) lens[i++] = prev; }
                else if (s == 17) { int rep = getbits(&r, 3) + 3;  while (rep-- && i < hlit+hdist) lens[i++] = 0; }
                else { int rep = getbits(&r, 7) + 11; while (rep-- && i < hlit+hdist) lens[i++] = 0; }
            }
            struct huff lh, dh;
            huff_build(&lh, lens, hlit); huff_build(&dh, lens + hlit, hdist);
            if (inflate_block(&r, &lh, &dh, out) < 0) return -1;
        } else return -1;
        if (final) return 0;
    }
}

/* ==== DEFLATE (fixed Huffman + LZ77) ===================================== */
struct bitw { struct buf *b; unsigned bitbuf, bitcnt; };
static void putbit(struct bitw *w, int bit) {
    w->bitbuf |= (bit & 1) << w->bitcnt;
    if (++w->bitcnt == 8) { bput(w->b, w->bitbuf); w->bitbuf = 0; w->bitcnt = 0; }
}
static void putbits(struct bitw *w, int val, int n) { for (int i = 0; i < n; i++) putbit(w, (val >> i) & 1); }
static void putbits_rev(struct bitw *w, int val, int n) { for (int i = n - 1; i >= 0; i--) putbit(w, (val >> i) & 1); }
static void flush_bits(struct bitw *w) { if (w->bitcnt) { bput(w->b, w->bitbuf); w->bitbuf = 0; w->bitcnt = 0; } }

/* fixed-Huffman literal/length code (RFC 1951 3.2.6) */
static void emit_lit(struct bitw *w, int lit) {
    if (lit < 144) putbits_rev(w, 0x30 + lit, 8);
    else putbits_rev(w, 0x190 + (lit - 144), 9);
}
static int len_sym(int len, int *extra, int *nextra) {
    *extra = 0; *nextra = 0;
    for (int s = 28; s >= 0; s--) if (len >= len_base[s]) { *extra = len - len_base[s]; *nextra = len_extra[s]; return 257 + s; }
    return 257;
}
static int dist_sym(int dist, int *extra, int *nextra) {
    *extra = 0; *nextra = 0;
    for (int s = 29; s >= 0; s--) if (dist >= dist_base[s]) { *extra = dist - dist_base[s]; *nextra = dist_extra[s]; return s; }
    return 0;
}
static void emit_len(struct bitw *w, int len) {
    int extra, nextra, s = len_sym(len, &extra, &nextra);
    if (s <= 279) putbits_rev(w, s - 256, 7);        /* 256..279 -> 7-bit */
    else putbits_rev(w, 0xc0 + (s - 280), 8);        /* 280..287 -> 8-bit */
    putbits(w, extra, nextra);
}
static void emit_dist(struct bitw *w, int dist) {
    int extra, nextra, s = dist_sym(dist, &extra, &nextra);
    putbits_rev(w, s, 5);
    putbits(w, extra, nextra);
}

#define HASHBITS 15
#define HASHSIZE (1 << HASHBITS)
#define WSIZE 32768
static void deflate(const unsigned char *in, size_t n, struct buf *out) {
    struct bitw w = { out, 0, 0 };
    putbit(&w, 1);                                   /* BFINAL */
    putbits(&w, 1, 2);                               /* BTYPE = 01 fixed */
    int *head = malloc(HASHSIZE * sizeof(int));
    int *prev = malloc(n ? n * sizeof(int) : sizeof(int));
    for (int i = 0; i < HASHSIZE; i++) head[i] = -1;
    size_t i = 0;
    while (i < n) {
        int best_len = 0, best_dist = 0;
        if (i + 3 <= n) {
            unsigned h = ((in[i] << 10) ^ (in[i+1] << 5) ^ in[i+2]) & (HASHSIZE - 1);
            int j = head[h], chain = 0;
            while (j >= 0 && chain++ < 128) {
                if ((size_t)(i - j) <= WSIZE) {
                    size_t l = 0;
                    while (l < 258 && i + l < n && in[j + l] == in[i + l]) l++;
                    if ((int)l > best_len) { best_len = l; best_dist = i - j; if ((int)l >= 258) break; }
                }
                j = prev[j];
            }
            prev[i] = head[h]; head[h] = i;
        }
        if (best_len >= 3) {
            emit_len(&w, best_len); emit_dist(&w, best_dist);
            for (int k = 1; k < best_len; k++) {     /* insert the covered positions into the hash */
                size_t p = i + k;
                if (p + 3 <= n) { unsigned h = ((in[p] << 10) ^ (in[p+1] << 5) ^ in[p+2]) & (HASHSIZE - 1); prev[p] = head[h]; head[h] = p; }
            }
            i += best_len;
        } else { emit_lit(&w, in[i]); i++; }
    }
    putbits_rev(&w, 0, 7);                            /* end-of-block symbol 256 = 7 zero bits */
    flush_bits(&w);
    free(head); free(prev);
}

/* ==== gzip container ===================================================== */
static int gz_compress(const unsigned char *in, size_t n, struct buf *out) {
    unsigned char hdr[10] = { 0x1f, 0x8b, 8, 0, 0,0,0,0, 0, 3 };
    bputmem(out, hdr, 10);
    deflate(in, n, out);
    unsigned crc = crc32(0, in, n);
    unsigned char tail[8] = { crc, crc>>8, crc>>16, crc>>24, n, n>>8, n>>16, n>>24 };
    bputmem(out, tail, 8);
    return 0;
}
static int gz_decompress(const unsigned char *in, size_t n, struct buf *out) {
    if (n < 18 || in[0] != 0x1f || in[1] != 0x8b || in[2] != 8) { fprintf(stderr, "gzip: not in gzip format\n"); return -1; }
    int flg = in[3]; size_t p = 10;
    if (flg & 4) { if (p + 2 > n) return -1; int xlen = in[p] | (in[p+1] << 8); p += 2 + xlen; }  /* FEXTRA */
    if (flg & 8) { while (p < n && in[p]) p++; p++; }                                              /* FNAME */
    if (flg & 16) { while (p < n && in[p]) p++; p++; }                                             /* FCOMMENT */
    if (flg & 2) p += 2;                                                                           /* FHCRC */
    if (p >= n) return -1;
    if (inflate(in + p, n - p, out) < 0) { fprintf(stderr, "gzip: invalid compressed data\n"); return -1; }
    unsigned crc = crc32(0, out->d, out->n);
    unsigned want; memcpy(&want, in + n - 8, 4);
    if (crc != want) { fprintf(stderr, "gzip: crc error\n"); return -1; }
    return 0;
}

/* ---- file helpers ------------------------------------------------------- */
static unsigned char *slurp(FILE *f, size_t *outn) {
    struct buf b = {0}; unsigned char t[65536]; size_t r;
    while ((r = fread(t, 1, sizeof t, f)) > 0) bputmem(&b, t, r);
    *outn = b.n; return b.d;
}

int main(int argc, char **argv) {
    crc_init();
    const char *me = strrchr(argv[0], '/'); me = me ? me + 1 : argv[0];
    int decompress = !strcmp(me, "gunzip") || !strcmp(me, "zcat");
    int tostdout = !strcmp(me, "zcat"), keep = 0;
    const char *files[256]; int nf = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-d")) decompress = 1;
        else if (!strcmp(argv[i], "-c")) tostdout = 1;
        else if (!strcmp(argv[i], "-k")) keep = 1;
        else if (!strcmp(argv[i], "-f") || !strcmp(argv[i], "-n") || !strcmp(argv[i], "-q")) ;
        else if (argv[i][0] != '-') files[nf++] = argv[i];
    }

    if (nf == 0) {                                   /* stdin -> stdout */
        size_t n; unsigned char *in = slurp(stdin, &n);
        struct buf out = {0};
        if (decompress) { if (gz_decompress(in, n, &out) < 0) return 1; }
        else gz_compress(in, n, &out);
        fwrite(out.d, 1, out.n, stdout);
        free(in); free(out.d); return 0;
    }

    int ret = 0;
    for (int i = 0; i < nf; i++) {
        FILE *f = fopen(files[i], "rb");
        if (!f) { fprintf(stderr, "gzip: %s: no such file\n", files[i]); ret = 1; continue; }
        size_t n; unsigned char *in = slurp(f, &n); fclose(f);
        struct buf out = {0};
        if (decompress) { if (gz_decompress(in, n, &out) < 0) { free(in); ret = 1; continue; } }
        else gz_compress(in, n, &out);

        if (tostdout) { fwrite(out.d, 1, out.n, stdout); }
        else {
            char outname[1024];
            if (decompress) {
                size_t l = strlen(files[i]);
                if (l > 3 && !strcmp(files[i] + l - 3, ".gz")) { snprintf(outname, sizeof outname, "%.*s", (int)(l - 3), files[i]); }
                else { snprintf(outname, sizeof outname, "%s.out", files[i]); }
            } else snprintf(outname, sizeof outname, "%s.gz", files[i]);
            FILE *of = fopen(outname, "wb");
            if (!of) { fprintf(stderr, "gzip: cannot write %s\n", outname); free(in); free(out.d); ret = 1; continue; }
            fwrite(out.d, 1, out.n, of); fclose(of);
            if (!keep) unlink(files[i]);
        }
        free(in); free(out.d);
    }
    return ret;
}
