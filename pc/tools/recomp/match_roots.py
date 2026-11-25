#!/usr/bin/env python3
"""Find the code labels of the disassembly in another build of the same game
(e.g. the French ROM) by byte matching.

    python pc/tools/recomp/match_roots.py --rom pokecrystal.gbc --sym pokecrystal.sym --src . \
        --target <French ROM>.gbc --out build/roots_fr.txt

For each code label of the reference ROM (bank:addr), the bytes from the label
to the end of its first basic block (at most 48 bytes) form a pattern in which
the operands holding absolute addresses (call/jp nn, ld rr,nn, ld [nn]) are
wildcards. The pattern is searched in the same bank of the target ROM, then in
every bank; a root is kept only if the match is unique. Output: one
`BB:AAAA # Name` line per root, for recomp.py --extra (with --no-sym).
"""
import argparse
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import sm83  # noqa: E402
import recomp  # noqa: E402

ABS16 = {0x01, 0x11, 0x21, 0x31, 0x08, 0xC2, 0xC3, 0xC4, 0xCA, 0xCC, 0xCD, 0xD2, 0xD4, 0xDA, 0xDC, 0xEA, 0xFA}
END = {0xC9, 0xD9, 0xC3, 0x18, 0xE9}


def pattern(rom, off, maxlen=48):
    parts, n, pos = [], 0, off
    while n < maxlen and pos < len(rom):
        ins = sm83.Insn(0, rom[pos:pos + 3])
        op = ins.op
        if op in sm83.INVALID:
            return None, 0
        if op in ABS16:
            parts.append(re.escape(bytes([op])) + b"..")
        elif op == 0x3E and n == 0:
            parts.append(re.escape(rom[pos:pos + ins.size]))
        else:
            parts.append(re.escape(rom[pos:pos + ins.size]))
        n += ins.size
        pos += ins.size
        if op in END:
            break
    return b"".join(parts), n


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--rom", required=True)
    ap.add_argument("--sym", required=True)
    ap.add_argument("--src", required=True)
    ap.add_argument("--target", required=True)
    ap.add_argument("--out", required=True)
    args = ap.parse_args()
    ref = open(args.rom, "rb").read()
    tgt = open(args.target, "rb").read()
    code, _ = recomp.classify_source(args.src)
    syms = recomp.parse_sym(args.sym)
    found, ambiguous, missing, short = 0, 0, 0, 0
    out = []
    banks = [tgt[b * 0x4000:(b + 1) * 0x4000] for b in range(len(tgt) // 0x4000)]
    byname = {n: (b, a) for b, a, n in syms}
    cands = [(b, a, n) for b, a, n in syms if a < 0x8000 and n in code]
    # code that follows text_asm inside texts
    for name, o in recomp.TEXT_ASM:
        if name in byname:
            b, a = byname[name]
            cands.append((b, a + o, f"{name}+{o}(text_asm)"))
    # code copied to RAM (LOAD blocks): found by its bytes, kept as "ram" lines
    for rom_l, ram_l, end_l in recomp.LOADS:
        if rom_l in byname and ram_l in byname and end_l in byname:
            (rb, ra), (_, mem), (_, ea) = byname[rom_l], byname[ram_l], byname[end_l]
            o = ra if ra < 0x4000 else rb * 0x4000 + ra - 0x4000
            blob = ref[o:o + ea - ra]
            hits = [m.start() for m in re.finditer(re.escape(blob), banks[rb if ra >= 0x4000 else 0])]
            if len(hits) == 1:
                base = 0 if ra < 0x4000 else 0x4000
                out.append(f"ram {rb:02X}:{hits[0] + base:04X}:{mem:04X}:{ea - ra:X} # {ram_l}")
    import bisect
    pending = []          # (bank, addr, name, pattern regex or None, length)
    unique = {}           # bank -> sorted list of (ref addr, target addr)
    for bank, addr, name in cands:
        if addr < 0x4000:
            bank = 0
        base = 0 if bank == 0 else 0x4000
        off = addr if bank == 0 else bank * 0x4000 + addr - 0x4000
        pat, n = pattern(ref, off)
        if not pat or n < 4:
            pending.append((bank, addr, name, None, n))
            continue
        rx = re.compile(pat, re.DOTALL)
        hits = [m.start() for m in rx.finditer(banks[bank])]
        if len(hits) > 1 and addr - base in hits:
            hits = [addr - base]
        if len(hits) == 1:
            out.append(f"{bank:02X}:{hits[0] + base:04X} # {name}")
            unique.setdefault(bank, []).append((addr, hits[0] + base))
            found += 1
        else:
            pending.append((bank, addr, name, rx if hits else None, n))
    for v in unique.values():
        v.sort()
    # second pass: code moves by blocks; predict the target address from the
    # nearest unique match before it in the same bank, and accept a candidate
    # that sits exactly there (or the only hit near it)
    for bank, addr, name, rx, n in pending:
        base = 0 if bank == 0 else 0x4000
        lst = unique.get(bank, [])
        i = bisect.bisect_right(lst, (addr, 0x10000)) - 1
        if i < 0:
            missing += 1
            continue
        pred = addr - lst[i][0] + lst[i][1]
        off = addr if bank == 0 else bank * 0x4000 + addr - 0x4000
        toff = pred - base
        if rx is not None:
            m = rx.match(banks[bank], toff) if 0 <= toff < 0x4000 else None
            ok = m is not None
        else:
            ln = max(n, 1)
            ok = 0 <= toff and toff + ln <= 0x4000 and banks[bank][toff:toff + ln] == ref[off:off + ln]
        if ok:
            out.append(f"{bank:02X}:{pred:04X} # {name}")
            found += 1
        elif rx is not None:
            ambiguous += 1
        elif n < 4:
            short += 1
        else:
            missing += 1
    with open(args.out, "w") as f:
        f.write("\n".join(out) + "\n")
    print(f"roots found {found}, ambiguous {ambiguous}, not found {missing}, too short {short}")


if __name__ == "__main__":
    main()
