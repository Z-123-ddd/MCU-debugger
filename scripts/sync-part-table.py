#!/usr/bin/env python3
"""Generate data/parts.json from every CMSIS-Pack `.pdsc` in the workspace.

Why a generated table
---------------------
`data/device_ids.json` is keyed by DBGMCU DEV_ID and only carries *families*
(`N32H473`), because one DEV_ID is shared by every package/flash variant. But the
vendor pack describes each **exact part** (`N32H473CE`) individually:

    <device Dname="N32H473CE">
      <description>
        - Package:UQFN48/LQFP48
        - Flash:  512KB
        - SRAM:   160KB
        - CCM SRAM: 32KB
        - I/O:    42
        - SPI/I2S:5/2
        ...
      </description>
      <memory id="IRAM1" start="0x20000000" size="0x30000"/>
      <memory id="IROM1" start="0x08000000" size="0x80000"/>
      <debug svd="svd/N32H473.svd"/>
    </device>

That is the authoritative source for "how much flash/RAM" and "how many SPIs".
This script flattens all 176 parts into one file the app can load at runtime
(`data/` is shipped, `svd/` is not committed).

Usage
-----
    python scripts/sync-part-table.py             # regenerate data/parts.json
    python scripts/sync-part-table.py --dry-run   # report only
"""

from __future__ import annotations

import argparse
import glob
import json
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
OUT = ROOT / "data" / "parts.json"

#: pdsc label -> SVD instance prefix. Labels differ between pack generations
#: ("CAN" on G4x vs "FDCAN" on H4x; "SPI/I2S" is spelled with a slash).
LABEL_TO_CLASS = {
    "SPI/I2S": "SPI",
    "SPI": "SPI",
    "I2C": "I2C",
    "USART": "USART",
    "UART": "UART",
    "LPUART": "LPUART",
    "CAN": "CAN",
    "FDCAN": "FDCAN",
    "DMA": "DMA",
    "LPTIM": "LPTIM",
    "xSPI": "xSPI",
    "QSPI": "QSPI",
    "SPI": "SPI",
    "ADC": "ADC",
    "DAC": "DAC",
    "COMP": "COMP",
    "OPAMP": "OPAMP",
    "SDIO": "SDIO",
    "ETH": "ETH",
    "DVP": "DVP",
    "TSC": "TSC",
}

#: Memory rows are counted, not compared as instances, so they are kept as raw
#: text for the UI only.
MEMORY_LABELS = {"Flash", "SRAM", "CCM SRAM", "Backup SRAM"}


def candidate_pdsc() -> list[str]:
    found = [str(p) for p in (ROOT / "svd").glob("**/*.pdsc")]
    found.extend(glob.glob("C:/Keil_v5/ARM/PACK/**/*.pdsc", recursive=True))
    return sorted(set(found))


def parse_description(block: str) -> dict[str, str]:
    desc = re.search(r"<description>(.*?)</description>", block, re.S)
    if not desc:
        return {}
    text = desc.group(1).replace("&#xA;", "\n").replace("&#xD;", "")
    fields: dict[str, str] = {}
    for raw in text.split("\n"):
        line = raw.strip().lstrip("-").strip()
        if ":" not in line:
            continue
        label, value = line.split(":", 1)
        fields[label.strip()] = value.strip()
    return fields


def parse_memory(block: str) -> dict[str, dict]:
    out: dict[str, dict] = {}
    for match in re.finditer(r"<memory\s+([^/>]+)/>", block):
        attrs = dict(re.findall(r'(\w+)="([^"]*)"', match.group(1)))
        ident = attrs.get("id", "")
        if not ident:
            continue
        try:
            size = int(attrs.get("size", "0"), 0)
        except ValueError:
            continue
        out[ident] = {"start": attrs.get("start", ""), "size": size}
    return out


def parse_svd(block: str) -> str:
    match = re.search(r'<debug[^>]*\bsvd="([^"]+)"', block)
    return match.group(1) if match else ""


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--dry-run", action="store_true")
    args = parser.parse_args()

    paths = candidate_pdsc()
    if not paths:
        print("[ERROR] no .pdsc found (run scripts/fetch-n32-svds.py first)", file=sys.stderr)
        return 1

    parts: dict[str, dict] = {}
    for path in paths:
        try:
            text = pathlib.Path(path).read_text(encoding="utf-8", errors="ignore")
        except OSError:
            continue
        pack = pathlib.Path(path).parent.name
        for match in re.finditer(r'<device\s+Dname="([^"]+)"[^>]*>(.*?)</device>', text, re.S):
            name, block = match.group(1), match.group(2)
            fields = parse_description(block)
            memory = parse_memory(block)

            entry: dict = {"pack": pack}
            svd = parse_svd(block)
            if svd:
                entry["svd"] = svd

            # RAM/Flash come from the linker memory rows: that is what the part
            # actually has, independent of how the prose describes it.
            if "IROM1" in memory:
                entry["flashBytes"] = memory["IROM1"]["size"]
            if "IRAM1" in memory:
                entry["ramBytes"] = memory["IRAM1"]["size"]

            # Prose breakdown (only where the pack provides it).
            if fields.get("SRAM"):
                entry["sram"] = fields["SRAM"]
            if fields.get("CCM SRAM"):
                entry["ccmSram"] = fields["CCM SRAM"]
            if fields.get("Backup SRAM"):
                entry["backupSram"] = fields["Backup SRAM"]
            if fields.get("Flash"):
                entry["flash"] = fields["Flash"]
            if fields.get("Package"):
                entry["package"] = fields["Package"]
            if fields.get("I/O"):
                try:
                    entry["ioCount"] = int(fields["I/O"])
                except ValueError:
                    pass
            if fields.get("Clock"):
                entry["clock"] = fields["Clock"]

            caps: dict[str, int] = {}
            for label, cls in LABEL_TO_CLASS.items():
                if label not in fields:
                    continue
                first = re.split(r"[/\s]", fields[label])[0]
                if first in ("-", ""):
                    caps[cls] = 0
                    continue
                try:
                    caps[cls] = int(first)
                except ValueError:
                    continue
            if caps:
                entry["capabilities"] = dict(sorted(caps.items()))

            parts[name] = entry

    if not parts:
        print("[ERROR] no <device> entries parsed", file=sys.stderr)
        return 1

    with_mem = sum(1 for v in parts.values() if "flashBytes" in v and "ramBytes" in v)
    with_caps = sum(1 for v in parts.values() if v.get("capabilities"))
    print(f"[OK] {len(parts)} parts from {len(paths)} packs "
          f"(flash+ram: {with_mem}, capabilities: {with_caps})")

    sample = parts.get("N32H473CE")
    if sample:
        print(f"      N32H473CE: flash={sample.get('flashBytes')} ram={sample.get('ramBytes')} "
              f"caps={sample.get('capabilities')}")

    if args.dry_run:
        print(f"[dry-run] would write {OUT}")
        return 0

    OUT.write_text(json.dumps({"parts": dict(sorted(parts.items()))},
                              ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(f"[OK] wrote {OUT}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
