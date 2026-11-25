#!/usr/bin/env python3
"""Static recompiler: Game Boy ROM (built by rgbds from a pret disassembly) -> C.

    python pc/tools/recomp/recomp.py --rom pokecrystal.gbc --sym pokecrystal.sym \
        --src . --out build/rc [--extra pc/roots_crystal.txt] [--name crystal]

The ROM bytes are the ground truth (the disassembly builds a byte-identical
ROM). The .sym file gives the address of every label; the asm sources tell
which labels are code (the first statement after the label is an
instruction or a code macro). Code is found by recursive descent from those
labels, the interrupt/rst vectors and the extra roots (addresses the runtime
reported through its fallback interpreter).

Output: one C function per ROM bank, `uint16_t rc_bank_NN(uint16_t pc)`:
the CPU state lives in locals, every block start is a `case` of an entry
switch, branches inside the bank are `goto`s, everything else (calls/returns
across banks, `jp hl`, interrupts due) returns the next pc to the runtime's
dispatch loop. Calls push the real return address on the emulated stack, so
stack tricks of the game (push de / ret, popping return addresses, ...)
behave as on hardware. Also writes interp_gen.c: an interpreter generated
from the same semantics, used for code in RAM and addresses not seen here.
"""
import argparse
import io
import os
import re
import sys
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import sm83  # noqa: E402

MNEMONICS = {
    "ld", "ldh", "ldi", "ldd", "call", "jp", "jr", "ret", "reti", "rst", "push", "pop", "inc", "dec",
    "add", "adc", "sub", "sbc", "and", "or", "xor", "cp", "cpl", "ccf", "scf", "daa", "nop", "halt",
    "stop", "di", "ei", "rlca", "rla", "rrca", "rra", "rlc", "rl", "rrc", "rr", "sla", "sra", "srl",
    "swap", "bit", "res", "set",
}
CODE_MACROS = {
    "farcall", "callfar", "homecall", "predef", "predef_jump", "lda_predef", "lb", "ln", "hlcoord",
    "bccoord", "decoord", "hlbgcoord", "bcbgcoord", "debgcoord", "ldcoord_a", "lda_coord", "jumptable",
    "farjp", "callba", "callab", "ld_hli_a_string", "maskbits_a",
}
SKIP_DIRECTIVES = {"if", "elif", "else", "endc", "assert", "static_assert", "endsection", "pushs", "pops",
                   "load", "endl", "opt", "popo", "pusho", "warn", "def", "redef", "export", "align", "for",
                   "endr", "rept", "purge"}

# ---------------------------------------------------------------- inputs


def parse_sym(path):
    out = []
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            line = line.split(";")[0].strip()
            m = re.match(r"([0-9A-Fa-f]+):([0-9A-Fa-f]+)\s+(\S+)", line)
            if m:
                out.append((int(m.group(1), 16), int(m.group(2), 16), m.group(3)))
    return out


LABEL_G = re.compile(r"^([A-Za-z_][\w#@]*(?:\.[\w#@]+)?)(::?)")
LABEL_L = re.compile(r"^\s*(\.[\w#@]+):{0,2}")


def strip_comment(line):
    out, q = [], False
    for ch in line:
        if ch == '"':
            q = not q
        if ch == ";" and not q:
            break
        out.append(ch)
    return "".join(out)


def code_macros(repo):
    """Names of the macros whose body starts with an instruction (or another
    such macro): a label followed by one of them is code."""
    first, seen = {}, set()
    for root, dirs, files in os.walk(repo):
        rel = os.path.relpath(root, repo).replace("\\", "/")
        if rel.split("/")[0] in ("pc", "tools", ".git", "build"):
            dirs[:] = []
            continue
        for fn in files:
            if not fn.endswith(".asm"):
                continue
            name = None
            with open(os.path.join(root, fn), encoding="utf-8", errors="replace") as f:
                for raw in f:
                    st = strip_comment(raw).strip()
                    if not st:
                        continue
                    m = re.match(r"MACRO\??\s+(\w+)", st, re.I)
                    if m:
                        name = m.group(1).lower()
                        continue
                    if st.lower().startswith("endm"):
                        name = None
                        continue
                    if name:
                        seen.add(name)
                    if name and name not in first:
                        # a label prefix (".VC_\1::", "\1Species::") is not a statement
                        body = re.sub(r"^[^\s:;]+::?\s*", "", st) if re.match(r"^[^\s:;\"]+::?", st) else st
                        tok = re.split(r"[\s,]+", body)[0].lower() if body else ""
                        if tok and tok not in SKIP_DIRECTIVES:
                            first[name] = tok
    # macros that emit nothing (vc_hook...): transparent for the classification
    TRANSPARENT.update(n for n in seen if n not in first)
    out = set(CODE_MACROS)
    changed = True
    while changed:
        changed = False
        for n, t in first.items():
            if n not in out and (t in MNEMONICS or t in out):
                out.add(n)
                changed = True
    return out


def classify_source(repo):
    """-> (code, data): sets of full label names (Parent.local for locals)."""
    code, data = set(), set()
    cmacros = code_macros(repo)
    for root, dirs, files in os.walk(repo):
        rel = os.path.relpath(root, repo).replace("\\", "/")
        if rel.split("/")[0] in ("pc", "tools", ".git", "build"):
            dirs[:] = []
            continue
        for fn in files:
            if not fn.endswith(".asm"):
                continue
            scope = None
            pending = []
            last_label, last_off = None, None
            load_state = 0
            load_scope = None
            in_macro = False
            with open(os.path.join(root, fn), encoding="utf-8", errors="replace") as f:
                for raw in f:
                    line = strip_comment(raw).rstrip()
                    if not line.strip():
                        continue
                    low = line.strip().lower()
                    if low.startswith("macro"):
                        in_macro = True
                        continue
                    if low.startswith("endm"):
                        in_macro = False
                        continue
                    if in_macro:
                        continue
                    # peel labels at the start of the line
                    while True:
                        m = LABEL_G.match(line)
                        if m and not line.strip().lower().startswith(("section", "def ", "if ", "else", "endc")):
                            name = m.group(1)
                            if "." in name:
                                pending.append(name)
                            else:
                                scope = name
                                pending.append(name)
                            line = line[m.end():]
                            continue
                        m = LABEL_L.match(line)
                        if m:
                            pending.append(f"{scope}{m.group(1)}" if scope else m.group(1))
                            line = line[m.end():]
                            continue
                        break
                    if pending and load_state == 2:
                        LOADS[-1][2] = pending[0]
                        load_state = 0
                    stmt = line.strip()
                    if not stmt:
                        continue
                    tok = re.split(r"[\s,]+", stmt)[0].lower()
                    # LOAD blocks: code stored in ROM, copied to and run from RAM
                    if tok == "load":
                        LOADS.append([pending[-1] if pending else last_label, None, None])
                        pending = []
                        load_state = 1
                        load_scope = scope
                        continue
                    if tok == "endl":
                        load_state = 2
                        scope = load_scope   # rgbds restores the label scope after ENDL
                        continue
                    if pending and load_state == 1 and LOADS[-1][1] is None:
                        LOADS[-1][1] = pending[0]
                    if pending and load_state == 2:
                        LOADS[-1][2] = pending[0]
                        load_state = 0
                    if tok in SKIP_DIRECTIVES or tok in TRANSPARENT:
                        continue
                    if tok == "section":
                        pending = []
                        continue
                    if pending:
                        target = code if (tok in MNEMONICS or tok in cmacros) else data
                        target.update(pending)
                        last_label, last_off = pending[-1], 0
                        pending = []
                    # code embedded in text (text_asm): label + size of the commands before it
                    if tok == "text_asm":
                        if last_label and last_off is not None:
                            TEXT_ASM.append((last_label, last_off + 1))
                        last_off = None
                    elif last_off is not None:
                        last_off = last_off + TEXT_SIZES[tok] if tok in TEXT_SIZES else None
    return code, data


# fixed-size text commands (macros/scripts/text.asm)
TEXT_SIZES = {"text_far": 4, "text_ram": 3, "text_bcd": 4, "text_move": 3, "text_box": 5, "text_low": 1,
              "text_promptbutton": 1, "text_scroll": 1, "text_start": 1, "text_decimal": 4, "text_pause": 1,
              "text_end": 1, "text_dots": 2, "text_waitbutton": 1, "text_linkpromptbutton": 1}
TEXT_ASM = []   # (label, offset) of code following text_asm
TRANSPARENT = set()   # macros that emit no bytes
LOADS = []      # [rom label, first label in RAM, label after the block] of LOAD blocks


# ---------------------------------------------------------------- discovery

RAM_BANK = 0x100   # pseudo-bank numbers of code copied to RAM


class Bank:
    def __init__(self, num, lo=None, hi=None, rom_off=None):
        self.num = num
        if lo is not None:
            self.lo, self.hi = lo, hi
        else:
            self.lo, self.hi = (0, 0x4000) if num == 0 else (0x4000, 0x8000)
        self.rom_off = rom_off
        self.insns = {}       # addr -> Insn
        self.leaders = set()
        self.roots = set()


class Recomp:
    def __init__(self, rom, syms):
        self.rom = rom
        self.nbanks = len(rom) // 0x4000
        self.banks = {}
        self.names = {}
        for bank, addr, name in syms:
            if addr < 0x8000:
                self.names.setdefault((bank if addr >= 0x4000 else 0, addr), name)
        self.byname = {name: (bank, addr) for bank, addr, name in syms if addr < 0x8000}
        self.work = []

    def bank(self, n):
        if n not in self.banks:
            self.banks[n] = Bank(n)
        return self.banks[n]

    def off(self, bank, addr):
        if bank >= RAM_BANK:
            b = self.banks[bank]
            return b.rom_off + (addr - b.lo)
        return addr if addr < 0x4000 else bank * 0x4000 + (addr - 0x4000)

    def add_ram_copy(self, rom_off, ram, length):
        n = RAM_BANK + len([k for k in self.banks if k >= RAM_BANK])
        self.banks[n] = Bank(n, ram, ram + length, rom_off)
        self.add_root(n, ram)
        return n

    def add_root(self, bank, addr, leader=True):
        if bank >= RAM_BANK and bank in self.banks:
            b = self.banks[bank]
            if b.lo <= addr < b.hi:
                b.leaders.add(addr)
                if addr not in b.insns:
                    self.work.append((bank, addr))
                return
            bank = 0
        if addr >= 0x8000:
            return
        if addr < 0x4000:
            bank = 0
        elif bank == 0 or bank >= self.nbanks:
            return
        b = self.bank(bank)
        if leader:
            b.leaders.add(addr)
        if addr not in b.insns:
            self.work.append((bank, addr))

    def decode_all(self):
        far_hl = self.byname.get("FarCall_hl", (None, None))[1]
        far_de = self.byname.get("FarCall_de", (None, None))[1]
        while self.work:
            bank, addr = self.work.pop()
            b = self.bank(bank)
            hist = []
            while b.lo <= addr < b.hi and addr not in b.insns:
                o = self.off(bank, addr)
                ins = sm83.Insn(addr, self.rom[o:o + 3])
                if addr + ins.size > b.hi:
                    break
                b.insns[addr] = ins
                op = ins.op
                nxt = addr + ins.size
                _, flow = (sm83.sem_cb(ins.b1), None) if op == 0xCB else sm83.sem(ins, 0, 0, 0, 0)
                hist = (hist + [ins])[-3:]
                if flow is None:
                    # a write to the MBC from banked code ends the block
                    if bank and op == 0xEA and ins.imm16 < 0x8000:
                        b.leaders.add(nxt)
                    addr = nxt
                    continue
                kind = flow[0]
                if kind == "jump":
                    t = ins.rel_target if op in (0x18, 0x20, 0x28, 0x30, 0x38) else ins.imm16
                    self.add_root(bank, t)
                    if flow[2] is None:
                        break
                    b.leaders.add(nxt)
                elif kind == "call":
                    t = op & 0x38 if (op & 0xC7) == 0xC7 else ins.imm16
                    tb = bank
                    if t >= 0x4000 and bank == 0:
                        tb = self.known_bank(hist)
                    if tb is not None:
                        self.add_root(tb, t)
                    self.farcall_root(hist, t, far_hl, far_de)
                    b.leaders.add(nxt)
                elif kind in ("ret",):
                    if flow[1] is None:
                        break
                    b.leaders.add(nxt)
                elif kind in ("reti", "jphl", "invalid"):
                    break
                elif kind in ("halt", "stop"):
                    b.leaders.add(nxt)
                addr = nxt

    @staticmethod
    def known_bank(hist):
        # homecall: ld a, BANK ; rst Bankswitch ; call X
        if len(hist) >= 3 and hist[-2].op == 0xD7 and hist[-3].op == 0x3E:
            return hist[-3].b1
        return None

    def farcall_root(self, hist, t, far_hl, far_de):
        if len(hist) < 3:
            return
        p1, p2 = hist[-3], hist[-2]
        regs = {}
        for p in (p1, p2):
            if p.op == 0x3E:
                regs["a"] = p.b1
            elif p.op == 0x21:
                regs["hl"] = p.imm16
            elif p.op == 0x11:
                regs["de"] = p.imm16
        if "a" not in regs:
            return
        if t == 0x08 or (far_hl is not None and t == far_hl):
            if "hl" in regs:
                self.add_root(regs["a"], regs["hl"])
        elif far_de is not None and t == far_de and "de" in regs:
            self.add_root(regs["a"], regs["de"])


# ---------------------------------------------------------------- emission


PREFIX = ""


def fn_name(n):
    return f"rc{PREFIX}_ram_{n - RAM_BANK}" if n >= RAM_BANK else f"rc{PREFIX}_bank_{n:02X}"


def emit_bank(rc, b, out):
    n = b.num
    insns = b.insns
    addrs = sorted(insns)
    # every jump target inside the bank must carry a label
    targets = set(b.leaders) | set(b.roots)
    lines = []
    w = lines.append
    w(f"uint16_t {fn_name(n)}(uint16_t pc)\n{{\n    CPU_LOCALS;\n    goto sw;\ndyn:")
    if n >= RAM_BANK:
        w(f"    if (g_cyc >= g_deadline || pc < 0x{b.lo:04X} || pc >= 0x{b.hi:04X}) goto out;")
    elif n == 0:
        w("    if (g_cyc >= g_deadline || pc >= 0x4000) goto out;")
    else:
        w(f"    if (g_cyc >= g_deadline || pc < 0x4000 || pc >= 0x8000 || g_rom_bank != {n}) goto out;")
    cases = sorted(a for a in targets if a in insns)
    w("sw:\n    switch (pc) {")
    for a in cases:
        w(f"    case 0x{a:04X}: goto L_{a:04X};")
    w("    default: g_rc_miss = 1; goto out;\n    }")

    def jump_to(t):
        if b.lo <= t < b.hi and t in insns:
            return f"if (g_cyc >= g_deadline) {{ pc = 0x{t:04X}; goto out; }} goto L_{t:04X};"
        return f"pc = 0x{t:04X}; goto out;"

    # fallthrough into an instruction that is not the next emitted one needs a label
    for k, a in enumerate(addrs[:-1]):
        nx = a + insns[a].size
        if nx != addrs[k + 1] and nx in insns:
            targets.add(nx)

    prev_next = None
    for a in addrs:
        ins = insns[a]
        if prev_next is not None and prev_next != a:
            # previous instruction falls through to something that is not next
            w("    " + (f"goto L_{prev_next:04X};" if prev_next in insns else f"pc = 0x{prev_next:04X}; goto out;"))
        label_needed = a in targets or prev_next != a
        if label_needed:
            w(f"L_{a:04X}:")
        name = rc.names.get((n, a))
        cmt = f" /* {name} */" if name else ""
        op = ins.op
        nxt = a + ins.size
        base, extra = sm83.cycles(op, ins.b1)
        if op == 0xCB:
            body, flow = sm83.sem_cb(ins.b1), None
        else:
            body, flow = sm83.sem(ins, f"0x{ins.b1:02X}", f"0x{ins.imm16:04X}", nxt, ins.rel_target)
        w(f"    CYC({base}); {' '.join(body)} /* {a:04X}: {ins.text} */{cmt}")
        prev_next = nxt
        if flow is None:
            if n and op == 0xEA and ins.imm16 < 0x8000:
                w(f"    pc = 0x{nxt:04X}; goto dyn;")
                prev_next = None
                targets.add(nxt)
            continue
        k = flow[0]
        if k == "jump":
            cond = flow[2]
            t = ins.rel_target if op in (0x18, 0x20, 0x28, 0x30, 0x38) else ins.imm16
            if cond:
                w(f"    if ({cond}) {{ CYC({extra}); {jump_to(t)} }}")
            else:
                w(f"    {jump_to(t)}")
                prev_next = None
        elif k == "call":
            cond = flow[2]
            tgt = op & 0x38 if (op & 0xC7) == 0xC7 else ins.imm16
            # a call from ROM0 into ROMX, or across banks, always goes through the dispatcher
            body = f"PUSH16(0x{nxt:04X}); {jump_to(tgt)}"
            if cond:
                w(f"    if ({cond}) {{ CYC({extra}); {body} }}")
            else:
                w(f"    {body}")
                # fallthrough to nxt only through the return
                prev_next = None
        elif k == "ret":
            cond = flow[1]
            if cond:
                w(f"    if ({cond}) {{ CYC({extra}); POP16(pc); goto dyn; }}")
            else:
                w("    POP16(pc); goto dyn;")
                prev_next = None
        elif k == "reti":
            w("    R.ime = 1; POP16(pc); goto dyn;")
            prev_next = None
        elif k == "jphl":
            w("    pc = HL; goto dyn;")
            prev_next = None
        elif k == "halt":
            w(f"    pc = 0x{nxt:04X}; R.halted = 1; goto out;")
            prev_next = None
        elif k == "stop":
            w(f"    pc = 0x{nxt:04X}; R.stopped = 1; goto out;")
            prev_next = None
        elif k == "invalid":
            w(f"    pc = 0x{a:04X}; g_rc_miss = 1; goto out;")
            prev_next = None
    if prev_next is not None:
        w(f"    pc = 0x{prev_next:04X}; goto out;")
    w("out:\n    CPU_SAVE();\n    return pc;\n}\n")
    out.write("\n".join(lines) + "\n")
    return len(cases)


class ChangedFile(io.StringIO):
    """Text file written only if its content changed (keeps build objects)."""

    def __init__(self, path):
        super().__init__()
        self.path = path

    def __exit__(self, *exc):
        data = self.getvalue()
        old = None
        if os.path.exists(self.path):
            with open(self.path, encoding="utf-8", newline="") as f:
                old = f.read()
        if old != data:
            with open(self.path, "w", encoding="utf-8", newline="") as f:
                f.write(data)
        return super().__exit__(*exc)


def gen_interp(path):
    """Interpreter generated from the same semantics (one case per opcode)."""
    L = []
    w = L.append
    w("/* generated by pc/tools/recomp/recomp.py - do not edit */")
    w('#include "sm83_ops.h"\n')
    w("uint64_t g_interp_count;\n")
    w("void interp_step(void)\n{\n    CPU_LOCALS;\n    uint16_t pc = R.pc, at = pc;\n    uint8_t op = rd(pc);")
    w("    g_interp_count++;\n    switch (op) {")
    for op in range(256):
        if op == 0xCB:
            w("    case 0xCB: {\n        uint8_t x = rd((uint16_t)(at + 1)); pc = (uint16_t)(at + 2);")
            w("        switch (x) {")
            for x in range(256):
                base, _ = sm83.cycles(0xCB, x)
                w(f"        case 0x{x:02X}: CYC({base}); {' '.join(sm83.sem_cb(x))} break;")
            w("        }\n        break;\n    }")
            continue
        ins = sm83.Insn(0, bytes([op, 0, 0]))
        size = ins.size
        base, extra = sm83.cycles(op)
        body, flow = sm83.sem(ins, "rd((uint16_t)(at + 1))", "rd16((uint16_t)(at + 1))",
                              None, "(uint16_t)(at + 2 + (int8_t)rd((uint16_t)(at + 1)))")
        s = [f"pc = (uint16_t)(at + {size});", f"CYC({base});"] + body
        if flow:
            k = flow[0]
            if k == "jump":
                t, cond = flow[1], flow[2]
                tt = t if isinstance(t, str) else f"0x{t:04X}"
                s.append(f"if ({cond}) {{ CYC({extra}); pc = {tt}; }}" if cond else f"pc = {tt};")
            elif k == "call":
                t, cond = flow[1], flow[2]
                tt = t if isinstance(t, str) else f"0x{t:04X}"
                s.append(f"if ({cond}) {{ CYC({extra}); PUSH16(pc); pc = {tt}; }}" if cond
                         else f"PUSH16(pc); pc = {tt};")
            elif k == "ret":
                cond = flow[1]
                s.append(f"if ({cond}) {{ CYC({extra}); POP16(pc); }}" if cond else "POP16(pc);")
            elif k == "reti":
                s.append("R.ime = 1; POP16(pc);")
            elif k == "jphl":
                s.append("pc = HL;")
            elif k == "halt":
                s.append("R.halted = 1;")
            elif k == "stop":
                s.append("R.stopped = 1;")
            elif k == "invalid":
                s.append("pc = at; interp_invalid(at, op);")
        w(f"    case 0x{op:02X}: {' '.join(s)} break; /* {ins.text} */")
    w("    }\n    CPU_SAVE();\n    R.pc = pc;\n}")
    with ChangedFile(path) as f:
        f.write("\n".join(L) + "\n")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--rom", required=True)
    ap.add_argument("--sym", help="rgblink .sym of this ROM (omit for a ROM without symbols)")
    ap.add_argument("--src", help="disassembly sources (to tell code labels from data)")
    ap.add_argument("--out", required=True)
    ap.add_argument("--extra", action="append", default=[])
    ap.add_argument("--name", default="game")
    ap.add_argument("--ram-copy", action="append", default=[],
                    help="BB:AAAA:RRRR:LEN (hex): code at ROM BB:AAAA copied to RAM RRRR")
    ap.add_argument("--prefix", default="en", help="symbol prefix (several translated ROMs in one executable)")
    ap.add_argument("--lang", default="en", help="language of this ROM (shown to the launcher)")
    args = ap.parse_args()
    global PREFIX
    PREFIX = "_" + args.prefix

    rom = open(args.rom, "rb").read()
    syms = parse_sym(args.sym) if args.sym else []
    code, data = classify_source(args.src) if args.src else (set(), set())
    # roots given as "BB:AAAA # Name" also name the address (ROM without .sym)
    extra = []
    for path in args.extra:
        if os.path.exists(path):
            for line in open(path):
                body, _, cmt = line.partition("#")
                body = body.strip()
                if body.startswith("ram "):
                    args.ram_copy.append(body[4:].strip())
                elif body:
                    bk, ad = body.split(":")
                    extra.append((int(bk, 16), int(ad, 16), cmt.strip()))
    if not args.sym:
        syms = [(b, a, n) for b, a, n in extra if n]
    rc = Recomp(rom, syms)
    nroots = 0
    for bank, addr, name in syms:
        if addr < 0x8000 and name in code:
            rc.add_root(bank, addr)
            nroots += 1
    byname = {n: (b, a) for b, a, n in syms}
    ntext = 0
    for name, off in TEXT_ASM:
        if name in byname:
            b, a = byname[name]
            rc.add_root(b, a + off)
            rc.names.setdefault((b if a >= 0x4000 else 0, a + off), name + f"+{off}(text_asm)")
            ntext += 1
    print(f"text_asm code: {ntext}/{len(TEXT_ASM)}")
    copies = []
    for rom_l, ram_l, end_l in LOADS:
        if rom_l in byname and ram_l in byname and end_l in byname:
            (rb, ra), (_, mem), (_, ea) = byname[rom_l], byname[ram_l], byname[end_l]
            if ra < 0x8000 <= mem:
                copies.append((rb, ra, mem, ea - ra))
    for spec in args.ram_copy:
        bk, ra, mem, ln = (int(x, 16) for x in spec.split(":"))
        copies.append((bk, ra, mem, ln))
    for rb, ra, mem, ln in copies:
        off = ra if ra < 0x4000 else rb * 0x4000 + ra - 0x4000
        n = rc.add_ram_copy(off, mem, ln)
        print(f"RAM code: {rb:02X}:{ra:04X} -> {mem:04X} ({ln} bytes) as {fn_name(n)}")
    for v in list(range(0, 0x40, 8)) + [0x40, 0x48, 0x50, 0x58, 0x60, 0x100]:
        rc.add_root(0, v)
    for bk, ad, _ in extra:
        rc.add_root(bk, ad)
    rc.decode_all()

    os.makedirs(args.out, exist_ok=True)
    total_i, total_c = 0, 0
    files = []
    for n in sorted(rc.banks):
        b = rc.banks[n]
        if not b.insns:
            continue
        fn = os.path.join(args.out, f"{fn_name(n)}.c")
        with ChangedFile(fn) as f:
            f.write("/* generated by pc/tools/recomp/recomp.py - do not edit */\n")
            f.write('#include "sm83_ops.h"\n\n')
            total_c += emit_bank(rc, b, f)
        total_i += len(b.insns)
        files.append(n)
    rom_files = [n for n in files if n < RAM_BANK]
    ram_files = [n for n in files if n >= RAM_BANK]
    P = args.prefix
    with ChangedFile(os.path.join(args.out, "rc_table.c")) as f:
        f.write('#include "gb.h"\n')
        for n in files:
            f.write(f"uint16_t {fn_name(n)}(uint16_t pc);\n")
        nb = rc.nbanks
        f.write(f"static const bank_fn banks[{nb}] = {{\n")
        for n in rom_files:
            f.write(f"    [0x{n:02X}] = {fn_name(n)},\n")
        f.write("};\n")
        # code copied to RAM: run only while the RAM still holds the ROM bytes
        f.write("static const RamCode ram[] = {\n")
        for n in ram_files:
            b = rc.banks[n]
            f.write(f"    {{ 0x{b.lo:04X}, {b.hi - b.lo}, 0x{b.rom_off:06X}, {fn_name(n)} }},\n")
        f.write("    { 0, 0, 0, 0 }\n};\n")
        # one descriptor per translated ROM; the runtime picks it by CRC
        f.write(f"const RcGame rc_game_{P} = {{ \"{args.name}\", \"{args.lang}\", "
                f"0x{zlib.crc32(rom) & 0xFFFFFFFF:08X}, banks, {nb}, ram, {len(ram_files)} }};\n")
    gen_interp(os.path.join(args.out, "interp_gen.c"))
    print(f"labels: {len(code)} code / {len(data)} data in sources; {nroots} code roots from sym")
    print(f"banks with code: {len(files)}; instructions: {total_i}; entry points: {total_c}")


if __name__ == "__main__":
    main()
