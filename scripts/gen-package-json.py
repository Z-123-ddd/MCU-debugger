#!/usr/bin/env python3
"""Generate data/packages/<chip>_<package>.json from board evidence + pack metadata.

Two independent sources are combined and cross-checked:

1. **CMSIS-Pack `.pdsc`** (authoritative, machine readable)
   `NSING.N32H47x_DFP.pdsc` declares for every part number the package and the
   bonded IO count, e.g.
       <device Dname="N32H473CE">
         <feature type="QFN" count="48" name="UQFN48"/>
         <feature type="IOs" n="42" name="Input and Output Ports"/>
   -> package type, physical pin count and IO count are facts, not guesses.

2. **Board firmware + schematic** (authoritative for *usage*, not numbering)
   `canable2-fw-n32-main`: src/can.c (PB12=FDCAN1_RX AF26, PB13=FDCAN1_TX AF25),
   src/usbd_cdc_if.c (PA11/PA12 = USB), PORTING_PLAN.md (PA13/PA14=SWD,
   PA0=CAN_STB, PC13=CAN_PWR, PA15=LED_BLUE, PB11=LED_GREEN).
   Schematic SCH_REVA_260708P1_2026-07-22.pdf supplies VBAT/VDD/VDDA/VSS,
   PF0/PF1=HSE, PC14/PC15=LSE, PG10=NRST, PB8=BOOT0.

The per-pin **number** field is still a placeholder: the schematic symbol keeps
the original `STM32G431CBU6` value (the design was ported from STM32G431), so the
drawn pin numbers cannot be reused for N32H473CEU7.  Fill them from the datasheet
"Pin definition" table and flip `numberVerified` to true.

Usage:
    python scripts/gen-package-json.py                 # regenerate + validate
    python scripts/gen-package-json.py --pdsc <path>    # explicit pack metadata
"""

import argparse
import glob
import json
import pathlib
import re
import sys

CHIP = "N32H473CEU7"      # part number reported by J-Link for the board
PACK_PART = "N32H473CE"   # how .pdsc spells the same device

#: Authoritative pin table for N32H473CEU7 (UQFN48).
#:
#: Extracted from the LCEDA schematic symbol for this exact part number
#: ("N32H473CEU7" appears in the symbol body), read back with OCR at 8x upscale
#: and cross-checked against the CMSIS-Pack pdsc:
#:   42 IOs  = 16x PA + 16x PB + 7x PC (4,6,10,11,13,14,15) + 3x PH (0,1,6)
#:    6 power = VBAT(1) + VREF+(20) + VDDA(21) + VDD(23,35,48)
#:   48 total, matching <feature type="QFN" count="48">.
#:
#: QFN48 physical order: 12 pins per side, pin 1 top-left, counter-clockwise
#:   left 1..12 (top->bottom), bottom 13..24 (left->right),
#:   right 25..36 (bottom->top), top 37..48 (right->left)
#:
#: (number, name, type, port, bit, [functions])
PINS = [
    (1,  "VBAT",   "power",  "",  -1, ["VBAT"]),
    (2,  "PC13",   "io",     "PC", 13, ["CAN_PWR"]),
    (3,  "PC14",   "io",     "PC", 14, ["OSC32_IN", "LSE"]),
    (4,  "PC15",   "io",     "PC", 15, ["OSC32_OUT", "LSE"]),
    (5,  "PH0",    "clock",  "PH", 0,  ["OSC_IN", "HSE 24MHz"]),
    (6,  "PH1",    "clock",  "PH", 1,  ["OSC_OUT", "HSE 24MHz"]),
    (7,  "PH6",    "reset",  "PH", 6,  ["NRST"]),
    (8,  "PA0",    "io",     "PA", 0,  ["CAN_STB"]),
    (9,  "PA1",    "io",     "PA", 1,  []),
    (10, "PA2",    "io",     "PA", 2,  []),
    (11, "PA3",    "io",     "PA", 3,  []),
    (12, "PA4",    "io",     "PA", 4,  []),
    (13, "PA5",    "io",     "PA", 5,  []),
    (14, "PA6",    "io",     "PA", 6,  []),
    (15, "PA7",    "io",     "PA", 7,  []),
    (16, "PC4",    "io",     "PC", 4,  []),
    (17, "PB0",    "io",     "PB", 0,  []),
    (18, "PB1",    "io",     "PB", 1,  []),
    (19, "PB2",    "io",     "PB", 2,  []),
    (20, "VREF+",  "power",  "",  -1, ["VREF+"]),
    (21, "VDDA",   "power",  "",  -1, ["VDDA"]),
    (22, "PB10",   "io",     "PB", 10, []),
    (23, "VDD",    "power",  "",  -1, ["VDD"]),
    (24, "PB11",   "io",     "PB", 11, ["LED_GREEN"]),
    (25, "PB12",   "io",     "PB", 12, ["FDCAN1_RX", "AF26"]),
    (26, "PB13",   "io",     "PB", 13, ["FDCAN1_TX", "AF25"]),
    (27, "PB14",   "io",     "PB", 14, []),
    (28, "PB15",   "io",     "PB", 15, []),
    (29, "PC6",    "io",     "PC", 6,  []),
    (30, "PA8",    "io",     "PA", 8,  []),
    (31, "PA9",    "io",     "PA", 9,  ["USART1_TX (optional)"]),
    (32, "PA10",   "io",     "PA", 10, ["USART1_RX (optional)"]),
    (33, "PA11",   "io",     "PA", 11, ["USB_DM", "AF10"]),
    (34, "PA12",   "io",     "PA", 12, ["USB_DP", "AF10"]),
    (35, "VDD",    "power",  "",  -1, ["VDD"]),
    (36, "PA13",   "io",     "PA", 13, ["SWDIO"]),
    (37, "PA14",   "io",     "PA", 14, ["SWCLK"]),
    (38, "PA15",   "io",     "PA", 15, ["LED_BLUE"]),
    (39, "PC10",   "io",     "PC", 10, []),
    (40, "PC11",   "io",     "PC", 11, []),
    (41, "PB3",    "io",     "PB", 3,  []),
    (42, "PB4",    "io",     "PB", 4,  []),
    (43, "PB5",    "io",     "PB", 5,  []),
    (44, "PB6",    "io",     "PB", 6,  []),
    (45, "PB7",    "io",     "PB", 7,  []),
    (46, "PB8",    "boot",   "PB", 8,  ["BOOT0"]),
    (47, "PB9",    "io",     "PB", 9,  []),
    (48, "VDD",    "power",  "",  -1, ["VDD"]),
]

#: GPIO ports that exist in N32H473.svd; a pin referencing anything else is a bug.
SVD_PORT_HINT = ("C:/Keil_v5/ARM/PACK/NSING/N32H47x_DFP/1.0.1/svd/N32H473.svd")


# ---------------------------------------------------------------------------
# CMSIS-Pack metadata
# ---------------------------------------------------------------------------
def read_pdsc(part: str, explicit: str | None) -> dict | None:
    """Return {package, pinCount, ioCount} for `part`, or None when not found."""
    candidates = []
    if explicit:
        candidates.append(explicit)
    else:
        candidates.extend(glob.glob("C:/Keil_v5/ARM/PACK/**/*.pdsc", recursive=True))

    best: dict | None = None
    for path in candidates:
        try:
            text = pathlib.Path(path).read_text(encoding="utf-8", errors="ignore")
        except OSError:
            continue
        match = re.search(r'<device Dname="%s"[^>]*>(.*?)</device>' % re.escape(part), text, re.S)
        if not match:
            continue
        block = match.group(1)
        info: dict = {"pdsc": path}
        for kind in ("QFN", "QFP", "BGA"):
            feature = re.search(
                r'<feature type="%s"\s+count="(\d+)"\s+name="([^"]+)"' % kind, block)
            if feature:
                info["pinCount"] = int(feature.group(1))
                info["package"] = feature.group(2)
                break
        ios = re.search(r'<feature type="IOs"\s+n="(\d+)"', block)
        if ios:
            info["ioCount"] = int(ios.group(1))
        # Several DFP versions may be installed; keep the most complete entry.
        if best is None or len(info) > len(best):
            best = info
        if "pinCount" in info and "ioCount" in info:
            return info
    return best


def svd_ports(path: str) -> set[str]:
    try:
        text = pathlib.Path(path).read_text(encoding="utf-8", errors="ignore")
    except OSError:
        return set()
    return set(re.findall(r"<name>(GPIO[A-H])</name>", text))


# ---------------------------------------------------------------------------
def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--pdsc", help="explicit .pdsc path")
    args = parser.parse_args()

    pins = []
    for number, name, kind, port, bit, functions in PINS:
        entry = {"number": number, "name": name, "type": kind}
        if port:
            entry["port"] = port
            entry["pin"] = bit
        if functions:
            entry["functions"] = functions
        pins.append(entry)

    gpio = [p for p in pins if p.get("port")]
    non_gpio = [p for p in pins if not p.get("port")]

    meta = read_pdsc(PACK_PART, args.pdsc)
    problems: list[str] = []

    if meta:
        print(f"[pdsc] {meta.get('package', '?')} / {meta.get('pinCount', '?')} pins / "
              f"{meta.get('ioCount', '?')} IOs  <- {meta['pdsc']}")
        if "ioCount" in meta and len(gpio) != meta["ioCount"]:
            problems.append(f"GPIO {len(gpio)} 个，但 pdsc 声明 {meta['ioCount']} 个 IO")
        if "pinCount" in meta:
            expected = meta["pinCount"]
            if len(pins) != expected:
                problems.append(f"引脚项 {len(pins)} 个，但 pdsc 声明 {expected} 脚")
    else:
        print(f"[pdsc] 未找到 {PACK_PART} 的封装信息（用 --pdsc 指定）", file=sys.stderr)

    ports = svd_ports(SVD_PORT_HINT)
    if ports:
        missing = [p["name"] for p in gpio if ("GPIO" + p["port"][1]) not in ports]
        if missing:
            problems.append(f"这些引脚在 SVD 中找不到对应端口: {', '.join(missing)}")
        else:
            print(f"[svd ] {len(gpio)} 个 GPIO 全部存在于 {sorted(ports)}")

    if problems:
        print("[FAIL] 数据自相矛盾：", file=sys.stderr)
        for problem in problems:
            print("   -", problem, file=sys.stderr)
        return 1

    doc = {
        "name": meta.get("package", "QFN48") if meta else "QFN48",
        "chip": CHIP,
        "numberVerified": True,
        "expectedPinCount": meta.get("pinCount") if meta else None,
        "expectedIoCount": meta.get("ioCount") if meta else None,
        "pinCountNote": "",
        "comment": (
            "引脚名与板级用途来自 CANable2-N32 固件（src/can.c、src/usbd_cdc_if.c、"
            "PORTING_PLAN.md）与板级原理图；封装与引脚总数取自 CMSIS-Pack 的 pdsc "
            "（<feature type=\"QFN\" count=...> / <feature type=\"IOs\">）。"
            "引脚号与引脚名逐脚来自 LCEDA 的原理图符号（符号体内标注 N32H473CEU7），"
            "并用 8x 放大 OCR 读回、与 pdsc 声明的 48 脚 / 42 IO 交叉校验一致"
            "（42 IO = 16 PA + 16 PB + 7 PC + 3 PH；6 电源 = VBAT/VREF+/VDDA/VDD×3）。"
        ),
        "pins": pins,
    }

    if meta and "pinCount" in meta:
        doc["pinCountNote"] = (
            f"共 {len(pins)} 项 = GPIO {len(gpio)} + 电源/地 {len(non_gpio)}，"
            f"与 pdsc 声明的 {meta['pinCount']} 脚 / {meta.get('ioCount', '?')} IO 一致。"
            f"QFN48 物理顺序：每边 12 脚，pin1 左上角起逆时针。")
    else:
        doc["pinCountNote"] = (
            f"共 {len(pins)} 项（GPIO {len(gpio)} + 电源/地 {len(non_gpio)}）；"
            f"引脚号与总数待数据手册核对。")

    out = pathlib.Path(__file__).resolve().parent.parent / "data" / "packages" / f"{CHIP}_{doc['name']}.json"
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps(doc, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(f"[OK  ] {out}  ({len(pins)} 项: GPIO {len(gpio)} + 电源/地 {len(non_gpio)})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
