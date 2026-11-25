#!/usr/bin/env python3
"""Build the native PC executable(s) of this repository in one command.

    python pc/build.py [--exe NAME] [--rom-fr "<French ROM>"] [--sdl2-dir <static SDL2 build>]

Steps: build the English ROM(s) with rgbds (`make`, needed for the .sym file),
translate them to C (pc/tools/recomp/recomp.py), translate the French ROM if
given (its entry points are in pc/roots_*_fr*.txt), then CMake + Ninja.
The games of this repository are described in pc/games.json.

Requirements: rgbds 1.0.x and make (to build the ROM), Python 3, CMake, Ninja,
Clang (or GCC), SDL2 (system package, a static build given with --sdl2-dir,
or downloaded by CMake).
"""
import argparse
import json
import os
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PC = os.path.join(ROOT, "pc")


def run(cmd, **kw):
    print("+", " ".join(cmd), flush=True)
    subprocess.run(cmd, check=True, cwd=ROOT, **kw)


def main():
    games = json.load(open(os.path.join(PC, "games.json"), encoding="utf-8"))
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", help="build only this executable (" + ", ".join(g["exe"] for g in games) + ")")
    ap.add_argument("--rom-fr", action="append", default=[], help="French ROM (repeat for each version)")
    ap.add_argument("--sdl2-dir", help="build directory of a static SDL2 (with libSDL2.a)")
    ap.add_argument("--build", default="build", help="build directory (default: build)")
    ap.add_argument("--make", default="make" if shutil.which("make") else "mingw32-make")
    a = ap.parse_args()

    cc = "clang" if shutil.which("clang") else "gcc"
    for g in games:
        if a.exe and g["exe"] != a.exe:
            continue
        en = g["en"]
        run([a.make, "-j8", en["make"]])
        rc_dirs = []
        out = os.path.join(a.build, "rc_" + en["prefix"])
        run([sys.executable, os.path.join(PC, "tools", "recomp", "recomp.py"), "--rom", en["rom"], "--sym", en["sym"],
             "--src", ".", "--out", out, "--name", en["name"], "--prefix", en["prefix"], "--lang", "en"])
        rc_dirs.append(out)
        fr = g.get("fr")
        if fr:
            # the French ROM of this version: the one whose title matches
            rom = None
            for path in a.rom_fr:
                with open(path, "rb") as f:
                    title = f.read(0x144)[0x134:0x144]
                if fr["title"].encode() in title:
                    rom = path
            if rom:
                out = os.path.join(a.build, "rc_" + en["prefix"] + "_fr")
                cmd = [sys.executable, os.path.join(PC, "tools", "recomp", "recomp.py"), "--rom", rom, "--out", out,
                       "--name", fr["name"], "--prefix", "fr", "--lang", "fr"]
                for r in fr["roots"]:
                    cmd += ["--extra", r]
                run(cmd)
                rc_dirs.append(out)
            else:
                print(f"note: no French ROM for {g['exe']} (--rom-fr): English only")
        bdir = os.path.join(a.build, "pc_" + g["exe"])
        cmd = ["cmake", "-S", "pc", "-B", bdir, "-G", "Ninja", f"-DCMAKE_C_COMPILER={cc}",
               "-DRC_DIRS=" + ";".join(rc_dirs), f"-DGAME_NAME={g['exe']}"]
        if a.sdl2_dir:
            cmd.append(f"-DSDL2_PREBUILT_DIR={a.sdl2_dir}")
        run(cmd)
        run(["cmake", "--build", bdir])
        print(f"\n==> {os.path.join(bdir, g['exe'])}  (give it your ROM: see README)\n")


if __name__ == "__main__":
    main()
