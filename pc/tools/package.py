#!/usr/bin/env python3
"""Windows/Linux package: executable + notice + configuration template.

    python pc/tools/package.py --exe build/pc/pokecrystal.exe --out dist

No game data goes in the package: the player gives his own ROM. The script
refuses ROMs, saves and states, and checks the executable holds no ROM bank.
"""
import argparse
import os
import subprocess
import sys
import zipfile

FORBIDDEN = (".gbc", ".gb", ".sav", ".rtc", ".sgm", ".bin", ".sn1", ".state")

NOTICE = """{title} — version PC native
==========================================

Ce programme fait tourner {title} sans émulateur : le code du jeu a été
traduit en C (recompilation statique du désassemblage pret/{repo}) et le
matériel Game Boy / Game Boy Color est reproduit (écran, son, manette,
horloge, sauvegarde).

Il ne contient AUCUNE donnée du jeu : il faut votre propre ROM.
{roms}

Lancement
---------
  {exe} "chemin\\vers\\la\\rom.gbc"
ou remplissez {ini} à côté du programme :
  lang=fr
  rom_en=C:\\...\\Pokemon - Crystal Version (USA).gbc
  rom_fr=C:\\...\\Pokemon - Version Cristal (France).gbc
Options : --lang en|fr  --fullscreen  --scale N  --save fichier.sav

Commandes
---------
  Flèches = croix, X ou Espace = A, Z ou Ctrl = B, Entrée = Start,
  Retour arrière = Select, Tab (maintenu) = avance rapide, F11 = plein écran.
  Manette : reconnue automatiquement (A, B, Start, Select/Back, croix, stick).

Sauvegarde
----------
  {saves} à côté du programme, au format des émulateurs : une sauvegarde
  d'émulateur (.sav) peut être copiée sous ce nom.
"""


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", required=True)
    ap.add_argument("--out", default="dist")
    ap.add_argument("--rom", action="append", default=[], help="ROMs that must not appear in the executable")
    ap.add_argument("--title", default="Pokémon Cristal / Crystal")
    ap.add_argument("--repo", default="pokecrystal")
    ap.add_argument("--expect", action="append", default=[],
                    help='ROM the player needs, e.g. "Anglais : Pokemon - Crystal Version (USA)"')
    ap.add_argument("--saves", default="pokemon_crystal.sav (anglais) / pokemon_cristal.sav (français)")
    a = ap.parse_args()
    exe = a.exe
    name = os.path.splitext(os.path.basename(exe))[0]
    try:
        rev = subprocess.run(["git", "rev-parse", "--short", "HEAD"], capture_output=True, text=True).stdout.strip()
    except OSError:
        rev = "local"
    blob = open(exe, "rb").read()
    for rom in a.rom:
        data = open(rom, "rb").read()
        # a few graphics/text banks: none may appear verbatim in the executable
        for bank in (0x10, 0x20, 0x30, 0x48, 0x60, 0x70):
            chunk = data[bank * 0x4000: bank * 0x4000 + 256]
            if chunk.strip(b"\0\xff") and chunk in blob:
                sys.exit(f"refused: the executable contains bytes of {rom} (bank {bank:02X})")
    os.makedirs(a.out, exist_ok=True)
    zpath = os.path.join(a.out, f"{name}-pc-{rev}.zip")
    ini = f"{name}.ini"
    with zipfile.ZipFile(zpath, "w", zipfile.ZIP_DEFLATED) as z:
        for f in [exe]:
            if f.lower().endswith(FORBIDDEN):
                sys.exit(f"refused: {f}")
        z.write(exe, f"{name}/{os.path.basename(exe)}")
        roms = "\n".join(f"  - {e}" for e in a.expect) or "  - (voir la documentation)"
        z.writestr(f"{name}/LISEZMOI.txt", NOTICE.format(exe=os.path.basename(exe), ini=ini, title=a.title,
                                                         repo=a.repo, roms=roms, saves=a.saves))
        z.writestr(f"{name}/{ini}", "# lang=fr\n# rom_en=\n# rom_fr=\n# fullscreen=0\n# scale=4\n")
    print(zpath)


if __name__ == "__main__":
    main()
