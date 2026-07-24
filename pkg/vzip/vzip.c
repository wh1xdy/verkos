/* vzip — VerkOS' own zip/unzip.
 *
 * Reads and writes PKZIP archives, reusing the same DEFLATE codec as vgzip
 * (method 8) plus stored (method 0), with CRC-32. Multi-call: zip and unzip.
 * Interoperates with standard zip/unzip.
 *
 *   zip [ARCHIVE.zip] FILE...     create/append (deflate)
 *   unzip [-l] [-p] ARCHIVE.zip   extract (-l list, -p to stdout)
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

/* ===== DEFLATE core (shared design with vgzip) =========================== */
static unsigned crc_tab[256];
static void crc_init(void) { for (unsigned i = 0; i < 256; i++) { unsigned c = i; for (int k = 0; k < 8; k++) c = (c & 1) ? 0xedb88320u ^ (c >> 1) : c >> 1; crc_tab[i] = c; } }
static unsigned crc32(unsigned crc, const unsigned char *p, size_t n) { crc = ~crc; while (n--) crc = crc_tab[(crc ^ *p++) & 0xff] ^ (crc >> 8); return ~crc; }

struct buf { unsigned char *d; size_t n, cap; };
static void bput(struct buf *b, int c) { if (b->n >= b->cap) { b->cap = b->cap ? b->cap * 2 : 65536; b->d = realloc(b->d, b->cap); } b->d[b->n++] = (unsigned char)c; }
static void bputmem(struct buf *b, const void *p, size_t n) { while (b->n + n > b->cap) { b->cap = b->cap ? b->cap * 2 : 65536; b->d = realloc(b->d, b->cap); } memcpy(b->d + b->n, p, n); b->n += n; }

struct bitr { const unsigned char *d; size_t n, pos; unsigned bitbuf, bitcnt; };
static int getbit(struct bitr *r) { if (r->bitcnt == 0) { if (r->pos >= r->n) return -1; r->bitbuf = r->d[r->pos++]; r->bitcnt = 8; } int b = r->bitbuf & 1; r->bitbuf >>= 1; r->bitcnt--; return b; }
static int getbits(struct bitr *r, int n) { int v = 0; for (int i = 0; i < n; i++) { int b = getbit(r); if (b < 0) return -1; v |= b << i; } return v; }
struct huff { unsigned short count[16]; unsigned short sym[288]; };
static void huff_build(struct huff *h, const unsigned char *lens, int n) { for (int i = 0; i < 16; i++) h->count[i] = 0; for (int i = 0; i < n; i++) h->count[lens[i]]++; h->count[0] = 0; unsigned short offs[16]; offs[1] = 0; for (int i = 1; i < 15; i++) offs[i + 1] = offs[i] + h->count[i]; for (int i = 0; i < n; i++) if (lens[i]) h->sym[offs[lens[i]]++] = i; }
static int huff_decode(struct bitr *r, struct huff *h) { int code = 0, first = 0, index = 0; for (int len = 1; len <= 15; len++) { int b = getbit(r); if (b < 0) return -1; code |= b; int count = h->count[len]; if (code - first < count) return h->sym[index + (code - first)]; index += count; first += count; first <<= 1; code <<= 1; } return -1; }
static const unsigned short len_base[] = {3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258};
static const unsigned char  len_extra[] = {0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0};
static const unsigned short dist_base[] = {1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577};
static const unsigned char  dist_extra[] = {0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13};
static int inflate_block(struct bitr *r, struct huff *lh, struct huff *dh, struct buf *out) {
    for (;;) { int sym = huff_decode(r, lh); if (sym < 0) return -1; if (sym == 256) return 0; if (sym < 256) { bput(out, sym); continue; }
        sym -= 257; if (sym >= 29) return -1; int len = len_base[sym] + getbits(r, len_extra[sym]);
        int ds = huff_decode(r, dh); if (ds < 0 || ds >= 30) return -1; int dist = dist_base[ds] + getbits(r, dist_extra[ds]);
        if ((size_t)dist > out->n) return -1;
        for (int i = 0; i < len; i++) bput(out, out->d[out->n - dist]); } }
static int inflate(const unsigned char *data, size_t n, struct buf *out) {
    struct bitr r = { data, n, 0, 0, 0 };
    for (;;) { int final = getbit(&r); if (final < 0) return -1; int type = getbits(&r, 2); if (type < 0) return -1;
        if (type == 0) { r.bitcnt = 0; if (r.pos + 4 > r.n) return -1; int len = r.d[r.pos] | (r.d[r.pos+1] << 8); r.pos += 4; if (r.pos + len > r.n) return -1; bputmem(out, r.d + r.pos, len); r.pos += len; }
        else if (type == 1) { struct huff lh, dh; unsigned char ll[288], dl[30]; for (int i=0;i<144;i++) ll[i]=8; for (int i=144;i<256;i++) ll[i]=9; for (int i=256;i<280;i++) ll[i]=7; for (int i=280;i<288;i++) ll[i]=8; for (int i=0;i<30;i++) dl[i]=5; huff_build(&lh,ll,288); huff_build(&dh,dl,30); if (inflate_block(&r,&lh,&dh,out)<0) return -1; }
        else if (type == 2) { int hlit=getbits(&r,5)+257, hdist=getbits(&r,5)+1, hclen=getbits(&r,4)+4; static const int ord[19]={16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15}; unsigned char cl[19]={0}; for (int i=0;i<hclen;i++) cl[ord[i]]=getbits(&r,3); struct huff ch; huff_build(&ch,cl,19); unsigned char lens[318]={0}; int i=0;
            while (i<hlit+hdist) { int s=huff_decode(&r,&ch); if (s<0) return -1; if (s<16) lens[i++]=s; else if (s==16){int rep=getbits(&r,2)+3; if(i==0) return -1; unsigned char pv=lens[i-1]; while(rep--&&i<hlit+hdist) lens[i++]=pv;} else if (s==17){int rep=getbits(&r,3)+3; while(rep--&&i<hlit+hdist) lens[i++]=0;} else {int rep=getbits(&r,7)+11; while(rep--&&i<hlit+hdist) lens[i++]=0;} }
            struct huff lh, dh; huff_build(&lh,lens,hlit); huff_build(&dh,lens+hlit,hdist); if (inflate_block(&r,&lh,&dh,out)<0) return -1; }
        else return -1;
        if (final) return 0; } }

struct bitw { struct buf *b; unsigned bitbuf, bitcnt; };
static void putbit(struct bitw *w, int bit) { w->bitbuf |= (bit & 1) << w->bitcnt; if (++w->bitcnt == 8) { bput(w->b, w->bitbuf); w->bitbuf = 0; w->bitcnt = 0; } }
static void putbits(struct bitw *w, int val, int n) { for (int i = 0; i < n; i++) putbit(w, (val >> i) & 1); }
static void putbits_rev(struct bitw *w, int val, int n) { for (int i = n - 1; i >= 0; i--) putbit(w, (val >> i) & 1); }
static void flush_bits(struct bitw *w) { if (w->bitcnt) { bput(w->b, w->bitbuf); w->bitbuf = 0; w->bitcnt = 0; } }
static void emit_lit(struct bitw *w, int lit) { if (lit < 144) putbits_rev(w, 0x30 + lit, 8); else putbits_rev(w, 0x190 + (lit - 144), 9); }
static int len_sym(int len, int *e, int *ne) { *e=0;*ne=0; for (int s=28;s>=0;s--) if (len>=len_base[s]) { *e=len-len_base[s]; *ne=len_extra[s]; return 257+s; } return 257; }
static int dist_sym(int dist, int *e, int *ne) { *e=0;*ne=0; for (int s=29;s>=0;s--) if (dist>=dist_base[s]) { *e=dist-dist_base[s]; *ne=dist_extra[s]; return s; } return 0; }
static void emit_len(struct bitw *w, int len) { int e,ne,s=len_sym(len,&e,&ne); if (s<=279) putbits_rev(w,s-256,7); else putbits_rev(w,0xc0+(s-280),8); putbits(w,e,ne); }
static void emit_dist(struct bitw *w, int dist) { int e,ne,s=dist_sym(dist,&e,&ne); putbits_rev(w,s,5); putbits(w,e,ne); }
#define HS (1<<15)
static void deflate(const unsigned char *in, size_t n, struct buf *out) {
    struct bitw w = { out, 0, 0 }; putbit(&w,1); putbits(&w,1,2);
    int *head = malloc(HS*sizeof(int)); int *prev = malloc((n?n:1)*sizeof(int));
    for (int i=0;i<HS;i++) head[i]=-1;
    size_t i=0;
    while (i<n) { int bl=0,bd=0;
        if (i+3<=n) { unsigned h=((in[i]<<10)^(in[i+1]<<5)^in[i+2])&(HS-1); int j=head[h],ch=0;
            while (j>=0&&ch++<128) { if ((size_t)(i-j)<=32768) { size_t l=0; while (l<258&&i+l<n&&in[j+l]==in[i+l]) l++; if ((int)l>bl){bl=l;bd=i-j; if((int)l>=258)break;} } j=prev[j]; }
            prev[i]=head[h]; head[h]=i; }
        if (bl>=3) { emit_len(&w,bl); emit_dist(&w,bd); for (int k=1;k<bl;k++){size_t p=i+k; if(p+3<=n){unsigned h=((in[p]<<10)^(in[p+1]<<5)^in[p+2])&(HS-1); prev[p]=head[h]; head[h]=p;}} i+=bl; }
        else { emit_lit(&w,in[i]); i++; } }
    putbits_rev(&w,0,7); flush_bits(&w); free(head); free(prev);
}

/* ===== little-endian helpers ============================================= */
static unsigned rd16(const unsigned char *p) { return p[0] | (p[1] << 8); }
static unsigned rd32(const unsigned char *p) { return p[0] | (p[1]<<8) | (p[2]<<16) | ((unsigned)p[3]<<24); }
static void wr16(struct buf *b, unsigned v) { bput(b, v & 0xff); bput(b, (v >> 8) & 0xff); }
static void wr32(struct buf *b, unsigned v) { bput(b, v & 0xff); bput(b, (v>>8)&0xff); bput(b, (v>>16)&0xff); bput(b, (v>>24)&0xff); }

static unsigned char *slurp_fd(FILE *f, size_t *n) { struct buf b = {0}; unsigned char t[65536]; size_t r; while ((r=fread(t,1,sizeof t,f))>0) bputmem(&b,t,r); *n=b.n; return b.d; }

/* ===== unzip ============================================================= */
static int name_wanted(const char *name, char **filt, int nf) {
    if (nf == 0) return 1;
    for (int i = 0; i < nf; i++) if (!strcmp(name, filt[i])) return 1;
    return 0;
}
static int do_unzip(const char *arch, int list, int pipe_out, char **filt, int nfilt) {
    FILE *f = fopen(arch, "rb"); if (!f) { fprintf(stderr, "unzip: cannot open %s\n", arch); return 1; }
    size_t n; unsigned char *z = slurp_fd(f, &n); fclose(f);
    /* find EOCD */
    long eo = -1;
    for (long i = n - 22; i >= 0 && i > (long)n - 22 - 65536; i--) if (rd32(z + i) == 0x06054b50) { eo = i; break; }
    if (eo < 0) { fprintf(stderr, "unzip: %s: not a zip file\n", arch); free(z); return 1; }
    int total = rd16(z + eo + 10);
    unsigned cd = rd32(z + eo + 16);
    if (list) printf("  Length      Name\n---------  ----\n");
    for (int e = 0; e < total && cd + 46 <= n; e++) {
        unsigned char *c = z + cd;
        if (rd32(c) != 0x02014b50) break;
        unsigned method = rd16(c + 10), csize = rd32(c + 20), usize = rd32(c + 24);
        unsigned nlen = rd16(c + 28), elen = rd16(c + 30), clen = rd16(c + 32), lho = rd32(c + 42);
        char name[1024]; unsigned cn = nlen < 1023 ? nlen : 1023; memcpy(name, c + 46, cn); name[cn] = 0;
        cd += 46 + nlen + elen + clen;
        if (!name_wanted(name, filt, nfilt)) continue;
        if (list) { printf("%9u  %s\n", usize, name); continue; }
        /* local header at lho */
        unsigned char *lh = z + lho;
        if (rd32(lh) != 0x04034b50) continue;
        unsigned lnlen = rd16(lh + 26), lelen = rd16(lh + 28);
        unsigned char *data = lh + 30 + lnlen + lelen;
        struct buf out = {0};
        if (method == 0) bputmem(&out, data, csize);
        else if (method == 8) { if (inflate(data, csize, &out) < 0) { fprintf(stderr, "unzip: %s: inflate error\n", name); continue; } }
        else { fprintf(stderr, "unzip: %s: unsupported method %u\n", name, method); continue; }
        if (out.n != usize && usize) { /* tolerate */ }
        if (pipe_out) fwrite(out.d, 1, out.n, stdout);
        else if (name[strlen(name) - 1] == '/') { mkdir(name, 0755); }
        else {
            for (char *s = strchr(name, '/'); s; s = strchr(s + 1, '/')) { *s = 0; mkdir(name, 0755); *s = '/'; }
            FILE *of = fopen(name, "wb"); if (of) { fwrite(out.d, 1, out.n, of); fclose(of); printf("  inflating: %s\n", name); }
        }
        free(out.d);
    }
    free(z);
    return 0;
}

/* ===== zip =============================================================== */
struct cdent { char name[1024]; unsigned crc, csize, usize, offset, method, nlen; };
static int do_zip(const char *arch, char **files, int nf) {
    struct buf out = {0};
    struct cdent *cds = malloc(nf * sizeof *cds); int nc = 0;
    for (int i = 0; i < nf; i++) {
        FILE *f = fopen(files[i], "rb"); if (!f) { fprintf(stderr, "zip: cannot read %s\n", files[i]); continue; }
        size_t un; unsigned char *data = slurp_fd(f, &un); fclose(f);
        unsigned crc = crc32(0, data, un);
        struct buf comp = {0}; deflate(data, un, &comp);
        unsigned method = 8, csize = comp.n;
        if (comp.n >= un) { method = 0; free(comp.d); comp.d = NULL; csize = un; }   /* stored if no gain */
        struct cdent *c = &cds[nc]; snprintf(c->name, sizeof c->name, "%s", files[i]);
        c->crc = crc; c->csize = csize; c->usize = un; c->offset = out.n; c->method = method; c->nlen = strlen(files[i]);
        /* local header */
        wr32(&out, 0x04034b50); wr16(&out, 20); wr16(&out, 0); wr16(&out, method);
        wr16(&out, 0); wr16(&out, 0x21); wr32(&out, crc); wr32(&out, csize); wr32(&out, un);
        wr16(&out, c->nlen); wr16(&out, 0);
        bputmem(&out, files[i], c->nlen);
        if (method == 8) bputmem(&out, comp.d, comp.n); else bputmem(&out, data, un);
        free(comp.d); free(data); nc++;
        printf("  adding: %s\n", files[i]);
    }
    unsigned cd_off = out.n;
    for (int i = 0; i < nc; i++) {
        struct cdent *c = &cds[i];
        wr32(&out, 0x02014b50); wr16(&out, 20); wr16(&out, 20); wr16(&out, 0); wr16(&out, c->method);
        wr16(&out, 0); wr16(&out, 0x21); wr32(&out, c->crc); wr32(&out, c->csize); wr32(&out, c->usize);
        wr16(&out, c->nlen); wr16(&out, 0); wr16(&out, 0); wr16(&out, 0); wr16(&out, 0); wr32(&out, 0);
        wr32(&out, c->offset); bputmem(&out, c->name, c->nlen);
    }
    unsigned cd_size = out.n - cd_off;
    wr32(&out, 0x06054b50); wr16(&out, 0); wr16(&out, 0); wr16(&out, nc); wr16(&out, nc);
    wr32(&out, cd_size); wr32(&out, cd_off); wr16(&out, 0);
    FILE *af = fopen(arch, "wb"); if (!af) { fprintf(stderr, "zip: cannot write %s\n", arch); return 1; }
    fwrite(out.d, 1, out.n, af); fclose(af);
    free(out.d); free(cds);
    return 0;
}

int main(int argc, char **argv) {
    crc_init();
    const char *me = strrchr(argv[0], '/'); me = me ? me + 1 : argv[0];
    if (!strcmp(me, "unzip")) {
        int list = 0, pipe_out = 0; const char *arch = NULL; char *filt[512]; int nfilt = 0;
        for (int i = 1; i < argc; i++) {
            if (!strcmp(argv[i], "-l")) list = 1;
            else if (!strcmp(argv[i], "-p")) pipe_out = 1;
            else if (argv[i][0] == '-') ;                    /* ignore other options */
            else if (!arch) arch = argv[i];
            else filt[nfilt++] = argv[i];                    /* file filters */
        }
        if (!arch) { fprintf(stderr, "usage: unzip [-l|-p] ARCHIVE.zip [FILE...]\n"); return 1; }
        if (list) printf("Archive:  %s\n", arch);
        return do_unzip(arch, list, pipe_out, filt, nfilt);
    }
    /* zip */
    const char *arch = NULL; char *files[512]; int nf = 0;
    for (int i = 1; i < argc; i++) { if (argv[i][0] == '-') continue; if (!arch) arch = argv[i]; else files[nf++] = argv[i]; }
    if (!arch || nf == 0) { fprintf(stderr, "usage: zip ARCHIVE.zip FILE...\n"); return 1; }
    return do_zip(arch, files, nf);
}
