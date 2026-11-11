#!/usr/bin/env python3
"""Build package pin tables from the NSING family datasheets.

What this produces
------------------
`data/packages/<FAMILY>_<PACKAGE>.json` — one file per (family, package), e.g.
`N32H473_LQFP48.json`. Every part that shares that package reuses the same table
(N32H473CC / N32H473CE / N32H473CG are all LQFP48), so this covers the whole
catalogue with a few dozen files instead of ~680 copies.

Why the datasheet
-----------------
The pin-definition table (chapter 3) is the only source that maps "pin 12 of
LQFP48" to "PA4". The SVD is die-level, the .pdsc only has counts, and schematic
symbols are unreliable (the STM32G431 residue in the LCEDA symbol proved that).

Layout auto-discovery
--------------------
Every family words and shapes the table differently:

    N32H487  10 columns    N32G455  14 columns
    N32H473  17 columns    N32G401  18 columns    N32G432  21 columns
    N32G033  English header "Package / Pin name (function ..) / Type / I/O .."

so nothing is hard-coded: the pin-name column is found by its header wording and
the package columns by matching package names in the header rows. Only fields we
can locate confidently are emitted; alternate-function lists are best effort.

Safety
------
A pin table is only written when it *validates*:

  * pin numbers form exactly 1..N (no duplicates, no gaps), and
  * N matches the package's pin count in the CMSIS-Pack `.pdsc`.

Anything that fails is reported and skipped, leaving that part on the generic
`synthesize()` fallback (which draws the right outline from the known counts and
labels pins generically) rather than shipping invented pin names.

Usage
-----
    python scripts/build-package-tables.py                 # all datasheets found
    python scripts/build-package-tables.py --family N32H473
    python scripts/build-package-tables.py --dry-run       # report, write nothing
"""

from __future__ import annotations

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
DATASHEETS = ROOT / "data" / "datasheets"
PACKAGES = ROOT / "data" / "packages"
PARTS = ROOT / "data" / "parts.json"

#: Package name patterns used in the header rows.
PACKAGE_RE = re.compile(r"\b(U?QFN|UFQFPN|LQFP|TSSOP|QFN|BGA|LGA)\s*-?\s*(\d+)(-\d+)?\b", re.I)

#: Header wording for the pin-name column, across pack generations.
NAME_HEADER_RE = re.compile(r"(管脚名称|引脚名称|引脚定义|Pin\s*name)", re.I)
#: Header wording for "default function" and "alternate function" columns.
DEFAULT_HEADER_RE = re.compile(r"(主功能|默认功能|Default)", re.I)
AF_HEADER_RE = re.compile(r"(复用功能|重定义|Alternate)", re.I)

POWER_NAMES = {"VDD", "VDDA", "VBAT", "VREF+", "VREFS+", "VDDIO", "VDD33"}
GROUND_NAMES = {"VSS", "VSSA", "VSSIO"}


# ---------------------------------------------------------------------------
# cell cleaning
# ---------------------------------------------------------------------------
def cell_text(cell) -> str:
    return (str(cell) if cell is not None else "").strip()


def normalize_name(cell: str) -> str:
    """Datasheet cells wrap mid-token and use '_' for an overbar."""
    if not cell:
        return ""
    lines = [ln.strip() for ln in cell.split("\n")]
    kept = [ln for ln in lines if ln and set(ln) - {"_", " "}]
    text = "".join(kept)
    text = re.sub(r"\s+", "_", text)
    return text.strip("_")


def clean_af(cell: str) -> list[str]:
    out: list[str] = []
    for line in (cell or "").split("\n"):
        text = line.strip()
        if not text or not (set(text) - {"_", " "}):
            continue
        text = re.sub(r"\s+", "_", text)
        text = re.sub(r"_+", "_", text).strip("_")
        if text and text != "-":
            out.append(text)
    return out


def port_bit(name: str) -> tuple[str, int] | None:
    match = re.match(r"^P([A-H])(\d{1,2})", name)
    if not match:
        return None
    return f"P{match.group(1)}", int(match.group(2))


def classify(name: str, kind: str) -> str:
    upper = name.upper()
    if upper in GROUND_NAMES or re.match(r"^VSS", upper):
        return "ground"
    if upper in POWER_NAMES or re.match(r"^(VDD|VDDA|VBAT|VREF)", upper):
        return "power"
    if kind.upper().startswith("S") and "I/O" not in kind.upper():
        return "power"
    if "NRST" in upper:
        return "reset"
    if "BOOT0" in upper:
        return "boot"
    if "OSC_IN" in upper or "OSC_OUT" in upper:
        return "clock"
    return "io"


# ---------------------------------------------------------------------------
# table discovery
# ---------------------------------------------------------------------------
#: Cheap text pre-filter to find where table 3-1 *starts*. Only the first page of
#: the table repeats the "管脚名称 / Pin name" heading; continuation pages do not,
#: so once a start page is found we keep scanning forward for a fixed window.
CANDIDATE_RE = re.compile(r"(管脚|引脚|Pin\s*name)", re.I)

CONTINUATION_PAGES = 14


def find_pin_tables(pdf: pathlib.Path, max_page: int = 90):
    """Yield (page_no, rows, layout) for every pin-definition table fragment."""
    document = pymupdf.open(pdf)
    limit = min(document.page_count, max_page)

    starts: list[int] = []
    for page_no in range(limit):
        try:
            text = document[page_no].get_text()
        except Exception:
            continue
        if CANDIDATE_RE.search(text):
            starts.append(page_no)
    if not starts:
        return

    # Scan each start page and its continuation window, without duplicates.
    pages: list[int] = []
    for start_page in starts:
        for offset in range(CONTINUATION_PAGES + 1):
            page_no = start_page + offset
            if page_no < limit and page_no not in pages:
                pages.append(page_no)

    for page_no in pages:
        try:
            tables = document[page_no].find_tables().tables
        except Exception:
            continue
        for table in tables:
            try:
                rows = table.extract()
            except Exception:
                continue
            if len(rows) < 5:
                continue
            layout = discover_layout(rows)
            if layout:
                yield page_no + 1, rows, layout


def discover_layout(rows) -> dict | None:
    """Locate the pin-name column and the package columns from the header rows."""
    header_rows = rows[:3]
    name_col = None
    for row in header_rows:
        for index, cell in enumerate(row):
            if NAME_HEADER_RE.search(cell_text(cell)):
                name_col = index
                break
        if name_col is not None:
            break
    if name_col is None:
        return None

    packages: dict[int, str] = {}
    for row in header_rows:
        for index, cell in enumerate(row):
            text = cell_text(cell).replace("\n", " ")
            match = PACKAGE_RE.search(text)
            if not match:
                continue
            canonical = (match.group(1) + match.group(2)
                         + (match.group(3) or "")).upper().replace(" ", "")
            packages[index] = canonical
    if not packages:
        return None

    # Default / alternate function columns, if the header names them.
    default_col = None
    af_cols: list[int] = []
    for row in header_rows:
        for index, cell in enumerate(row):
            text = cell_text(cell).replace("\n", " ")
            if default_col is None and DEFAULT_HEADER_RE.search(text):
                default_col = index
            elif AF_HEADER_RE.search(text) and index not in af_cols:
                af_cols.append(index)

    return {
        "name_col": name_col,
        "packages": packages,
        "default_col": default_col,
        "af_cols": af_cols,
    }


def extract_package(rows, layout: dict, package: str) -> list[dict]:
    """Rows belonging to one package column -> pin dicts (numbers 1..N)."""
    column = None
    for index, name in layout["packages"].items():
        if name == package:
            column = index
            break
    if column is None:
        return []

    pins: dict[int, dict] = {}
    for row in rows:
        number_cell = cell_text(row[column]) if column < len(row) else ""
        if not number_cell.isdigit():
            continue
        name = normalize_name(cell_text(row[layout["name_col"]]))
        if not re.search(r"[A-Za-z]", name):
            continue
        number = int(number_cell)
        if number in pins:
            continue  # a row split across a page break
        kind_col = layout["name_col"] + 1
        kind = cell_text(row[kind_col]) if kind_col < len(row) else ""
        pin = {"number": number, "name": name, "type": classify(name, kind)}
        pb = port_bit(name)
        if pb and pin["type"] not in ("power", "ground"):
            pin["port"], pin["pin"] = pb

        functions: list[str] = []
        default_col = layout["default_col"]
        if default_col is not None and default_col < len(row):
            functions += clean_af(cell_text(row[default_col]))
        for index in layout["af_cols"]:
            if index < len(row):
                functions += clean_af(cell_text(row[index]))
        seen: set[str] = set()
        unique = []
        for entry in functions:
            if entry in seen:
                continue
            seen.add(entry)
            unique.append(entry)
        if unique:
            pin["functions"] = unique
        pins[number] = pin

    return pins


# ---------------------------------------------------------------------------
# validation
# ---------------------------------------------------------------------------
def family_group(pdf_stem: str) -> str:
    """Series group that one datasheet covers, from the DFP folder name.

    N32G455.pdf covers N32G451/452/455/457, and its pack is `N32G45x_DFP`, so
    `x` is the wildcard the app matches parts against ("N32G452" starts with
    "N32G45"). Using the PDF stem alone would hide the table from the sibling
    series that the same datasheet describes.
    """
    for pack in (ROOT / "svd").iterdir():
        if not pack.is_dir() or not pack.name.startswith(pdf_stem[:6]):
            continue
        name = pack.name.split("_DFP")[0]
        if name:
            return name
    return pdf_stem


def load_expected_counts() -> dict[str, dict[str, int]]:
    """(family-ish stem -> {package -> pinCount}) from the CMSIS-Pack pdsc.

    The pdsc describes each exact part with `<feature type="QFN" count="48"
    name="QFN48"/>` style rows, so it can check our extraction.
    """
    expected: dict[str, dict[str, int]] = {}
    for pdsc in sorted((ROOT / "svd").glob("**/*.pdsc")):
        text = pdsc.read_text(encoding="utf-8", errors="ignore")
        for match in re.finditer(r'<device\s+Dname="([^"]+)"[^>]*>(.*?)</device>', text, re.S):
            part, block = match.group(1), match.group(2)
            found: dict[str, int] = {}
            for kind in ("QFN", "QFP", "BGA", "LGA"):
                feature = re.search(
                    r'<feature\s+type="%s"\s+count="(\d+)"\s+name="([^"]+)"' % kind, block)
                if feature:
                    found[feature.group(2).upper().replace(" ", "")] = int(feature.group(1))
            if found:
                expected[part] = found
    return expected


def merge_tables(a: list[dict], b: list[dict]) -> tuple[list[dict], bool]:
    """Union of two tables of the same package; also reports whether the AF
    lists differed (pin names are expected to be identical and are not merged)."""
    by_number = {pin["number"]: dict(pin) for pin in a}
    changed = False
    for pin in b:
        target = by_number.get(pin["number"])
        if target is None:
            by_number[pin["number"]] = dict(pin)
            changed = True
            continue
        if target.get("name") != pin.get("name"):
            # A pin on a different signal would mean the column was misread;
            # keep the first table and report it rather than guessing.
            print(f"[WARN] 脚 {pin['number']} 名称冲突: "
                  f"{target.get('name')} vs {pin.get('name')}", file=sys.stderr)
            continue
        merged = list(target.get("functions", []))
        for entry in pin.get("functions", []):
            if entry not in merged:
                merged.append(entry)
                changed = True
        if merged:
            target["functions"] = merged
    return [by_number[key] for key in sorted(by_number)], changed


def make_table(chip: str, package: str, pins: list[dict], source_family: str) -> dict:
    return {
        "name": package,
        "chip": chip,
        "numberVerified": True,
        "source": (f"datasheet: data/datasheets/{source_family}.pdf 表3-1 管脚定义"
                   f"（{package} 列）"),
        "pinCountNote": (f"{len(pins)} 脚，取自厂商数据手册管脚定义表并校验为 "
                         f"1..{len(pins)} 连续"),
        "pins": pins,
    }


def validate(pins: list[dict], family: str, package: str, expected: dict) -> str | None:
    """Return an error string, or None when the table looks trustworthy."""
    if not pins:
        return "没有解析到任何引脚"
    numbers = [pin["number"] for pin in pins]
    if numbers != list(range(1, len(pins) + 1)):
        # A gap means the column bled into another table; a duplicate means the
        # dedup failed. Both would put signals on the wrong physical pins.
        dupes = sorted({n for n in numbers if numbers.count(n) > 1})
        return (f"引脚号不连续 1..{len(pins)}（最大 {max(numbers)}"
                + (f"，重复 {dupes}" if dupes else "") + "）")

    # Strongest, self-evident check: the package name encodes its pin count, so
    # LQFP64 must produce exactly 64 pins. This catches a table fragment or the
    # wrong table being picked up (which otherwise slips past 1..N continuity).
    digits = re.search(r"(\d+)", package)
    if digits:
        declared = int(digits.group(1))
        if declared != len(pins):
            return f"封装名 {package} 声明 {declared} 脚，但解析出 {len(pins)} 脚"

    known = expected.get(family)
    if known:
        for pkg, count in known.items():
            if pkg == package and count != len(pins):
                return f"引脚数 {len(pins)} 与 pdsc 的 {pkg}={count} 不符"
    return None


# ---------------------------------------------------------------------------
def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--family", help="only this family (e.g. N32H473)")
    parser.add_argument("--dry-run", action="store_true")
    args = parser.parse_args()

    if not DATASHEETS.exists():
        print("[ERROR] data/datasheets 不存在；先运行 scripts/fetch-n32-datasheets.py",
              file=sys.stderr)
        return 1

    expected = load_expected_counts()
    pdfs = sorted(DATASHEETS.glob("*.pdf"))
    if args.family:
        pdfs = [p for p in pdfs if p.stem.upper() == args.family.upper()]

    written = 0
    rejected: list[str] = []
    seen: dict[tuple[str, str], list[dict]] = {}
    written_data: dict[tuple[str, str], list[dict]] = {}
    for pdf in pdfs:
        family = pdf.stem
        # Table 3-1 spans several pages (one fragment per page, header repeated),
        # so pins are accumulated across every fragment and merged by pin number.
        collected: dict[str, dict[int, dict]] = {}
        try:
            for page_no, rows, layout in find_pin_tables(pdf):
                for package in layout["packages"].values():
                    fragment = extract_package(rows, layout, package)
                    if not fragment:
                        continue
                    bucket = collected.setdefault(package, {})
                    for number, pin in fragment.items():
                        bucket.setdefault(number, pin)
        except Exception as exc:  # pragma: no cover - vendor PDF oddities
            print(f"[WARN] {family}: 解析异常 {exc}", file=sys.stderr)
            continue

        if not collected:
            print(f"[--]   {family}: 未找到可解析的管脚定义表")
            continue

        for package, bucket in sorted(collected.items()):
            pins = [bucket[key] for key in sorted(bucket)]
            error = validate(pins, family, package, expected)
            if error:
                rejected.append(f"{family}/{package}: {error}")
                print(f"[skip] {family}/{package}: {error}")
                continue
            out = {
                "name": package,
                "chip": family_group(family),
                "numberVerified": True,
                "source": f"datasheet: data/datasheets/{family}.pdf 表3-1 管脚定义（{package} 列）",
                "pinCountNote": f"{len(pins)} 脚，取自厂商数据手册管脚定义表并校验为 1..{len(pins)} 连续",
                "pins": pins,
            }
            # One table per (series group, package): several datasheets of the same
            # group (N32H482 and N32H487 both document N32H48x) would otherwise
            # write the same key twice. Identical content is deduplicated; a
            # disagreement between two datasheets is a real error.
            key = (out["chip"], package)
            previous = seen.get(key)
            if previous is not None:
                # Two datasheets of the same series group describe the same
                # package. Pin names always agree (that part is deterministic),
                # but alternate-function lists differ because the AF column is
                # found heuristically and one document may lose entries.
                # A missing AF is worse than an extra one, so merge the union.
                merged, changed = merge_tables(previous, pins)
                if changed:
                    print(f"[merge] {key[0]:9s} {package:11s} {family} 与已有手册取 AF 并集")
                    written_data[key] = merged
                    target = PACKAGES / f"{key[0]}_{package}.json"
                    if not args.dry_run:
                        target.write_text(
                            json.dumps(make_table(key[0], package, merged, family),
                                       ensure_ascii=False, indent=2) + "\n",
                            encoding="utf-8")
                else:
                    print(f"[dup]  {key[0]:9s} {package:11s} 与已有手册一致，去重")
                continue
            seen[key] = pins
            written_data[key] = pins

            target = PACKAGES / f"{out['chip']}_{package}.json"
            if not args.dry_run:
                target.write_text(json.dumps(out, ensure_ascii=False, indent=2) + "\n",
                                  encoding="utf-8")
            gpis = sum(1 for p in pins if p.get("port"))
            print(f"[ok]   {family:9s} {package:11s} {len(pins):3d} 脚 ({gpis} GPIO)"
                  + ("" if not args.dry_run else "  [dry-run]"))
            written += 1

    print(f"\n{written} 张封装表" + ("（dry-run，未写入）" if args.dry_run else f" -> {PACKAGES}"))
    if rejected:
        print(f"{len(rejected)} 张被校验拒绝（保留 synthesize 兜底）：")
        for line in rejected:
            print(f"  - {line}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
