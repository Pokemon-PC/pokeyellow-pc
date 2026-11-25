#!/usr/bin/env python3
"""Scripted play of the recompiled game (tests).

    python pc/tools/play.py --exe build/pc/pokecrystal.exe --rom <rom> --dir build/run \
        [--load state.bin] [--start FRAME] "steps..."

Steps (separated by ';' or newlines):
    wait N          let N frames pass
    press KEYS [N]  hold KEYS (a b s t u d l r, e.g. 'a' or 'ua') for N frames (default 4), then release 4
    hold KEYS N     hold KEYS for N frames (no release pause)
    mash KEYS N     press KEYS every 20 frames for N frames
    shot NAME       screenshot NAME.bmp now
    save NAME       save state NAME.bin now (one per run)
The run ends 2 frames after the last step; a contact sheet of the shots is
written to <dir>/sheet.png.
"""
import argparse
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", required=True)
    ap.add_argument("--rom", required=True)
    ap.add_argument("--dir", default="build/run")
    ap.add_argument("--load")
    ap.add_argument("--start", type=int, default=0, help="frame number stored in the loaded state")
    ap.add_argument("--save", default="test.sav")
    ap.add_argument("steps")
    a = ap.parse_args()

    f = a.start + 1
    inputs, shots, state = [], [], None
    for raw in a.steps.replace("\n", ";").split(";"):
        p = raw.split()
        if not p:
            continue
        cmd = p[0]
        if cmd == "wait":
            f += int(p[1])
        elif cmd in ("press", "hold"):
            n = int(p[2]) if len(p) > 2 else 4
            inputs += [f"{f}={p[1]}", f"{f + n}="]
            f += n + (4 if cmd == "press" else 0)
        elif cmd == "mash":
            end = f + int(p[2])
            while f < end:
                inputs += [f"{f}={p[1]}", f"{f + 4}="]
                f += 20
        elif cmd == "shot":
            shots.append((f, p[1]))
        elif cmd == "save":
            state = (f, p[1])
        else:
            sys.exit(f"unknown step {raw!r}")
    env = dict(os.environ)
    env.update(GB_HEADLESS="1", GB_FAST="1", GB_MUTE="1", GB_FRAMES=str(f + 2), GB_SAVE=a.save,
               GB_INPUT=";".join(inputs), GB_SHOT=",".join(f"{fr}:{n}.bmp" for fr, n in shots))
    if state:
        env["GB_STATE_SAVE"] = f"{state[0]}:{state[1]}.bin"
    if a.load:
        env["GB_STATE_LOAD"] = os.path.abspath(a.load)
    os.makedirs(a.dir, exist_ok=True)
    r = subprocess.run([os.path.abspath(a.exe), a.rom], cwd=a.dir, env=env, capture_output=True, text=True,
                       timeout=900)
    log = [l for l in (r.stdout + r.stderr).splitlines() if not l.startswith(("shot:", "save: written"))]
    print("\n".join(log[-30:]))
    print(f"end frame {f + 2}, exit {r.returncode}")
    if shots:
        files = [os.path.join(a.dir, n + ".bmp") for _, n in shots]
        subprocess.run([sys.executable, os.path.join(HERE, "shots.py"), os.path.join(a.dir, "sheet.png")] + files)


if __name__ == "__main__":
    main()
