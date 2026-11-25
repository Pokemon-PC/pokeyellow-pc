"""SM83 (Game Boy CPU) decoder and C semantics.

decode(rom_bytes_at, addr) -> Insn
sem(insn, imm8, imm16) -> (lines, flow) with C statements using the macros of
pc/runtime/sm83_ops.h.

`flow` describes control transfer:
    None                       fall through
    ('jump', target, cond)     target: int (static) or C expr str; cond: C expr or None
    ('call', target, cond)
    ('ret', cond)
    ('reti',)
    ('jphl',)
    ('halt',) ('stop',) ('invalid',)
Cycle counts are T-cycles: (base, extra_when_taken).
"""

R8 = ["b", "c", "d", "e", "h", "l", "(hl)", "a"]
R16 = ["bc", "de", "hl", "sp"]
R16P = ["bc", "de", "hl", "af"]
CC = ["nz", "z", "nc", "c"]
CC_C = {"nz": "!zf", "z": "zf", "nc": "!cf", "c": "cf"}
ALU = ["ADD8", "ADC8", "SUB8", "SBC8", "AND8", "XOR8", "OR8", "CP8"]
ALU_NAME = ["add", "adc", "sub", "sbc", "and", "xor", "or", "cp"]
CBOPS = ["RLC", "RRC", "RL", "RR", "SLA", "SRA", "SWAP", "SRL"]
INVALID = {0xD3, 0xDB, 0xDD, 0xE3, 0xE4, 0xEB, 0xEC, 0xED, 0xF4, 0xFC, 0xFD}


def length(op):
    if op in INVALID:
        return 1
    if op == 0xCB or op == 0x10:
        return 2
    if op in (0x01, 0x11, 0x21, 0x31, 0x08, 0xC2, 0xC3, 0xC4, 0xCA, 0xCC, 0xCD,
              0xD2, 0xD4, 0xDA, 0xDC, 0xEA, 0xFA):
        return 3
    if (op & 0xC7) == 0x06 or op in (0x18, 0x20, 0x28, 0x30, 0x38, 0xE0, 0xF0, 0xE8, 0xF8) \
            or (op & 0xC7) == 0xC6:
        return 2
    return 1


class Insn:
    __slots__ = ("addr", "op", "size", "b1", "b2", "text")

    def __init__(self, addr, data):
        self.addr = addr
        self.op = data[0]
        self.size = length(self.op)
        self.b1 = data[1] if len(data) > 1 else 0
        self.b2 = data[2] if len(data) > 2 else 0
        self.text = mnemonic(self)

    @property
    def imm16(self):
        return self.b1 | (self.b2 << 8)

    @property
    def rel_target(self):
        off = self.b1 - 256 if self.b1 >= 128 else self.b1
        return (self.addr + 2 + off) & 0xFFFF


def mnemonic(i):
    op = i.op
    n, nn = f"${i.b1:02x}", f"${i.b1 | (i.b2 << 8):04x}"
    if op == 0xCB:
        x = i.b1
        r = R8[x & 7]
        if x < 0x40:
            return f"{CBOPS[x >> 3].lower()} {r}"
        return f"{['bit', 'res', 'set'][(x >> 6) - 1]} {(x >> 3) & 7}, {r}"
    if op in INVALID:
        return f"db ${op:02x}"
    if 0x40 <= op < 0x80:
        return "halt" if op == 0x76 else f"ld {R8[(op >> 3) & 7]}, {R8[op & 7]}"
    if 0x80 <= op < 0xC0:
        return f"{ALU_NAME[(op >> 3) & 7]} {R8[op & 7]}"
    t = {0x00: "nop", 0x07: "rlca", 0x0F: "rrca", 0x17: "rla", 0x1F: "rra", 0x27: "daa", 0x2F: "cpl",
         0x37: "scf", 0x3F: "ccf", 0x10: "stop", 0xC9: "ret", 0xD9: "reti", 0xE9: "jp hl", 0xF3: "di",
         0xFB: "ei", 0xF9: "ld sp, hl", 0x02: "ld [bc], a", 0x12: "ld [de], a", 0x22: "ld [hli], a",
         0x32: "ld [hld], a", 0x0A: "ld a, [bc]", 0x1A: "ld a, [de]", 0x2A: "ld a, [hli]",
         0x3A: "ld a, [hld]", 0xE2: "ldh [c], a", 0xF2: "ldh a, [c]"}
    if op in t:
        return t[op]
    if (op & 0xCF) == 0x01:
        return f"ld {R16[op >> 4]}, {nn}"
    if (op & 0xCF) == 0x03:
        return f"inc {R16[op >> 4]}"
    if (op & 0xCF) == 0x0B:
        return f"dec {R16[op >> 4]}"
    if (op & 0xCF) == 0x09:
        return f"add hl, {R16[op >> 4]}"
    if (op & 0xC7) == 0x04:
        return f"inc {R8[op >> 3]}"
    if (op & 0xC7) == 0x05:
        return f"dec {R8[op >> 3]}"
    if (op & 0xC7) == 0x06:
        return f"ld {R8[op >> 3]}, {n}"
    if op == 0x08:
        return f"ld [{nn}], sp"
    if op == 0x18:
        return f"jr ${i.rel_target:04x}"
    if op in (0x20, 0x28, 0x30, 0x38):
        return f"jr {CC[(op >> 3) & 3]}, ${i.rel_target:04x}"
    if (op & 0xE7) == 0xC0:
        return f"ret {CC[(op >> 3) & 3]}"
    if (op & 0xCF) == 0xC1:
        return f"pop {R16P[(op >> 4) & 3]}"
    if (op & 0xCF) == 0xC5:
        return f"push {R16P[(op >> 4) & 3]}"
    if (op & 0xE7) == 0xC2:
        return f"jp {CC[(op >> 3) & 3]}, {nn}"
    if op == 0xC3:
        return f"jp {nn}"
    if (op & 0xE7) == 0xC4:
        return f"call {CC[(op >> 3) & 3]}, {nn}"
    if op == 0xCD:
        return f"call {nn}"
    if (op & 0xC7) == 0xC6:
        return f"{ALU_NAME[(op >> 3) & 7]} {n}"
    if (op & 0xC7) == 0xC7:
        return f"rst ${op & 0x38:02x}"
    if op == 0xE0:
        return f"ldh [$ff{i.b1:02x}], a"
    if op == 0xF0:
        return f"ldh a, [$ff{i.b1:02x}]"
    if op == 0xE8:
        return f"add sp, {n}"
    if op == 0xF8:
        return f"ld hl, sp+{n}"
    if op == 0xEA:
        return f"ld [{nn}], a"
    if op == 0xFA:
        return f"ld a, [{nn}]"
    return f"?? {op:02x}"


def cycles(op, cb=0):
    """-> (base, extra_if_taken)"""
    if op == 0xCB:
        if (cb & 7) == 6:
            return (12, 0) if 0x40 <= cb < 0x80 else (16, 0)
        return (8, 0)
    if op in INVALID:
        return (4, 0)
    if 0x40 <= op < 0xC0:
        return (8, 0) if ((op & 7) == 6 or (0x70 <= op < 0x78 and op != 0x76)) else (4, 0)
    if op in (0x20, 0x28, 0x30, 0x38):
        return (8, 4)
    if (op & 0xE7) == 0xC0:
        return (8, 12)
    if (op & 0xE7) == 0xC2:
        return (12, 4)
    if (op & 0xE7) == 0xC4:
        return (12, 12)
    t = {0x00: 4, 0x08: 20, 0x10: 4, 0x18: 12, 0x34: 12, 0x35: 12, 0x36: 12,
         0xC3: 16, 0xC9: 16, 0xD9: 16, 0xCD: 24, 0xE0: 12, 0xF0: 12, 0xE2: 8, 0xF2: 8,
         0xE8: 16, 0xF8: 12, 0xF9: 8, 0xE9: 4, 0xEA: 16, 0xFA: 16}
    if op in t:
        return (t[op], 0)
    if (op & 0xCF) == 0x01:
        return (12, 0)
    if (op & 0xC7) in (0x02,):
        return (8, 0)
    if (op & 0xC7) == 0x03 or (op & 0xCF) == 0x09:
        return (8, 0)
    if (op & 0xC7) in (0x04, 0x05):
        return (4, 0)
    if (op & 0xC7) == 0x06:
        return (8, 0)
    if (op & 0xC7) == 0x0A:
        return (8, 0)
    if (op & 0xCF) == 0xC1:
        return (12, 0)
    if (op & 0xCF) == 0xC5:
        return (16, 0)
    if (op & 0xC7) == 0xC6:
        return (8, 0)
    if (op & 0xC7) == 0xC7:
        return (16, 0)
    return (4, 0)


def r8_get(i):
    return "rd(HL)" if R8[i] == "(hl)" else R8[i]


def sem(insn, imm8, imm16, next_pc, rel_target):
    """C semantics. imm8/imm16/next_pc/rel_target are C expressions (str) or
    ints. Returns (lines, flow)."""
    op = insn.op
    L = []
    flow = None
    if op == 0xCB:
        x = insn.b1 if isinstance(imm8, int) else None
        return None  # handled by sem_cb (operand known at decode time)
    if op in INVALID:
        return [], ("invalid",)
    if op == 0x76:
        return [], ("halt",)
    if 0x40 <= op < 0x80:
        dst, src = (op >> 3) & 7, op & 7
        if R8[dst] == "(hl)":
            L.append(f"wr(HL, {R8[src]});")
        else:
            L.append(f"{R8[dst]} = {r8_get(src)};")
        return L, None
    if 0x80 <= op < 0xC0:
        L.append(f"{ALU[(op >> 3) & 7]}({r8_get(op & 7)});")
        return L, None
    if (op & 0xC7) == 0xC6:
        L.append(f"{ALU[(op >> 3) & 7]}({imm8});")
        return L, None
    if (op & 0xCF) == 0x01:
        r = R16[op >> 4]
        L.append("sp = (uint16_t)(%s);" % imm16 if r == "sp" else f"SET_{r.upper()}({imm16});")
        return L, None
    if (op & 0xCF) in (0x03, 0x0B):
        r = R16[op >> 4]
        dlt = "+ 1" if (op & 0xF) == 3 else "- 1"
        L.append(f"sp = (uint16_t)(sp {dlt});" if r == "sp" else f"SET_{r.upper()}({r.upper()} {dlt});")
        return L, None
    if (op & 0xCF) == 0x09:
        r = R16[op >> 4]
        L.append(f"ADDHL({'sp' if r == 'sp' else r.upper()});")
        return L, None
    if (op & 0xC7) == 0x04:
        r = R8[op >> 3]
        L.append("INC_MHL();" if r == "(hl)" else f"INC8({r});")
        return L, None
    if (op & 0xC7) == 0x05:
        r = R8[op >> 3]
        L.append("DEC_MHL();" if r == "(hl)" else f"DEC8({r});")
        return L, None
    if (op & 0xC7) == 0x06:
        r = R8[op >> 3]
        L.append(f"wr(HL, {imm8});" if r == "(hl)" else f"{r} = {imm8};")
        return L, None
    simple = {
        0x00: [], 0x07: ["RLCA();"], 0x0F: ["RRCA();"], 0x17: ["RLA();"], 0x1F: ["RRA();"],
        0x27: ["DAA();"], 0x2F: ["CPL();"], 0x37: ["SCF();"], 0x3F: ["CCF();"],
        0x02: ["wr(BC, a);"], 0x12: ["wr(DE, a);"],
        0x22: ["wr(HL, a); SET_HL(HL + 1);"], 0x32: ["wr(HL, a); SET_HL(HL - 1);"],
        0x0A: ["a = rd(BC);"], 0x1A: ["a = rd(DE);"],
        0x2A: ["a = rd(HL); SET_HL(HL + 1);"], 0x3A: ["a = rd(HL); SET_HL(HL - 1);"],
        0xE2: ["wrh(c, a);"], 0xF2: ["a = rdh(c);"],
        0xF9: ["sp = HL;"], 0xF3: ["R.ime = 0;"], 0xFB: ["R.ime = 1;"],
        0xC1: ["POP_BC();"], 0xD1: ["POP_DE();"], 0xE1: ["POP_HL();"], 0xF1: ["POP_AF();"],
        0xC5: ["PUSH16(BC);"], 0xD5: ["PUSH16(DE);"], 0xE5: ["PUSH16(HL);"], 0xF5: ["PUSH16(AF);"],
    }
    if op in simple:
        return list(simple[op]), None
    if op == 0x08:
        return [f"wr16({imm16}, sp);"], None
    if op == 0xE0:
        return [f"wrh({imm8}, a);"], None
    if op == 0xF0:
        return [f"a = rdh({imm8});"], None
    if op == 0xE8:
        return [f"ADDSP({imm8});"], None
    if op == 0xF8:
        return [f"LDHLSP({imm8});"], None
    if op == 0xEA:
        return [f"wr({imm16}, a);"], None
    if op == 0xFA:
        return [f"a = rd({imm16});"], None
    if op == 0x10:
        return [], ("stop",)
    if op == 0x18:
        return [], ("jump", rel_target, None)
    if op in (0x20, 0x28, 0x30, 0x38):
        return [], ("jump", rel_target, CC_C[CC[(op >> 3) & 3]])
    if op == 0xC3:
        return [], ("jump", imm16, None)
    if (op & 0xE7) == 0xC2:
        return [], ("jump", imm16, CC_C[CC[(op >> 3) & 3]])
    if op == 0xCD:
        return [], ("call", imm16, None)
    if (op & 0xE7) == 0xC4:
        return [], ("call", imm16, CC_C[CC[(op >> 3) & 3]])
    if (op & 0xC7) == 0xC7:
        return [], ("call", op & 0x38, None)
    if op == 0xC9:
        return [], ("ret", None)
    if (op & 0xE7) == 0xC0:
        return [], ("ret", CC_C[CC[(op >> 3) & 3]])
    if op == 0xD9:
        return [], ("reti",)
    if op == 0xE9:
        return [], ("jphl",)
    raise ValueError(f"unhandled opcode {op:02x}")


def sem_cb(x):
    r = R8[x & 7]
    if x < 0x40:
        o = CBOPS[x >> 3]
        return [f"CB_MHL({o});" if r == "(hl)" else f"{o}({r});"]
    n = (x >> 3) & 7
    kind = (x >> 6)
    if kind == 1:
        return [f"BIT({n}, {'rd(HL)' if r == '(hl)' else r});"]
    o = "RES" if kind == 2 else "SET"
    return [f"CBN_MHL({o}, {n});" if r == "(hl)" else f"{o}({n}, {r});"]
