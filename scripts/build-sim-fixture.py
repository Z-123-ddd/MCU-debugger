#!/usr/bin/env python3
"""Build the FMU used by the simulation self-test.

    python scripts/build-sim-fixture.py

Result: data/test/sim_lag.fmu

The fixture is a *real* FMI 2.0 Co-Simulation package: a mingw-built DLL exporting the
standard entry points plus a modelDescription, zipped the way every FMU tool does it. That
is what makes the FMU test meaningful -- a hand-written mock would only prove our code
agrees with itself.

Committed as a binary fixture for the same reason data/test/symbol_test.axf is: the test
has to run without a compiler, and an FMU is a binary format.
"""

from __future__ import annotations

import os
import shutil
import subprocess
import sys
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
SRC_DIR = os.path.join(HERE, "sim_fixture")
OUT = os.path.join(ROOT, "data", "test", "sim_lag.fmu")

# The toolchain the project is built with; override with MRD_CC if needed.
DEFAULT_CC = r"D:/Qt/Tools/mingw1310_64/bin/gcc.exe"


def find_compiler() -> str:
    candidate = os.environ.get("MRD_CC", DEFAULT_CC)
    if os.path.exists(candidate):
        return candidate
    for name in ("gcc", "cc", "clang"):
        found = shutil.which(name)
        if found:
            return found
    sys.exit("[ERROR] no C compiler found; set MRD_CC")


def main() -> None:
    cc = find_compiler()
    print(f"[INFO] compiler: {cc}")

    tmp = os.path.join(ROOT, ".sim_fixture_build")
    if os.path.isdir(tmp):
        shutil.rmtree(tmp)
    os.makedirs(os.path.join(tmp, "binaries", "win64"), exist_ok=True)

    dll = os.path.join(tmp, "binaries", "win64", "testlag.dll")
    cmd = [
        cc,
        "-shared",
        "-O2",
        "-o",
        dll,
        os.path.join(SRC_DIR, "testlag.c"),
    ]
    print("[INFO] " + " ".join(cmd))
    result = subprocess.run(cmd, capture_output=True, text=True)
    if result.returncode != 0:
        print(result.stdout)
        print(result.stderr)
        sys.exit("[ERROR] compile failed")
    print(f"[INFO] dll: {os.path.getsize(dll)} bytes")

    shutil.copy(os.path.join(SRC_DIR, "modelDescription.xml"),
                os.path.join(tmp, "modelDescription.xml"))

    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    if os.path.exists(OUT):
        os.remove(OUT)

    # A .fmu is an ordinary ZIP with modelDescription.xml at the root.
    with zipfile.ZipFile(OUT, "w", zipfile.ZIP_DEFLATED) as archive:
        for base, _dirs, files in os.walk(tmp):
            for name in files:
                full = os.path.join(base, name)
                rel = os.path.relpath(full, tmp).replace(os.sep, "/")
                archive.write(full, rel)
                print(f"[INFO] + {rel}")

    shutil.rmtree(tmp)
    print(f"[OK] {OUT} ({os.path.getsize(OUT)} bytes)")


if __name__ == "__main__":
    main()
