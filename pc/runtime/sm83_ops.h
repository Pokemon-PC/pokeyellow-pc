/* SM83 (Game Boy CPU) instruction semantics, shared by the recompiled code
 * and the fallback interpreter. Every macro works on local variables named
 *   a b c d e h l   (uint8_t registers)
 *   zf nf hf cf     (flags, 0 or 1)
 *   sp              (uint16_t)
 * so that the C compiler can keep the CPU state in host registers inside a
 * recompiled bank function. Memory goes through rd()/wr() (gb.h). */
#pragma once
#include "gb.h"

#define BC ((uint16_t)((b << 8) | c))
#define DE ((uint16_t)((d << 8) | e))
#define HL ((uint16_t)((h << 8) | l))
#define SET_BC(v) do { uint16_t _v = (uint16_t)(v); b = _v >> 8; c = (uint8_t)_v; } while (0)
#define SET_DE(v) do { uint16_t _v = (uint16_t)(v); d = _v >> 8; e = (uint8_t)_v; } while (0)
#define SET_HL(v) do { uint16_t _v = (uint16_t)(v); h = _v >> 8; l = (uint8_t)_v; } while (0)
#define GET_F() ((uint8_t)((zf << 7) | (nf << 6) | (hf << 5) | (cf << 4)))
#define SET_F(v) do { uint8_t _fv = (v); zf = (_fv >> 7) & 1; nf = (_fv >> 6) & 1; hf = (_fv >> 5) & 1; cf = (_fv >> 4) & 1; } while (0)
#define AF ((uint16_t)((a << 8) | GET_F()))

#define CPU_LOCALS \
    uint8_t a = R.a, b = R.b, c = R.c, d = R.d, e = R.e, h = R.h, l = R.l; \
    uint8_t zf = R.zf, nf = R.nf, hf = R.hf, cf = R.cf; \
    uint16_t sp = R.sp; \
    (void)a; (void)b; (void)c; (void)d; (void)e; (void)h; (void)l; \
    (void)zf; (void)nf; (void)hf; (void)cf; (void)sp
#define CPU_SAVE() do { R.a = a; R.b = b; R.c = c; R.d = d; R.e = e; R.h = h; R.l = l; \
    R.zf = zf; R.nf = nf; R.hf = hf; R.cf = cf; R.sp = sp; } while (0)

#define CYC(n) (g_cyc += (n))

/* ---- 8-bit ALU ---- */
#define ADD8(v) do { uint8_t _v = (v); unsigned _r = a + _v; \
    hf = ((a & 0xF) + (_v & 0xF)) > 0xF; cf = _r > 0xFF; a = (uint8_t)_r; zf = a == 0; nf = 0; } while (0)
#define ADC8(v) do { uint8_t _v = (v); unsigned _r = a + _v + cf; \
    hf = ((a & 0xF) + (_v & 0xF) + cf) > 0xF; cf = _r > 0xFF; a = (uint8_t)_r; zf = a == 0; nf = 0; } while (0)
#define SUB8(v) do { uint8_t _v = (v); \
    hf = (a & 0xF) < (_v & 0xF); cf = a < _v; a = (uint8_t)(a - _v); zf = a == 0; nf = 1; } while (0)
#define SBC8(v) do { uint8_t _v = (v); int _r = a - _v - cf; \
    hf = ((a & 0xF) - (_v & 0xF) - cf) < 0; cf = _r < 0; a = (uint8_t)_r; zf = a == 0; nf = 1; } while (0)
#define AND8(v) do { a &= (v); zf = a == 0; nf = 0; hf = 1; cf = 0; } while (0)
#define XOR8(v) do { a ^= (v); zf = a == 0; nf = 0; hf = 0; cf = 0; } while (0)
#define OR8(v)  do { a |= (v); zf = a == 0; nf = 0; hf = 0; cf = 0; } while (0)
#define CP8(v)  do { uint8_t _v = (v); \
    hf = (a & 0xF) < (_v & 0xF); cf = a < _v; zf = a == _v; nf = 1; } while (0)
#define INC8(r) do { r = (uint8_t)(r + 1); zf = r == 0; nf = 0; hf = (r & 0xF) == 0; } while (0)
#define DEC8(r) do { r = (uint8_t)(r - 1); zf = r == 0; nf = 1; hf = (r & 0xF) == 0xF; } while (0)
#define INC_MHL() do { uint16_t _a = HL; uint8_t _t = rd(_a); INC8(_t); wr(_a, _t); } while (0)
#define DEC_MHL() do { uint16_t _a = HL; uint8_t _t = rd(_a); DEC8(_t); wr(_a, _t); } while (0)

/* ---- 16-bit ---- */
#define ADDHL(v) do { uint16_t _hl = HL, _v = (uint16_t)(v); unsigned _r = _hl + _v; \
    hf = ((_hl & 0xFFF) + (_v & 0xFFF)) > 0xFFF; cf = _r > 0xFFFF; nf = 0; SET_HL(_r); } while (0)
/* sp + e8 (add sp,e8 / ld hl,sp+e8): flags from the low byte, unsigned */
#define SPE8(dst, e8) do { uint8_t _u = (uint8_t)(e8); uint16_t _r = (uint16_t)(sp + (int8_t)_u); \
    hf = ((sp & 0xF) + (_u & 0xF)) > 0xF; cf = ((sp & 0xFF) + _u) > 0xFF; zf = 0; nf = 0; dst = _r; } while (0)
#define ADDSP(e8) SPE8(sp, e8)
#define LDHLSP(e8) do { uint16_t _t; SPE8(_t, e8); SET_HL(_t); } while (0)

/* ---- stack ---- */
#define PUSH16(v) do { uint16_t _v = (uint16_t)(v); sp--; wr(sp, _v >> 8); sp--; wr(sp, (uint8_t)_v); } while (0)
#define POP16(dst) do { uint8_t _lo = rd(sp); sp++; uint8_t _hi = rd(sp); sp++; dst = (uint16_t)((_hi << 8) | _lo); } while (0)
#define POP_BC() do { c = rd(sp); sp++; b = rd(sp); sp++; } while (0)
#define POP_DE() do { e = rd(sp); sp++; d = rd(sp); sp++; } while (0)
#define POP_HL() do { l = rd(sp); sp++; h = rd(sp); sp++; } while (0)
#define POP_AF() do { uint8_t _pf = rd(sp); sp++; a = rd(sp); sp++; SET_F(_pf); } while (0)

/* ---- rotates on A (Z always cleared) ---- */
#define RLCA() do { cf = a >> 7; a = (uint8_t)((a << 1) | cf); zf = nf = hf = 0; } while (0)
#define RRCA() do { cf = a & 1; a = (uint8_t)((a >> 1) | (cf << 7)); zf = nf = hf = 0; } while (0)
#define RLA()  do { uint8_t _c = cf; cf = a >> 7; a = (uint8_t)((a << 1) | _c); zf = nf = hf = 0; } while (0)
#define RRA()  do { uint8_t _c = cf; cf = a & 1; a = (uint8_t)((a >> 1) | (_c << 7)); zf = nf = hf = 0; } while (0)

/* ---- CB prefix ---- */
#define RLC(r)  do { cf = r >> 7; r = (uint8_t)((r << 1) | cf); zf = r == 0; nf = hf = 0; } while (0)
#define RRC(r)  do { cf = r & 1; r = (uint8_t)((r >> 1) | (cf << 7)); zf = r == 0; nf = hf = 0; } while (0)
#define RL(r)   do { uint8_t _c = cf; cf = r >> 7; r = (uint8_t)((r << 1) | _c); zf = r == 0; nf = hf = 0; } while (0)
#define RR(r)   do { uint8_t _c = cf; cf = r & 1; r = (uint8_t)((r >> 1) | (_c << 7)); zf = r == 0; nf = hf = 0; } while (0)
#define SLA(r)  do { cf = r >> 7; r = (uint8_t)(r << 1); zf = r == 0; nf = hf = 0; } while (0)
#define SRA(r)  do { cf = r & 1; r = (uint8_t)((r >> 1) | (r & 0x80)); zf = r == 0; nf = hf = 0; } while (0)
#define SRL(r)  do { cf = r & 1; r = (uint8_t)(r >> 1); zf = r == 0; nf = hf = 0; } while (0)
#define SWAP(r) do { r = (uint8_t)((r << 4) | (r >> 4)); zf = r == 0; nf = hf = cf = 0; } while (0)
#define BIT(n, r) do { zf = !(((r) >> (n)) & 1); nf = 0; hf = 1; } while (0)
#define RES(n, r) do { r &= (uint8_t)~(1u << (n)); } while (0)
#define SET(n, r) do { r |= (uint8_t)(1u << (n)); } while (0)
/* CB op on (hl): read-modify-write */
#define CB_MHL(OP) do { uint16_t _a = HL; uint8_t _t = rd(_a); OP(_t); wr(_a, _t); } while (0)
#define CBN_MHL(OP, n) do { uint16_t _a = HL; uint8_t _t = rd(_a); OP(n, _t); wr(_a, _t); } while (0)

/* ---- misc ---- */
#define CPL() do { a = (uint8_t)~a; nf = 1; hf = 1; } while (0)
#define SCF() do { cf = 1; nf = 0; hf = 0; } while (0)
#define CCF() do { cf ^= 1; nf = 0; hf = 0; } while (0)
#define DAA() do { unsigned _a = a; \
    if (!nf) { if (cf || _a > 0x99) { _a += 0x60; cf = 1; } if (hf || (_a & 0xF) > 9) _a += 6; } \
    else { if (cf) _a -= 0x60; if (hf) _a -= 6; } \
    a = (uint8_t)_a; zf = a == 0; hf = 0; } while (0)

/* high page (ldh): HRAM fast path, I/O otherwise */
static inline uint8_t rdh(uint8_t n) { return (n >= 0x80 && n != 0xFF) ? g_hram[n] : io_rd(n); }
static inline void wrh(uint8_t n, uint8_t v) { if (n >= 0x80 && n != 0xFF) g_hram[n] = v; else io_wr(n, v); }
