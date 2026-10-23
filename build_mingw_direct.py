#!/usr/bin/env python3
# build_mingw_direct.py - MinGW32 direct build helper for EZ2CustomMod.dll
# Avoids CMake. Run from MSYS2 MINGW32 shell, or from CMD/PowerShell with C:\msys64\mingw32\bin in PATH.

import argparse
import os
import platform
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent
SRC = ROOT / "src"
OUT = ROOT / "EZ2CustomMod.dll"
OBJDIR = ROOT / "build-mingw-direct-x86"

SOURCES = [
    "mod_main.cpp",
    "config.cpp",
    "logger.cpp",
    "patch.cpp",
    "iat.cpp",
    "window_hook.cpp",
    "bms_scanner.cpp",
    "audio_decode.cpp",
    "fnex_crypto.cpp",
    "trace_hooks.cpp",
    "progress_popup.cpp",
]


def which(name: str):
    return shutil.which(name)


def run(cmd, cwd=ROOT):
    print("\n> " + " ".join(str(x) for x in cmd))
    p = subprocess.run(cmd, cwd=str(cwd), text=True)
    if p.returncode != 0:
        raise SystemExit(p.returncode)


def output(cmd):
    try:
        return subprocess.check_output(cmd, text=True, stderr=subprocess.STDOUT).strip()
    except Exception:
        return ""


def ensure_mingw32():
    gpp = which("g++") or which("i686-w64-mingw32-g++")
    if not gpp:
        raise SystemExit(
            "ERROR: g++ not found in PATH.\n"
            "Open MSYS2 MINGW32 or install it: pacman -S --needed mingw-w64-i686-gcc\n"
            "You can also run this from CMD with C:\\msys64\\mingw32\\bin in PATH."
        )

    target = output([gpp, "-dumpmachine"])
    print("g++ found:", gpp)
    print("Target:", target)
    msystem = os.environ.get("MSYSTEM", "")
    if msystem:
        print("MSYSTEM:", msystem)

    if "i686" not in target and "mingw32" not in target.lower():
        raise SystemExit(
            "ERROR: the compiler found does not look like 32-bit/i686.\n"
            f"Target found: {target}\n"
            "You need MSYS2 MINGW32, not MINGW64/UCRT64/CLANG64.\n"
            "Open: C:\\msys64\\msys2_shell.cmd -mingw32"
        )
    return gpp


def main():
    ap = argparse.ArgumentParser(description="Builds EZ2CustomMod.dll directly with MinGW32 g++, without CMake.")
    ap.add_argument("--clean", action="store_true", help="delete build-mingw-direct-x86 before building")
    ap.add_argument("--debug", action="store_true", help="build with -g and no optimisation")
    args = ap.parse_args()

    gpp = ensure_mingw32()

    if args.clean and OBJDIR.exists():
        shutil.rmtree(OBJDIR)
    OBJDIR.mkdir(exist_ok=True)

    if OUT.exists():
        OUT.unlink()

    src_paths = [str(SRC / s) for s in SOURCES]
    missing = [s for s in src_paths if not Path(s).exists()]
    if missing:
        raise SystemExit("ERROR: missing sources:\n" + "\n".join(missing))

    flags = [
        gpp,
        "-shared",
        "-std=c++17",
        "-DWIN32_LEAN_AND_MEAN",
        "-DNOMINMAX",
        "-D_CRT_SECURE_NO_WARNINGS",
        "-Wall",
        "-Wextra",
        "-Wno-cast-function-type",
        "-Wno-unused-parameter",
        "-o", str(OUT),
    ]

    if args.debug:
        flags += ["-O0", "-g"]
    else:
        flags += ["-O2"]

    # Embed the MinGW runtimes without statically replacing Windows' native DLLs.
    link_flags = [
        "-static-libgcc",
        "-static-libstdc++",
        "-no-pthread",
        "-Wl,-Bstatic",
        "-Wl,--whole-archive",
        "-lwinpthread",
        "-Wl,--no-whole-archive",
        "-Wl,-Bdynamic",
        "-luser32",
        "-lkernel32",
        "-lshell32",
        "-lcomctl32",
        "-lgdi32",
        "-lgdiplus",
        "-lole32",
        # mfplat/mfreadwrite are NOT linked: XP has no Media Foundation and a static import would
        # block DLL load there — they are LoadLibrary'd at runtime (bms_scanner MediaFoundationAvailable).
        "-lmfuuid",  # static GUIDs only, no runtime DLL dependency
        "-Wl,--kill-at",
    ]

    run(flags + src_paths + link_flags)

    if not OUT.exists():
        raise SystemExit("ERROR: g++ finished, but EZ2CustomMod.dll is not there")

    print("\nOK: DLL built:")
    print(OUT)
    # Auto-deploy to the running game dir so we never run a stale DLL (the game loads EZ2AC_FNEX\EZ2CustomMod.dll).
    game = ROOT / "EZ2AC_FNEX"
    if game.is_dir():
        try:
            shutil.copy2(OUT, game / "EZ2CustomMod.dll")
            print("deployed ->", game / "EZ2CustomMod.dll")
        except Exception as e:
            print("WARN: could not deploy to EZ2AC_FNEX:", e)
    print("In 2EZConfig > Settings > Extra DLLs enter: EZ2CustomMod.dll")


if __name__ == "__main__":
    main()
