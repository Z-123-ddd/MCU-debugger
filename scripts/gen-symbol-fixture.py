#!/usr/bin/env python3
"""Rebuild the ELF fixture used by `--selftest` to exercise the DWARF reader.

Why a checked-in fixture instead of generating it during the test
---------------------------------------------------------------
The point of the fixture is to prove that variable *types* survive the round trip:
a plot has to know that `g_temperature` is a 4-byte IEEE-754 float and that
`g_adc_raw` is a 4-element uint16_t array. Those facts come from DWARF, so the test
needs a real compiler-produced ELF. Committing the 8 KB result keeps the self-test
runnable on a machine with no ARM toolchain installed.

`data/test/symbol_test.c` is the source; this script turns it into
`data/test/symbol_test.axf`.

The expected load addresses below are asserted by the self-test, so if you change
the source or the compiler version, re-run this and update Cli.cpp's expectations
(`arm-none-eabi-nm -S` prints them).

Usage
-----
    python scripts/gen-symbol-fixture.py [--toolchain-dir DIR]
"""

from __future__ import annotations

import argparse
import os
import pathlib
import shutil
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
# Two translation units: a single-CU fixture cannot catch "only the first unit was
# read" or "a forward declaration was not followed to its definition".
SOURCES = [
    ROOT / "data" / "test" / "symbol_test.c",
    ROOT / "data" / "test" / "symbol_test_b.c",
]
OUTPUT = ROOT / "data" / "test" / "symbol_test.axf"

# Preferred order: an on-PATH cross toolchain, then the workspace-local one the
# other scripts fetch.
SEARCH_DIRS = [
    None,  # PATH
    pathlib.Path.home() / "tools" / "arm-gnu-toolchain-13.3.rel1-mingw-w64-i686-arm-none-eabi" / "bin",
]


def find_gcc(extra: str | None) -> str | None:
    names = ["arm-none-eabi-gcc.exe", "arm-none-eabi-gcc"]
    dirs = ([pathlib.Path(extra)] if extra else []) + [d for d in SEARCH_DIRS if d]
    for directory in dirs:
        for name in names:
            candidate = (pathlib.Path(directory) / name) if directory else None
            if candidate and candidate.exists():
                return str(candidate)
    # Fall back to PATH lookup.
    for name in names:
        found = shutil.which(name)
        if found:
            return found
    return None


def main() -> int:
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--toolchain-dir", help="directory holding arm-none-eabi-gcc")
    args = parser.parse_args()

    missing = [str(source) for source in SOURCES if not source.exists()]
    if missing:
        print("[ERROR] source missing: " + ", ".join(missing), file=sys.stderr)
        return 1

    gcc = find_gcc(args.toolchain_dir)
    if not gcc:
        print("[ERROR] arm-none-eabi-gcc not found.\n"
              "        Install the GNU Arm toolchain, or point --toolchain-dir at its bin/.",
              file=sys.stderr)
        return 1

    # -gdwarf-4 keeps the fixture readable by the oldest DWARF path in the reader.
    # -O0 prevents the optimiser from dropping the globals we want to assert on.
    command = [
        gcc,
        "-mcpu=cortex-m4", "-mthumb",
        "-g", "-gdwarf-4", "-O0",
        "-nostdlib", "-nostartfiles",
        "-Wl,-Ttext=0x08000000", "-Wl,-e,main",
        "-o", str(OUTPUT),
    ] + [str(source) for source in SOURCES]
    print("[..] " + " ".join(command))
    result = subprocess.run(command, capture_output=True, text=True)
    if result.returncode != 0:
        print("[ERROR] compile failed:\n" + (result.stderr or result.stdout), file=sys.stderr)
        return 1

    size = OUTPUT.stat().st_size
    print(f"[OK] wrote {OUTPUT} ({size} bytes)")

    nm = shutil.which("arm-none-eabi-nm") or str(
        pathlib.Path(gcc).parent / "arm-none-eabi-nm")
    if pathlib.Path(nm).exists():
        listing = subprocess.run([nm, "-S", "--defined-only", str(OUTPUT)],
                                 capture_output=True, text=True)
        rows = [line for line in listing.stdout.splitlines() if " g_" in line]
        if rows:
            print("[OK] variables the self-test expects (name/address/size):")
            for row in rows:
                print("     " + row.strip())
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
