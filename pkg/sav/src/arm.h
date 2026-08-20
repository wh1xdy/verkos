/* aarch64 instruction encoding.
 *
 * savc writes machine code, not assembly: there is no assembler under it and
 * no linker after it. Every function here returns the 32 bits of one
 * instruction, so the code generator can be read as a list of instructions
 * and this file can be checked against the architecture manual on its own.
 */
#ifndef SAV_ARM_H
#define SAV_ARM_H
#include "sav.h"

enum { X0=0, X1=1, X2=2, X3=3, X4=4, X5=5, X6=6, X7=7, X8=8, X9=9, X10=10, X11=11,
       X16=16, X17=17, X29=29, X30=30, XZR=31, SP=31 };
enum { CC_EQ=0, CC_NE=1, CC_HS=2, CC_LO=3, CC_MI=4, CC_PL=5, CC_HI=8, CC_LS=9,
       CC_GE=10, CC_LT=11, CC_GT=12, CC_LE=13, CC_AL=14 };

u32 a_movz(int rd, u32 imm16, int hw);
u32 a_movk(int rd, u32 imm16, int hw);
u32 a_movn(int rd, u32 imm16, int hw);
u32 a_mov(int rd, int rm);
u32 a_mov_sp(int rd, int rn);           /* add rd, rn, #0 — the only way to name sp */
u32 a_add_imm(int rd, int rn, u32 imm12, int shift12);
u32 a_sub_imm(int rd, int rn, u32 imm12, int shift12);
u32 a_add(int rd, int rn, int rm);
u32 a_sub(int rd, int rn, int rm);
u32 a_subs(int rd, int rn, int rm);
u32 a_and(int rd, int rn, int rm);
u32 a_orr(int rd, int rn, int rm);
u32 a_eor(int rd, int rn, int rm);
u32 a_mul(int rd, int rn, int rm);
u32 a_sdiv(int rd, int rn, int rm);
u32 a_udiv(int rd, int rn, int rm);
u32 a_msub(int rd, int rn, int rm, int ra);
u32 a_lslv(int rd, int rn, int rm);
u32 a_lsrv(int rd, int rn, int rm);
u32 a_asrv(int rd, int rn, int rm);
u32 a_mvn(int rd, int rm);
u32 a_neg(int rd, int rm);
u32 a_cset(int rd, int cond);
u32 a_b(int off26);
u32 a_bl(int off26);
u32 a_bcond(int cond, int off19);
u32 a_cbz(int rt, int off19);
u32 a_cbnz(int rt, int off19);
u32 a_ret(void);
u32 a_svc0(void);
u32 a_adrp(int rd, int imm21);
/* loads and stores; size is 1, 2, 4 or 8 bytes, sign only matters on loads */
u32 a_str_off(int rt, int rn, i64 off, int size);
u32 a_ldr_off(int rt, int rn, i64 off, int size, int is_signed);
u32 a_push(int rt);                      /* str rt, [sp, #-16]! */
u32 a_pop(int rt);                       /* ldr rt, [sp], #16   */
u32 a_stp_pre(int rt, int rt2, int rn, int off);
u32 a_ldp_post(int rt, int rt2, int rn, int off);
u32 a_sxt(int rd, int rn, int bits);     /* sign-extend the low bits */
u32 a_uxt(int rd, int rn, int bits);
int a_offset_fits(i64 off, int size);    /* does a plain load/store reach it? */

#endif
