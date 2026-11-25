#!/usr/bin/env python3
"""Windows/Linux package: executable + notice + configuration template.

    python pc/tools/package.py --exe build/pc/pokecrystal.exe --out dist [--rom <ROM> ...]
    python pc/tools/package.py --exe <linux build>/pokecrystal --out dist ...

A Windows executable (.exe) gives a .zip, a Linux one a .tar.gz (keeps the
executable bit). No game data goes in the package: the player gives his own
ROM. The script refuses ROMs, saves and states, and checks that no bank of
the given ROMs appears in the executable.
"""
import argparse
import io
import os
import subprocess
import sys
import tarfile
import time
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
  {run} "{rompath}"
ou posez la ROM à côté du programme, ou indiquez le dossier de vos ROM
dans {ini} (la bonne ROM y est reconnue à son contenu, quel que soit
son nom) :
  rom_dir = C:\\...\\roms
  lang = fr
Chemins précis possibles : rom_en= / rom_fr=.
Options : --lang en|fr  --rom-dir DOSSIER  --fullscreen  --scale N  --save fichier.sav

Commandes
---------
  Flèches = croix, X ou Espace = A, Z ou Ctrl = B, Entrée = Start,
  Retour arrière = Select, Tab (maintenu) = avance rapide, F11 = plein écran.
  Manette : reconnue automatiquement (A, B, Start, Select/Back, croix, stick).

Sauvegarde
----------
  {saves} à côté du programme, au format des émulateurs : une sauvegarde
  d'émulateur (.sav) peut être copiée sous ce nom.
{platform}"""

LINUX_NOTE = """
Linux
-----
  Binaire x86-64 lié statiquement à SDL2 (fenêtre X11 ou Wayland, son ALSA,
  PulseAudio ou PipeWire) ; il n'utilise que la glibc du système, version
  2.34 ou plus récente (Ubuntu 22.04, Debian 12, Fedora 35 et suivantes).
"""

INI = ("# Dossier de vos ROM : le jeu y reconnait la bonne ROM a son contenu, quel que soit son nom\n"
       "# (cherchee aussi a cote du programme). Folder holding your ROMs (picked by content).\n"
       "rom_dir =\n"
       "# Langue : en ou fr / language: en or fr\n"
       "# lang=fr\n"
       "# Chemins precis (facultatif) / explicit paths (optional)\n"
       "# rom_en=\n# rom_fr=\n"
       "# fullscreen=0\n# scale=4\n")


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
    ap.add_argument("--rev", help="version tag in the file name (default: git short hash)")
    a = ap.parse_args()
    exe = a.exe
    base = os.path.basename(exe)
    name = os.path.splitext(base)[0]
    linux = not base.lower().endswith(".exe")
    if base.lower().endswith(FORBIDDEN):
        sys.exit(f"refused: {exe}")
    rev = a.rev
    if not rev:
        try:
            rev = subprocess.run(["git", "rev-parse", "--short", "HEAD"], capture_output=True,
                                 text=True).stdout.strip() or "local"
        except OSError:
            rev = "local"
    blob = open(exe, "rb").read()
    for rom in a.rom:
        data = open(rom, "rb").read()
        # several graphics/text banks: none may appear verbatim in the executable
        for bank in range(0x10, len(data) // 0x4000, 7):
            chunk = data[bank * 0x4000: bank * 0x4000 + 256]
            if chunk.strip(b"\0\xff") and chunk in blob:
                sys.exit(f"refused: the executable contains bytes of {rom} (bank {bank:02X})")
    os.makedirs(a.out, exist_ok=True)
    ini = f"{name}.ini"
    roms = "\n".join(f"  - {e}" for e in a.expect) or "  - (voir la documentation)"
    notice = NOTICE.format(title=a.title, repo=a.repo, roms=roms, saves=a.saves, ini=ini,
                           run=("./" + base) if linux else base,
                           rompath="chemin/vers/la/rom.gb" if linux else "chemin\\vers\\la\\rom.gb",
                           platform=LINUX_NOTE if linux else "")
    if linux:
        path = os.path.join(a.out, f"{name}-pc-{rev}-linux-x86_64.tar.gz")

        def add(tf, arc, data, mode):
            ti = tarfile.TarInfo(arc)
            ti.size, ti.mode, ti.mtime = len(data), mode, int(time.time())
            tf.addfile(ti, io.BytesIO(data))
        with tarfile.open(path, "w:gz") as tf:
            add(tf, f"{name}/{base}", blob, 0o755)
            add(tf, f"{name}/LISEZMOI.txt", notice.encode("utf-8"), 0o644)
            add(tf, f"{name}/{ini}", INI.encode(), 0o644)
    else:
        path = os.path.join(a.out, f"{name}-pc-{rev}-windows-x64.zip")
        with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED) as z:
            z.write(exe, f"{name}/{base}")
            z.writestr(f"{name}/LISEZMOI.txt", notice)
            z.writestr(f"{name}/{ini}", INI)
    print(path)


if __name__ == "__main__":
    main()
