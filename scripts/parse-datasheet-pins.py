#!/usr/bin/env python3
"""Build a package pin table from the vendor datasheet's "管脚定义" table.

The CMSIS-SVD describes the **die** (it lists GPIOA..GPIOH for the whole 473
family), so it cannot say which pins a specific package bonds out. The datasheet
table 3-1 does: it lists one column per package (QFN32 / QFN48 / UQFN48 /
UQFN48-1 / LQFP48 / QFN60 / LQFP64 / LQFP80 / LQFP100 / LQFP128) with the pin
number per package, the pin name, its type and the alternate-function list.

This script extracts one package column into `data/packages/<part>_<pkg>.json`,
cross-checks the counts against the CMSIS-Pack `.pdsc`, and marks
`numberVerified: true` because the numbers now come from the datasheet itself.

Usage:
    python scripts/parse-datasheet-pins.py                       # UQFN48 / N32H473CE
    python scripts/parse-datasheet-pins.py --package LQFP48
    python scripts/parse-datasheet-pins.py --pdf <path> --part N32H473KE --package QFN32
"""

import argparse
import json
import pathlib
import re
import sys

try:
    import pymupdf
except ImportError:  # pragma: no cover
    print("需要 PyMuPDF: pip install pymupdf", file=sys.stderr)
    raise SystemExit(3)

ROOT = pathlib.Path(__file__).resolve().parent.parent
DEFAULT_PDF = pathlib.Path(r"C:/Users/yyg/Desktop/原理图/数据手册/N32H473.pdf")

#: Table 3-1 column order, left to right.
PACKAGE_COLUMNS = [
    "QFN32", "QFN48", "UQFN48", "UQFN48-1", "LQFP48",
    "QFN60", "LQFP64", "LQFP80", "LQFP100", "LQFP128",
]
COL_NAME, COL_TYPE, COL_IO, COL_FAILSAFE, COL_DEFAULT = 10, 11, 12, 13, 14
COL_AF = 15          # 复用功能 (默认)
COL_AF_REMAP = 16    # 复用功能 (重定义)

#: How this particular board wires the pin (CANable2-N32 firmware). Purely
#: informational: prefixed so it never looks like a datasheet alternate function.
BOARD_FUNCTIONS = {
    "PA0":  ["板级/CAN_STB"],
    "PA11": ["板级/USB_DM"],
    "PA12": ["板级/USB_DP"],
    "PA13": ["板级/SWDIO"],
    "PA14": ["板级/SWCLK"],
    "PA15": ["板级/LED_BLUE"],
    "PB11": ["板级/LED_GREEN"],
    "PB12": ["板级/FDCAN1_RX"],
    "PB13": ["板级/FDCAN1_TX"],
    "PC13": ["板级/CAN_PWR"],
}

#: Power / ground names seen on this family.
POWER_NAMES = {"VDD", "VDDA", "VBAT", "VREF+", "VREFS+"}
GROUND_NAMES = {"VSS", "VSSA"}


def normalize_text(cell: str) -> str:
    """Datasheet cells wrap mid-token and use '_' to mean an overbar."""
    if not cell:
        return ""
    lines = [line.strip() for line in cell.split("\n")]
    kept = [line for line in lines if line and set(line) - {"_", " "}]
    text = "".join(kept)
    text = re.sub(r"\s+", "_", text)
    return text.strip("_")


def clean_af(cell: str) -> list[str]:
    """Alternate functions: one per line, overbar-only lines dropped."""
    entries: list[str] = []
    for line in (cell or "").split("\n"):
        text = line.strip()
        if not text or not (set(text) - {"_", " "}):
            continue
        text = re.sub(r"\s+", "_", text)
        text = re.sub(r"_+", "_", text).strip("_")
        if text and text != "-":
            entries.append(text)
    return entries


def port_bit(name: str) -> tuple[str, int] | None:
    """'PA0-WKUP1' -> ('PA', 0);  'PH6-NRST' -> ('PH', 6)."""
    match = re.match(r"^P([A-H])(\d{1,2})", name)
    if not match:
        return None
    return f"P{match.group(1)}", int(match.group(2))


def classify(name: str, default: str, kind: str) -> str:
    upper = name.upper()
    if kind.upper() == "S" or upper in POWER_NAMES | GROUND_NAMES:
        return "ground" if upper in GROUND_NAMES else "power"
    if "NRST" in upper:
        return "reset"
    if "BOOT0" in upper:
        return "boot"
    if "OSC_IN" in upper or "OSC_OUT" in upper:
        # HSE / LSE pins: they are still GPIO, but they primarily carry the clock.
        return "clock"
    return "io"


def extract(pdf: pathlib.Path, package: str) -> list[dict]:
    column = PACKAGE_COLUMNS.index(package)
    document = pymupdf.open(pdf)

    rows: list[tuple[int, list]] = []
    for page_no in range(document.page_count):
        page = document[page_no]
        try:
            tables = page.find_tables()
        except Exception:  # pragma: no cover - defensive
            continue
        for table in tables.tables:
            for row in table.extract():
                if len(row) <= COL_DEFAULT:
                    continue
                cell = row[column]
                if not cell or not str(cell).strip().isdigit():
                    continue
                # Other tables on unrelated pages also have numeric columns; require
                # a pin name so only table 3-1 rows survive.
                name_cell = row[COL_NAME] or ""
                if not re.search(r"[A-Za-z]", name_cell):
                    continue
                rows.append((int(str(cell).strip()), row))

    # A row can straddle a page break and be extracted twice; keep the first.
    rows.sort(key=lambda item: item[0])
    deduped: list[tuple[int, list]] = []
    seen_numbers: set[int] = set()
    for number, row in rows:
        if number in seen_numbers:
            continue
        seen_numbers.add(number)
        deduped.append((number, row))
    rows = deduped

    pins: list[dict] = []
    for number, row in rows:
        raw_name = row[COL_NAME] or ""
        name = normalize_text(raw_name)
        default = normalize_text(row[COL_DEFAULT] or "")
        kind = (row[COL_TYPE] or "").strip()

        pin = {"number": number, "name": name, "type": classify(name, default, kind)}

        # Port/bit comes from the pin name: PH6-NRST is still port H bit 6 even
        # though its reset default is NRST rather than a GPIO.
        pb = port_bit(name) or port_bit(default)
        if pb and pin["type"] not in ("power", "ground"):
            pin["port"], pin["pin"] = pb

        functions: list[str] = []
        if default and default.upper() not in POWER_NAMES | GROUND_NAMES:
            functions.append(default)
        functions += clean_af(row[COL_AF] if len(row) > COL_AF else "")
        functions += clean_af(row[COL_AF_REMAP] if len(row) > COL_AF_REMAP else "")

        # Stable de-duplication, keeping the datasheet order.
        seen = set()
        unique = []
        for entry in functions:
            if entry in seen:
                continue
            seen.add(entry)
            unique.append(entry)

        board = BOARD_FUNCTIONS.get(pb[0].replace("P", "P") + str(pb[1]) if pb else "", [])
        if board:
            unique = board + unique
        if unique:
            pin["functions"] = unique
        pins.append(pin)

    return pins


def pdsc_expectation(part: str) -> dict | None:
    """Pin / IO counts the vendor pack declares for this part number.

    Several DFP versions may be installed (and older ones omit the feature tags
    entirely), so keep scanning and return the most complete entry.
    """
    best: dict | None = None
    for pdsc in pathlib.Path("C:/Keil_v5/ARM/PACK").rglob("*.pdsc"):
        try:
            text = pdsc.read_text(encoding="utf-8", errors="ignore")
        except OSError:
            continue
        if part not in text:
            continue
        match = re.search(rf'<device Dname="{re.escape(part)}"[^>]*>(.*?)</device>', text, re.S)
        if not match:
            continue
        body = match.group(1)
        info: dict = {"pdsc": pdsc.name}
        for kind in ("QFN", "QFP", "BGA"):
            feature = re.search(rf'<feature type="{kind}"\s+count="(\d+)"\s+name="([^"]+)"', body)
            if feature:
                info["pinCount"] = int(feature.group(1))
                info["package"] = feature.group(2)
                break
        ios = re.search(r'<feature type="IOs"\s+n="(\d+)"', body)
        if ios:
            info["ioCount"] = int(ios.group(1))
        if "pinCount" in info and "ioCount" in info:
            return info
        if best is None or len(info) > len(best):
            best = info
    return best


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--pdf", default=str(DEFAULT_PDF))
    parser.add_argument("--part", default="N32H473CE", help="part number for the JSON header")
    parser.add_argument("--package", default="UQFN48", choices=PACKAGE_COLUMNS)
    parser.add_argument("--chip", default="N32H473CEU7", help="chip name used in the file name")
    args = parser.parse_args()

    pdf = pathlib.Path(args.pdf)
    if not pdf.is_file():
        print(f"找不到数据手册: {pdf}", file=sys.stderr)
        return 1

    pins = extract(pdf, args.package)
    if not pins:
        print("没有从表格里提取到任何引脚", file=sys.stderr)
        return 1

    # ---- validation -------------------------------------------------------
    numbers = [p["number"] for p in pins]
    problems: list[str] = []

    expected_numbers = list(range(1, len(pins) + 1))
    if numbers != expected_numbers:
        problems.append(f"引脚号不连续: {numbers[:5]}...{numbers[-5:]}")

    io = [p for p in pins if p.get("port")]
    power = [p for p in pins if p["type"] in ("power", "ground")]
    ports: dict[str, list[int]] = {}
    for pin in io:
        ports.setdefault(pin["port"], []).append(pin["pin"])

    print(f"[table] {args.package}: {len(pins)} 脚 = GPIO {len(io)} + 电源/地 {len(power)}")
    for port in sorted(ports):
        bits = sorted(ports[port])
        print(f"        {port}: {len(bits):2d} 个 -> {bits}")

    expected = pdsc_expectation(args.part)
    if expected:
        print(f"[pdsc ] {args.part}: {expected.get('package')} / "
              f"{expected.get('pinCount')} 脚 / {expected.get('ioCount')} IO")
        if expected.get("pinCount") and len(pins) != expected["pinCount"]:
            problems.append(f"表格 {len(pins)} 脚，pdsc 声明 {expected['pinCount']} 脚")
        if expected.get("ioCount") and len(io) != expected["ioCount"]:
            problems.append(f"表格 {len(io)} 个 GPIO，pdsc 声明 {expected['ioCount']} 个")

    if problems:
        print("[FAIL] 数据自相矛盾:", file=sys.stderr)
        for problem in problems:
            print("   -", problem, file=sys.stderr)
        return 1

    package_name = expected.get("package", args.package) if expected else args.package
    if "/" in package_name:                       # "UQFN48/LQFP48"
        package_name = args.package

    document = {
        "name": package_name,
        "chip": args.chip,
        "numberVerified": True,
        "source": f"datasheet: {pdf.name} 表3-1 管脚定义 ({args.package} 列)",
        "expectedPinCount": len(pins),
        "expectedIoCount": len(io),
        "pinCountNote": (
            f"共 {len(pins)} 脚 = GPIO {len(io)} + 电源/地 {len(power)}；"
            f"端口仅 {'/'.join(sorted(ports))}。引脚号逐脚取自数据手册表3-1 的 {args.package} 列。"),
        "comment": (
            "由 scripts/parse-datasheet-pins.py 从厂商数据手册的管脚定义表直接提取："
            "引脚号、引脚名、类型、复用功能均来自手册，因此 numberVerified=true。"
            "functions 里带 '板级/' 前缀的条目来自 CANable2-N32 固件，不是手册内容；"
            "信号名的上划线在文本提取中会丢失，这里已去掉。"),
        "pins": pins,
    }

    out = ROOT / "data" / "packages" / f"{args.chip}_{package_name}.json"
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps(document, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(f"\n[OK  ] {out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
