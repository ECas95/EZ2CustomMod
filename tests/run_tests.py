#!/usr/bin/env python3
# Builds and runs every standalone test: each tests/*_test.cpp is its own executable, each
# tests/*_test.py its own script. Needs MSYS2 MINGW32's g++ in PATH (see docs/DEVELOPMENT.md).
#
#   py -3 tests/run_tests.py            all of them
#   py -3 tests/run_tests.py lane       only the ones whose name contains "lane"
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
TESTS = ROOT / "tests"
# Tests that compile a source file of the DLL alongside themselves.
EXTRA_SOURCES = {"audio_decode_test": ["src/audio_decode.cpp", "-static"]}
LIBS = ["-lgdi32", "-lgdiplus", "-lole32"]


def run(cmd, cwd):
    p = subprocess.run(cmd, cwd=str(cwd), capture_output=True, text=True, errors="replace")
    return p.returncode, (p.stdout + p.stderr).strip()


def main():
    only = sys.argv[1] if len(sys.argv) > 1 else ""
    failed = []
    total = 0
    with tempfile.TemporaryDirectory() as out:
        for src in sorted(TESTS.glob("*_test.cpp")):
            if only not in src.stem:
                continue
            total += 1
            exe = Path(out) / (src.stem + ".exe")
            cmd = ["g++", "-std=c++17", "-O1", "-Wall", "-Wextra", "-o", str(exe), str(src)]
            cmd += EXTRA_SOURCES.get(src.stem, []) + LIBS
            code, log = run(cmd, ROOT)
            if code == 0:
                code, log = run([str(exe)], out)
            print(("ok    " if code == 0 else "FAIL  ") + src.name)
            if code != 0:
                failed.append((src.name, log))
        for src in sorted(TESTS.glob("*_test.py")):
            if only not in src.stem:
                continue
            total += 1
            code, log = run([sys.executable, str(src)], ROOT)
            print(("ok    " if code == 0 else "FAIL  ") + src.name)
            if code != 0:
                failed.append((src.name, log))
    for name, log in failed:
        print("\n---- " + name + "\n" + "\n".join(log.splitlines()[-8:]))
    print("\n%d/%d passed" % (total - len(failed), total))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
