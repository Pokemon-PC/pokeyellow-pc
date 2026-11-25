#!/usr/bin/env python3
"""Read game variables from a runtime save state (GB_STATE_SAVE).

    python pc/tools/peek.py state.bin pokecrystal.sym wMapGroup wMapNumber wXCoord wYCoord [wPartyCount:6]

A label may be followed by :N to dump N bytes.
"""
import struct
import sys

# layout of the state (main.c state_file, mem.c mem_state)
CPU_SIZE = 20
OFF_WRAM = 4 + 8 + CPU_SIZE + 8 + 8 + 4 + 4 + 2 * 0x2000
OFF_HRAM = OFF_WRAM + 8 * 0x1000 + 0xA0 + 16 * 0x2000
OFF_IO = OFF_HRAM + 0x100


def main():
    st = open(sys.argv[1], "rb").read()
    syms = {}
    for line in open(sys.argv[2]):
        p = line.split(";")[0].split()
        if len(p) == 2 and ":" in p[0]:
            b, a = p[0].split(":")
            syms[p[1]] = (int(b, 16), int(a, 16))
    svbk = st[OFF_IO + 0x70] & 7 or 1
    for arg in sys.argv[3:]:
        name, _, n = arg.partition(":")
        n = int(n or 1)
        bank, addr = syms[name]
        out = []
        for k in range(n):
            a = addr + k
            if 0xC000 <= a < 0xD000:
                out.append(st[OFF_WRAM + a - 0xC000])
            elif 0xD000 <= a < 0xE000:
                wb = bank if bank else svbk
                out.append(st[OFF_WRAM + wb * 0x1000 + a - 0xD000])
            elif 0xFF80 <= a < 0xFFFF:
                out.append(st[OFF_HRAM + (a & 0xFF)])
            else:
                out.append(None)
        print(f"{name} = " + " ".join("--" if v is None else f"{v:02X}({v})" for v in out))


if __name__ == "__main__":
    main()
