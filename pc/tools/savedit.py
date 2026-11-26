#!/usr/bin/env python3
"""Read and edit a save file (.sav) of the Game Boy Pokémon games, for tests.

    python pc/tools/savedit.py SAVE.sav GAME.sym get wMapGroup wXCoord wPartyCount:7
    python pc/tools/savedit.py SAVE.sav GAME.sym set wXCoord=5 wYCoord=3 wJohtoBadges=ff
    python pc/tools/savedit.py SAVE.sav GAME.sym set "wPartyMon1Level=64" "wPartyMon1MaxHP=03 e7"

Names are the WRAM labels of the disassembly (.sym of the ROM the save comes
from); `NAME+N` adds an offset, values are hex bytes. The tool maps each WRAM
label to its place in the save (the game copies blocks of WRAM to SRAM when
saving) and rewrites the checksum, so the game accepts the edited save:
  - 2nd generation (Gold/Silver/Crystal): wPlayerData, wCurMapData,
    wPokemonData -> sPlayerData, sCurMapData, sPokemonData; 16-bit sum of
    sGameData..sGameDataEnd in sChecksum (the backup copy is left as is);
  - 1st generation (Red/Blue/Yellow): wPlayerName, wMainData, wSpriteData,
    wPartyData, wBoxData -> sPlayerName, sMainData, sSpriteData, sPartyData,
    sCurBoxData; complemented 8-bit sum in sMainDataCheckSum.
Only for the English ROMs built from this repository (their .sym); the
French ROMs have other WRAM addresses.
"""
import sys


def load_sym(path):
    syms = {}
    for line in open(path, encoding="utf-8", errors="replace"):
        p = line.split(";")[0].split()
        if len(p) == 2 and ":" in p[0]:
            b, a = p[0].split(":")
            syms[p[1]] = (int(b, 16), int(a, 16))
    return syms


class Save:
    def __init__(self, sav, sym):
        self.path = sav
        self.data = bytearray(open(sav, "rb").read())
        self.s = load_sym(sym)
        s = self.s
        if "sPokemonData" in s:          # 2nd generation
            self.gen = 2
            self.regions = [(s["wPlayerData"][1], s["wPlayerDataEnd"][1], s["sPlayerData"]),
                            (s["wCurMapData"][1], s["wCurMapDataEnd"][1], s["sCurMapData"]),
                            (s["wPokemonData"][1], s["wPokemonDataEnd"][1], s["sPokemonData"])]
        else:                            # 1st generation
            self.gen = 1
            self.regions = [(s["wPlayerName"][1], s["wPlayerName"][1] + 11, s["sPlayerName"]),
                            (s["wMainDataStart"][1], s["wMainDataEnd"][1], s["sMainData"]),
                            (s["wSpriteDataStart"][1], s["wSpriteDataEnd"][1], s["sSpriteData"]),
                            (s["wPartyDataStart"][1], s["wPartyDataEnd"][1], s["sPartyData"]),
                            (s["wBoxDataStart"][1], s["wBoxDataEnd"][1], s["sCurBoxData"])]

    @staticmethod
    def sram_off(bank, addr):
        return bank * 0x2000 + addr - 0xA000

    def off(self, name):
        base, _, plus = name.partition("+")
        if base not in self.s:
            sys.exit(f"unknown label {base}")
        bank, addr = self.s[base]
        addr += int(plus, 0) if plus else 0
        if addr >= 0xA000 and addr < 0xC000:      # an SRAM label itself
            return self.sram_off(bank, addr)
        for lo, hi, (sb, sa) in self.regions:
            if lo <= addr < hi:
                return self.sram_off(sb, sa + addr - lo)
        sys.exit(f"{name} ({addr:04X}) is not saved by the game")

    def checksum(self):
        s = self.s
        b, lo = s["sGameData"]
        _, hi = s["sGameDataEnd"]
        a, z = self.sram_off(b, lo), self.sram_off(b, hi)
        if self.gen == 2:
            v = sum(self.data[a:z]) & 0xFFFF
            o = self.sram_off(*s["sChecksum"])
            self.data[o], self.data[o + 1] = v & 0xFF, v >> 8
        else:
            v = (~sum(self.data[a:z])) & 0xFF
            self.data[self.sram_off(*s["sMainDataCheckSum"])] = v

    def write(self):
        self.checksum()
        open(self.path, "wb").write(self.data)


def main():
    if len(sys.argv) < 4:
        sys.exit(__doc__)
    sv = Save(sys.argv[1], sys.argv[2])
    cmd = sys.argv[3]
    if cmd == "get":
        for arg in sys.argv[4:]:
            name, _, n = arg.partition(":")
            o = sv.off(name)
            print(f"{name} = {sv.data[o:o + int(n or 1)].hex(' ')}")
    elif cmd == "set":
        for arg in sys.argv[4:]:
            name, _, val = arg.partition("=")
            o = sv.off(name)
            b = bytes.fromhex(val)
            sv.data[o:o + len(b)] = b
        sv.write()
        print(f"saved {sv.path} (generation {sv.gen}, checksum updated)")
    elif cmd == "teleport1":
        # 1st generation: put the player on map MAP (index) at X,Y, MAP being
        # WIDTH blocks wide: what a warp does (macro warp_to / event_displacement)
        m, w, x, y = (int(v, 0) for v in sys.argv[4:8])
        ptr = sv.s["wOverworldMap"][1] + 7 + w + (w + 6) * (y >> 1) + (x >> 1)
        for name, val in (("wCurMap", bytes([m])), ("wCurrentTileBlockMapViewPointer", bytes([ptr & 0xFF, ptr >> 8])),
                          ("wYCoord", bytes([y])), ("wXCoord", bytes([x])),
                          ("wYBlockCoord", bytes([y & 1])), ("wXBlockCoord", bytes([x & 1]))):
            o = sv.off(name)
            sv.data[o:o + len(val)] = val
        sv.write()
        print(f"saved {sv.path}: map {m:#04x} at {x},{y} (view pointer {ptr:04X})")
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main()
