#include "arm.h"

static u32 r(int x) { return (u32)(x & 31); }

u32 a_movz(int rd, u32 imm16, int hw) { return 0xD2800000u | ((u32)hw << 21) | ((imm16 & 0xFFFF) << 5) | r(rd); }
u32 a_movk(int rd, u32 imm16, int hw) { return 0xF2800000u | ((u32)hw << 21) | ((imm16 & 0xFFFF) << 5) | r(rd); }
u32 a_movn(int rd, u32 imm16, int hw) { return 0x92800000u | ((u32)hw << 21) | ((imm16 & 0xFFFF) << 5) | r(rd); }
u32 a_mov(int rd, int rm) { return 0xAA0003E0u | (r(rm) << 16) | r(rd); }
u32 a_mov_sp(int rd, int rn) { return 0x91000000u | (r(rn) << 5) | r(rd); }
u32 a_add_imm(int rd, int rn, u32 imm12, int sh) { return 0x91000000u | ((u32)(sh?1:0) << 22) | ((imm12 & 0xFFF) << 10) | (r(rn) << 5) | r(rd); }
u32 a_sub_imm(int rd, int rn, u32 imm12, int sh) { return 0xD1000000u | ((u32)(sh?1:0) << 22) | ((imm12 & 0xFFF) << 10) | (r(rn) << 5) | r(rd); }
u32 a_add(int rd, int rn, int rm) { return 0x8B000000u | (r(rm) << 16) | (r(rn) << 5) | r(rd); }
u32 a_sub(int rd, int rn, int rm) { return 0xCB000000u | (r(rm) << 16) | (r(rn) << 5) | r(rd); }
u32 a_subs(int rd, int rn, int rm) { return 0xEB000000u | (r(rm) << 16) | (r(rn) << 5) | r(rd); }
u32 a_and(int rd, int rn, int rm) { return 0x8A000000u | (r(rm) << 16) | (r(rn) << 5) | r(rd); }
u32 a_orr(int rd, int rn, int rm) { return 0xAA000000u | (r(rm) << 16) | (r(rn) << 5) | r(rd); }
u32 a_eor(int rd, int rn, int rm) { return 0xCA000000u | (r(rm) << 16) | (r(rn) << 5) | r(rd); }
u32 a_mul(int rd, int rn, int rm) { return 0x9B007C00u | (r(rm) << 16) | (r(rn) << 5) | r(rd); }
u32 a_sdiv(int rd, int rn, int rm) { return 0x9AC00C00u | (r(rm) << 16) | (r(rn) << 5) | r(rd); }
u32 a_udiv(int rd, int rn, int rm) { return 0x9AC00800u | (r(rm) << 16) | (r(rn) << 5) | r(rd); }
u32 a_msub(int rd, int rn, int rm, int ra) { return 0x9B008000u | (r(rm) << 16) | (r(ra) << 10) | (r(rn) << 5) | r(rd); }
u32 a_lslv(int rd, int rn, int rm) { return 0x9AC02000u | (r(rm) << 16) | (r(rn) << 5) | r(rd); }
u32 a_lsrv(int rd, int rn, int rm) { return 0x9AC02400u | (r(rm) << 16) | (r(rn) << 5) | r(rd); }
u32 a_asrv(int rd, int rn, int rm) { return 0x9AC02800u | (r(rm) << 16) | (r(rn) << 5) | r(rd); }
u32 a_mvn(int rd, int rm) { return 0xAA2003E0u | (r(rm) << 16) | r(rd); }
u32 a_neg(int rd, int rm) { return 0xCB0003E0u | (r(rm) << 16) | r(rd); }
/* cset rd, cond is csinc rd, xzr, xzr, invert(cond) */
u32 a_cset(int rd, int cond) { return 0x9A9F07E0u | ((u32)(cond ^ 1) << 12) | r(rd); }
u32 a_b(int off26) { return 0x14000000u | ((u32)off26 & 0x03FFFFFFu); }
u32 a_bl(int off26) { return 0x94000000u | ((u32)off26 & 0x03FFFFFFu); }
u32 a_bcond(int cond, int off19) { return 0x54000000u | (((u32)off19 & 0x7FFFFu) << 5) | (u32)(cond & 15); }
u32 a_cbz(int rt, int off19) { return 0xB4000000u | (((u32)off19 & 0x7FFFFu) << 5) | r(rt); }
u32 a_cbnz(int rt, int off19) { return 0xB5000000u | (((u32)off19 & 0x7FFFFu) << 5) | r(rt); }
u32 a_ret(void) { return 0xD65F03C0u; }
u32 a_svc0(void) { return 0xD4000001u; }
u32 a_adrp(int rd, int imm21) {
    u32 lo = (u32)imm21 & 3, hi = ((u32)imm21 >> 2) & 0x7FFFFu;
    return 0x90000000u | (lo << 29) | (hi << 5) | r(rd);
}

int a_offset_fits(i64 off, int size) {
    if (off >= 0 && off % size == 0 && off / size < 4096) return 1;   /* scaled form */
    if (off >= -256 && off < 256) return 1;                            /* unscaled form */
    return 0;
}

/* One store, of exactly the width asked for. Nothing here merges two of them
 * into a wider one — which is rule 4 of the language, and the reason a device
 * register can be written from sav at all. */
u32 a_str_off(int rt, int rn, i64 off, int size) {
    u32 sz = size == 1 ? 0u : size == 2 ? 1u : size == 4 ? 2u : 3u;
    if (off >= 0 && off % size == 0 && off / size < 4096) {
        u32 imm12 = (u32)(off / size);
        return (sz << 30) | 0x39000000u | (imm12 << 10) | (r(rn) << 5) | r(rt);
    }
    return (sz << 30) | 0x38000000u | (((u32)off & 0x1FFu) << 12) | (r(rn) << 5) | r(rt);
}

u32 a_ldr_off(int rt, int rn, i64 off, int size, int is_signed) {
    u32 sz = size == 1 ? 0u : size == 2 ? 1u : size == 4 ? 2u : 3u;
    /* opc 01 is an unsigned load; 10 is a signed load into a 64-bit register.
     * There is no signed 64-bit load, because there is nothing to extend. */
    u32 opc = (is_signed && size < 8) ? 2u : 1u;
    if (off >= 0 && off % size == 0 && off / size < 4096) {
        u32 imm12 = (u32)(off / size);
        return (sz << 30) | 0x39000000u | (opc << 22) | (imm12 << 10) | (r(rn) << 5) | r(rt);
    }
    return (sz << 30) | 0x38000000u | (opc << 22) | (((u32)off & 0x1FFu) << 12) | (r(rn) << 5) | r(rt);
}

u32 a_push(int rt) { return 0xF8000C00u | (((u32)(-16) & 0x1FFu) << 12) | (31u << 5) | r(rt); }
u32 a_pop(int rt)  { return 0xF8400400u | ((16u & 0x1FFu) << 12) | (31u << 5) | r(rt); }

u32 a_stp_pre(int rt, int rt2, int rn, int off) {
    u32 imm7 = (u32)(off / 8) & 0x7Fu;
    return 0xA9800000u | (imm7 << 15) | (r(rt2) << 10) | (r(rn) << 5) | r(rt);
}
u32 a_ldp_post(int rt, int rt2, int rn, int off) {
    u32 imm7 = (u32)(off / 8) & 0x7Fu;
    return 0xA8C00000u | (imm7 << 15) | (r(rt2) << 10) | (r(rn) << 5) | r(rt);
}

u32 a_sxt(int rd, int rn, int bits) {
    u32 imms = bits == 8 ? 7u : bits == 16 ? 15u : 31u;
    return 0x93400000u | (imms << 10) | (r(rn) << 5) | r(rd);
}
u32 a_uxt(int rd, int rn, int bits) {
    if (bits == 32) return 0x2A0003E0u | (r(rn) << 16) | r(rd);   /* mov wd, wn */
    u32 imms = bits == 8 ? 7u : 15u;
    return 0xD3400000u | (imms << 10) | (r(rn) << 5) | r(rd);
}
