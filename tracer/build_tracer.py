#!/usr/bin/env python3
# Build ez2tracer.dll (32-bit) with MinGW, then run its self-check.
# Usage: python tracer/build_tracer.py   (needs C:\msys64\mingw32\bin on PATH)
import os, subprocess, sys

here = os.path.dirname(os.path.abspath(__file__))
src  = os.path.join(here, "ez2tracer.cpp")
dll  = os.path.join(here, "ez2tracer.dll")
gpp  = "g++"

# self-check first (host build) — fails the build if the trace logic breaks
chk = os.path.join(here, "ez2tracer_selftest.exe")
subprocess.check_call([gpp, "-O2", "-DSELFTEST", "-o", chk, src])
out = subprocess.check_output([chk]).decode().strip()
print(out)
assert out == "selftest ok", "selftest failed"
os.remove(chk)

# the DLL: 32-bit, static libgcc/libstdc++ so it has no runtime deps next to the game
subprocess.check_call([gpp, "-O2", "-shared", "-m32", "-static",
                       "-o", dll, src, "-lkernel32"])
print("OK:", dll)
