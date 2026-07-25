/* ed25519.h — SHA-512 and Ed25519 for VerkOS, freestanding.
 *
 * vboot has to decide whether to trust a kernel before there is an operating
 * system, so this cannot depend on libc or on any library: it is plain C with
 * fixed-size arithmetic and no allocation. Used by vboot to verify a kernel and
 * initramfs, and by the vsign host tool to produce those signatures.
 *
 * The field arithmetic follows the classic compact formulation: an element of
 * GF(2^255-19) is 16 limbs of ~16 bits held in 64-bit words, carried after each
 * operation. It is slower than a 51-bit-limb implementation and that is fine —
 * a boot verifies two signatures, once.
 *
 * Correctness is checked against the RFC 8032 test vectors (see ed25519-test.c);
 * do not change this file without re-running them.
 */
#ifndef VERK_ED25519_H
#define VERK_ED25519_H
#include <stdint.h>

typedef uint8_t  ed_u8;
typedef uint64_t ed_u64;
typedef int64_t  ed_i64;
typedef ed_i64   ed_gf[16];

/* ---------------------------------------------------------------- SHA-512 */
static const ed_u64 ED_K[80] = {
0x428a2f98d728ae22ULL,0x7137449123ef65cdULL,0xb5c0fbcfec4d3b2fULL,0xe9b5dba58189dbbcULL,
0x3956c25bf348b538ULL,0x59f111f1b605d019ULL,0x923f82a4af194f9bULL,0xab1c5ed5da6d8118ULL,
0xd807aa98a3030242ULL,0x12835b0145706fbeULL,0x243185be4ee4b28cULL,0x550c7dc3d5ffb4e2ULL,
0x72be5d74f27b896fULL,0x80deb1fe3b1696b1ULL,0x9bdc06a725c71235ULL,0xc19bf174cf692694ULL,
0xe49b69c19ef14ad2ULL,0xefbe4786384f25e3ULL,0x0fc19dc68b8cd5b5ULL,0x240ca1cc77ac9c65ULL,
0x2de92c6f592b0275ULL,0x4a7484aa6ea6e483ULL,0x5cb0a9dcbd41fbd4ULL,0x76f988da831153b5ULL,
0x983e5152ee66dfabULL,0xa831c66d2db43210ULL,0xb00327c898fb213fULL,0xbf597fc7beef0ee4ULL,
0xc6e00bf33da88fc2ULL,0xd5a79147930aa725ULL,0x06ca6351e003826fULL,0x142929670a0e6e70ULL,
0x27b70a8546d22ffcULL,0x2e1b21385c26c926ULL,0x4d2c6dfc5ac42aedULL,0x53380d139d95b3dfULL,
0x650a73548baf63deULL,0x766a0abb3c77b2a8ULL,0x81c2c92e47edaee6ULL,0x92722c851482353bULL,
0xa2bfe8a14cf10364ULL,0xa81a664bbc423001ULL,0xc24b8b70d0f89791ULL,0xc76c51a30654be30ULL,
0xd192e819d6ef5218ULL,0xd69906245565a910ULL,0xf40e35855771202aULL,0x106aa07032bbd1b8ULL,
0x19a4c116b8d2d0c8ULL,0x1e376c085141ab53ULL,0x2748774cdf8eeb99ULL,0x34b0bcb5e19b48a8ULL,
0x391c0cb3c5c95a63ULL,0x4ed8aa4ae3418acbULL,0x5b9cca4f7763e373ULL,0x682e6ff3d6b2b8a3ULL,
0x748f82ee5defb2fcULL,0x78a5636f43172f60ULL,0x84c87814a1f0ab72ULL,0x8cc702081a6439ecULL,
0x90befffa23631e28ULL,0xa4506cebde82bde9ULL,0xbef9a3f7b2c67915ULL,0xc67178f2e372532bULL,
0xca273eceea26619cULL,0xd186b8c721c0c207ULL,0xeada7dd6cde0eb1eULL,0xf57d4f7fee6ed178ULL,
0x06f067aa72176fbaULL,0x0a637dc5a2c898a6ULL,0x113f9804bef90daeULL,0x1b710b35131c471bULL,
0x28db77f523047d84ULL,0x32caab7b40c72493ULL,0x3c9ebe0a15c9bebcULL,0x431d67c49c100d4cULL,
0x4cc5d4becb3e42b6ULL,0x597f299cfc657e2aULL,0x5fcb6fab3ad6faecULL,0x6c44198c4a475817ULL};

#define ED_ROR(x,n) (((x) >> (n)) | ((x) << (64 - (n))))
static void ed_sha512_block(ed_u64 h[8], const ed_u8 *p) {
    ed_u64 w[80], a, b, c, d, e, f, g, hh, t1, t2;
    for (int i = 0; i < 16; i++) {
        w[i] = 0;
        for (int j = 0; j < 8; j++) w[i] = (w[i] << 8) | p[i * 8 + j];
    }
    for (int i = 16; i < 80; i++) {
        ed_u64 s0 = ED_ROR(w[i-15],1) ^ ED_ROR(w[i-15],8) ^ (w[i-15] >> 7);
        ed_u64 s1 = ED_ROR(w[i-2],19) ^ ED_ROR(w[i-2],61) ^ (w[i-2] >> 6);
        w[i] = w[i-16] + s0 + w[i-7] + s1;
    }
    a=h[0];b=h[1];c=h[2];d=h[3];e=h[4];f=h[5];g=h[6];hh=h[7];
    for (int i = 0; i < 80; i++) {
        ed_u64 S1 = ED_ROR(e,14) ^ ED_ROR(e,18) ^ ED_ROR(e,41);
        ed_u64 ch = (e & f) ^ (~e & g);
        t1 = hh + S1 + ch + ED_K[i] + w[i];
        ed_u64 S0 = ED_ROR(a,28) ^ ED_ROR(a,34) ^ ED_ROR(a,39);
        ed_u64 mj = (a & b) ^ (a & c) ^ (b & c);
        t2 = S0 + mj;
        hh=g; g=f; f=e; e=d+t1; d=c; c=b; b=a; a=t1+t2;
    }
    h[0]+=a;h[1]+=b;h[2]+=c;h[3]+=d;h[4]+=e;h[5]+=f;h[6]+=g;h[7]+=hh;
}
/* Streaming state, so a multi-megabyte kernel can be hashed without buffering. */
typedef struct { ed_u64 h[8]; ed_u8 buf[128]; ed_u64 len; unsigned n; } ed_sha512_ctx;
static void ed_sha512_init(ed_sha512_ctx *s) {
    static const ed_u64 iv[8] = {
        0x6a09e667f3bcc908ULL,0xbb67ae8584caa73bULL,0x3c6ef372fe94f82bULL,0xa54ff53a5f1d36f1ULL,
        0x510e527fade682d1ULL,0x9b05688c2b3e6c1fULL,0x1f83d9abfb41bd6bULL,0x5be0cd19137e2179ULL };
    for (int i = 0; i < 8; i++) s->h[i] = iv[i];
    s->len = 0; s->n = 0;
}
static void ed_sha512_update(ed_sha512_ctx *s, const ed_u8 *p, ed_u64 n) {
    s->len += n;
    while (n) {
        unsigned take = 128 - s->n;
        if (take > n) take = (unsigned)n;
        for (unsigned i = 0; i < take; i++) s->buf[s->n + i] = p[i];
        s->n += take; p += take; n -= take;
        if (s->n == 128) { ed_sha512_block(s->h, s->buf); s->n = 0; }
    }
}
static void ed_sha512_final(ed_sha512_ctx *s, ed_u8 out[64]) {
    ed_u64 bits = s->len * 8;
    s->buf[s->n++] = 0x80;
    if (s->n > 112) { while (s->n < 128) s->buf[s->n++] = 0; ed_sha512_block(s->h, s->buf); s->n = 0; }
    while (s->n < 120) s->buf[s->n++] = 0;          /* high half of the length is 0 */
    for (int i = 7; i >= 0; i--) s->buf[s->n++] = (ed_u8)(bits >> (i * 8));
    ed_sha512_block(s->h, s->buf);
    for (int i = 0; i < 8; i++)
        for (int j = 0; j < 8; j++) out[i * 8 + j] = (ed_u8)(s->h[i] >> (56 - j * 8));
}
static void ed_sha512(const ed_u8 *m, ed_u64 n, ed_u8 out[64]) {
    ed_sha512_ctx s; ed_sha512_init(&s); ed_sha512_update(&s, m, n); ed_sha512_final(&s, out);
}

/* ------------------------------------------------- GF(2^255-19) arithmetic */
static const ed_gf ED_gf0, ED_gf1 = {1},
    ED_D  = {0x78a3,0x1359,0x4dca,0x75eb,0xd8ab,0x4141,0x0a4d,0x0070,
             0xe898,0x7779,0x4079,0x8cc7,0xfe73,0x2b6f,0x6cee,0x5203},
    ED_D2 = {0xf159,0x26b2,0x9b94,0xebd6,0xb156,0x8283,0x149a,0x00e0,
             0xd130,0xeef3,0x80f2,0x198e,0xfce7,0x56df,0xd9dc,0x2406},
    ED_X  = {0xd51a,0x8f25,0x2d60,0xc956,0xa7b2,0x9525,0xc760,0x692c,
             0xdc5c,0xfdd6,0xe231,0xc0a4,0x53fe,0xcd6e,0x36d3,0x2169},
    ED_Y  = {0x6658,0x6666,0x6666,0x6666,0x6666,0x6666,0x6666,0x6666,
             0x6666,0x6666,0x6666,0x6666,0x6666,0x6666,0x6666,0x6666},
    ED_I  = {0xa0b0,0x4a0e,0x1b27,0xc4ee,0xe478,0xad2f,0x1806,0x2f43,
             0xd7a7,0x3dfb,0x0099,0x2b4d,0xdf0b,0x4fc1,0x2480,0x2b83};

static void ed_set25519(ed_gf r, const ed_gf a) { for (int i = 0; i < 16; i++) r[i] = a[i]; }
static void ed_car25519(ed_gf o) {
    for (int i = 0; i < 16; i++) {
        o[i] += (1LL << 16);
        ed_i64 c = o[i] >> 16;
        o[(i + 1) * (i < 15)] += c - 1 + 37 * (c - 1) * (i == 15);
        o[i] -= c << 16;
    }
}
/* constant-time conditional swap: the secret must not steer the branch */
static void ed_sel25519(ed_gf p, ed_gf q, ed_i64 b) {
    ed_i64 t, c = ~(b - 1);
    for (int i = 0; i < 16; i++) { t = c & (p[i] ^ q[i]); p[i] ^= t; q[i] ^= t; }
}
static void ed_pack25519(ed_u8 *o, const ed_gf n) {
    ed_gf m, t;
    ed_set25519(t, n);
    ed_car25519(t); ed_car25519(t); ed_car25519(t);
    for (int j = 0; j < 2; j++) {
        m[0] = t[0] - 0xffed;
        for (int i = 1; i < 15; i++) {
            m[i] = t[i] - 0xffff - ((m[i-1] >> 16) & 1);
            m[i-1] &= 0xffff;
        }
        m[15] = t[15] - 0x7fff - ((m[14] >> 16) & 1);
        ed_i64 b = (m[15] >> 16) & 1;
        m[14] &= 0xffff;
        ed_sel25519(t, m, 1 - b);
    }
    for (int i = 0; i < 16; i++) { o[2*i] = (ed_u8)(t[i] & 0xff); o[2*i+1] = (ed_u8)(t[i] >> 8); }
}
static int ed_neq25519(const ed_gf a, const ed_gf b) {
    ed_u8 c[32], d[32];
    ed_pack25519(c, a); ed_pack25519(d, b);
    int r = 0;
    for (int i = 0; i < 32; i++) r |= c[i] ^ d[i];
    return r != 0;
}
static ed_u8 ed_par25519(const ed_gf a) { ed_u8 d[32]; ed_pack25519(d, a); return d[0] & 1; }
static void ed_unpack25519(ed_gf o, const ed_u8 *n) {
    for (int i = 0; i < 16; i++) o[i] = n[2*i] + ((ed_i64)n[2*i+1] << 8);
    o[15] &= 0x7fff;
}
static void ed_A(ed_gf o, const ed_gf a, const ed_gf b) { for (int i = 0; i < 16; i++) o[i] = a[i] + b[i]; }
static void ed_Z(ed_gf o, const ed_gf a, const ed_gf b) { for (int i = 0; i < 16; i++) o[i] = a[i] - b[i]; }
static void ed_M(ed_gf o, const ed_gf a, const ed_gf b) {
    ed_i64 t[31];
    for (int i = 0; i < 31; i++) t[i] = 0;
    for (int i = 0; i < 16; i++) for (int j = 0; j < 16; j++) t[i+j] += a[i] * b[j];
    for (int i = 0; i < 15; i++) t[i] += 38 * t[i+16];   /* fold 2^256 = 38 mod p */
    for (int i = 0; i < 16; i++) o[i] = t[i];
    ed_car25519(o); ed_car25519(o);
}
static void ed_S(ed_gf o, const ed_gf a) { ed_M(o, a, a); }
static void ed_inv25519(ed_gf o, const ed_gf i) {
    ed_gf c; ed_set25519(c, i);
    for (int a = 253; a >= 0; a--) { ed_S(c, c); if (a != 2 && a != 4) ed_M(c, c, i); }
    ed_set25519(o, c);
}
static void ed_pow2523(ed_gf o, const ed_gf i) {
    ed_gf c; ed_set25519(c, i);
    for (int a = 250; a >= 0; a--) { ed_S(c, c); if (a != 1) ed_M(c, c, i); }
    ed_set25519(o, c);
}

/* ------------------------------------------------------- group operations */
static void ed_add(ed_gf p[4], ed_gf q[4]) {
    ed_gf a,b,c,d,t,e,f,g,h;
    ed_Z(a, p[1], p[0]); ed_Z(t, q[1], q[0]); ed_M(a, a, t);
    ed_A(b, p[0], p[1]); ed_A(t, q[0], q[1]); ed_M(b, b, t);
    ed_M(c, p[3], q[3]); ed_M(c, c, ED_D2);
    ed_M(d, p[2], q[2]); ed_A(d, d, d);
    ed_Z(e, b, a); ed_Z(f, d, c); ed_A(g, d, c); ed_A(h, b, a);
    ed_M(p[0], e, f); ed_M(p[1], h, g); ed_M(p[2], g, f); ed_M(p[3], e, h);
}
static void ed_cswap(ed_gf p[4], ed_gf q[4], ed_u8 b) {
    for (int i = 0; i < 4; i++) ed_sel25519(p[i], q[i], b);
}
static void ed_pack(ed_u8 *r, ed_gf p[4]) {
    ed_gf tx, ty, zi;
    ed_inv25519(zi, p[2]);
    ed_M(tx, p[0], zi); ed_M(ty, p[1], zi);
    ed_pack25519(r, ty);
    r[31] ^= ed_par25519(tx) << 7;
}
static void ed_scalarmult(ed_gf p[4], ed_gf q[4], const ed_u8 *s) {
    ed_set25519(p[0], ED_gf0); ed_set25519(p[1], ED_gf1);
    ed_set25519(p[2], ED_gf1); ed_set25519(p[3], ED_gf0);
    for (int i = 255; i >= 0; --i) {
        ed_u8 b = (s[i / 8] >> (i & 7)) & 1;
        ed_cswap(p, q, b);
        ed_add(q, p);
        ed_add(p, p);
        ed_cswap(p, q, b);
    }
}
static void ed_scalarbase(ed_gf p[4], const ed_u8 *s) {
    ed_gf q[4];
    ed_set25519(q[0], ED_X); ed_set25519(q[1], ED_Y);
    ed_set25519(q[2], ED_gf1); ed_M(q[3], ED_X, ED_Y);
    ed_scalarmult(p, q, s);
}

static const ed_u64 ED_L[32] = {
    0xed,0xd3,0xf5,0x5c,0x1a,0x63,0x12,0x58,0xd6,0x9c,0xf7,0xa2,0xde,0xf9,0xde,0x14,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0x10 };
static void ed_modL(ed_u8 *r, ed_i64 x[64]) {
    for (int i = 63; i >= 32; --i) {
        ed_i64 carry = 0;
        int j, k;
        for (j = i - 32, k = i - 12; j < k; ++j) {
            x[j] += carry - 16 * x[i] * (ed_i64)ED_L[j - (i - 32)];
            carry = (x[j] + 128) >> 8;
            x[j] -= carry << 8;
        }
        x[j] += carry;
        x[i] = 0;
    }
    ed_i64 carry = 0;
    for (int j = 0; j < 32; j++) {
        x[j] += carry - (x[31] >> 4) * (ed_i64)ED_L[j];
        carry = x[j] >> 8;
        x[j] &= 255;
    }
    for (int j = 0; j < 32; j++) x[j] -= carry * (ed_i64)ED_L[j];
    for (int i = 0; i < 32; i++) { x[i+1] += x[i] >> 8; r[i] = (ed_u8)(x[i] & 255); }
}
static void ed_reduce(ed_u8 *r) {
    ed_i64 x[64];
    for (int i = 0; i < 64; i++) x[i] = (ed_i64)r[i];
    for (int i = 0; i < 64; i++) r[i] = 0;
    ed_modL(r, x);
}

static int ed_unpackneg(ed_gf r[4], const ed_u8 p[32]) {
    ed_gf t, chk, num, den, den2, den4, den6;
    ed_set25519(r[2], ED_gf1);
    ed_unpack25519(r[1], p);
    ed_S(num, r[1]); ed_M(den, num, ED_D);
    ed_Z(num, num, r[2]); ed_A(den, r[2], den);
    ed_S(den2, den); ed_S(den4, den2); ed_M(den6, den4, den2);
    ed_M(t, den6, num); ed_M(t, t, den);
    ed_pow2523(t, t);
    ed_M(t, t, num); ed_M(t, t, den); ed_M(t, t, den);
    ed_M(r[0], t, den);
    ed_S(chk, r[0]); ed_M(chk, chk, den);
    if (ed_neq25519(chk, num)) ed_M(r[0], r[0], ED_I);
    ed_S(chk, r[0]); ed_M(chk, chk, den);
    if (ed_neq25519(chk, num)) return -1;              /* not on the curve */
    if (ed_par25519(r[0]) == (p[31] >> 7)) ed_Z(r[0], ED_gf0, r[0]);
    ed_M(r[3], r[0], r[1]);
    return 0;
}

/* Verify a 64-byte detached signature over msg with a 32-byte public key.
 * Returns 0 on success, -1 on any failure. */
static int ed25519_verify(const ed_u8 *sig, const ed_u8 *msg, ed_u64 len, const ed_u8 *pk) {
    ed_u8 h[64], t[32];
    ed_gf p[4], q[4];
    if (sig[63] & 224) return -1;                      /* S must be reduced */
    if (ed_unpackneg(q, pk)) return -1;

    ed_sha512_ctx s;
    ed_sha512_init(&s);
    ed_sha512_update(&s, sig, 32);                     /* R */
    ed_sha512_update(&s, pk, 32);                      /* A */
    ed_sha512_update(&s, msg, len);                    /* M */
    ed_sha512_final(&s, h);
    ed_reduce(h);

    ed_scalarmult(p, q, h);                            /* h*(-A) */
    ed_scalarbase(q, sig + 32);                        /* S*B */
    ed_add(p, q);
    ed_pack(t, p);
    int diff = 0;
    for (int i = 0; i < 32; i++) diff |= sig[i] ^ t[i];
    return diff ? -1 : 0;
}

/* Sign msg with a 64-byte secret key (seed||public), producing 64 bytes.
 * Only the host-side signing tool needs this; vboot itself only verifies. */
__attribute__((unused))
static void ed25519_pubkey(ed_u8 pk[32], const ed_u8 seed[32]) {
    ed_u8 d[64];
    ed_gf p[4];
    ed_sha512(seed, 32, d);
    d[0] &= 248; d[31] &= 127; d[31] |= 64;
    ed_scalarbase(p, d);
    ed_pack(pk, p);
}
__attribute__((unused))
static void ed25519_sign(ed_u8 sig[64], const ed_u8 *msg, ed_u64 len,
                         const ed_u8 seed[32], const ed_u8 pk[32]) {
    ed_u8 d[64], h[64], r[64];
    ed_gf p[4];
    ed_sha512(seed, 32, d);
    d[0] &= 248; d[31] &= 127; d[31] |= 64;

    ed_sha512_ctx s;
    ed_sha512_init(&s);
    ed_sha512_update(&s, d + 32, 32);
    ed_sha512_update(&s, msg, len);
    ed_sha512_final(&s, r);
    ed_reduce(r);
    ed_scalarbase(p, r);
    ed_pack(sig, p);                                   /* R */

    ed_sha512_init(&s);
    ed_sha512_update(&s, sig, 32);
    ed_sha512_update(&s, pk, 32);
    ed_sha512_update(&s, msg, len);
    ed_sha512_final(&s, h);
    ed_reduce(h);

    /* S = r + h*a mod L */
    ed_i64 x[64];
    for (int i = 0; i < 64; i++) x[i] = 0;
    for (int i = 0; i < 32; i++) x[i] = (ed_i64)r[i];
    for (int i = 0; i < 32; i++)
        for (int j = 0; j < 32; j++) x[i+j] += (ed_i64)h[i] * (ed_i64)d[j];
    ed_modL(sig + 32, x);
}

#endif /* VERK_ED25519_H */
