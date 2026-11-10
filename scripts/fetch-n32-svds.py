#!/usr/bin/env python3
"""Download the official Nations/N32 DFP packs and extract every SVD file.

Why: a CMSIS-Pack is **per series** (N32H47x, N32G45x, ...), not one pack for the
whole N32 family. The vendor's pack server is declared inside each pack's own
`.pdsc` as `<url>`, so this script reads those indexes (Keil keeps a copy under
`PACK/.Web/`) and pulls the matching archives.

Output layout (relative to the repository root):

    svd/<VENDOR>.<SERIES>_DFP/<file>.svd      one directory per series
    svd/manifest.json                          part number -> svd path + package

`manifest.json` is what lets the app pick the SVD *the vendor assigned to that
exact part number* instead of fuzzy-matching a family name.

Usage:
    python scripts/fetch-n32-svds.py                  # all series
    python scripts/fetch-n32-svds.py --only N32H47x   # one series
    python scripts/fetch-n32-svds.py --list           # show what would be fetched

The archives are NOT committed (see .gitignore): run this once per machine.
"""

import argparse
import io
import json
import pathlib
import re
import sys
import urllib.error
import urllib.request
import zipfile

# Keil keeps the pack index it fetched from the vendor here; it carries both the
# download <url> and the newest <release version="...">.
WEB_INDEX_DIRS = [
    pathlib.Path(r"C:/Keil_v5/ARM/PACK/.Web"),
    pathlib.Path(r"C:/ProgramData/Keil_v5/ARM/PACK/.Web"),
]

# Fallback when no Keil index is available: (series, base url, version).
FALLBACK = [
    ("N32A455", "https://www.nsing.com.sg/public/uploads/armpacks/", "1.0.1"),
    ("N32G003", "https://www.nsing.com.sg/public/uploads/armpacks/", "1.0.2"),
    ("N32G030", "https://www.nsing.com.sg/uploadfile/file/pack/", "1.0.5"),
    ("N32G031", "https://www.nsing.com.sg/public/uploads/armpacks/", "1.0.5"),
    ("N32G032", "https://www.nsing.com.sg/public/uploads/armpacks/", "0.2.2"),
    ("N32G05x", "https://www.nsing.com.sg/uploadfile/file/pack/", "1.0.2"),
    ("N32G401", "https://www.nsing.com.sg/public/uploads/armpacks/", "1.0.1"),
    ("N32G430", "https://www.nsing.com.sg/public/uploads/armpacks/", "1.1.1"),
    ("N32G43x", "https://www.nsing.com.sg/public/uploads/armpacks/", "0.9.7"),
    ("N32G45x", "https://www.nsing.com.sg/public/uploads/armpacks/", "1.0.7"),
    ("N32G4FR", "https://www.nsing.com.sg/public/uploads/armpacks/", "1.0.6"),
    ("N32H47x", "https://www.nsing.com.sg/public/uploads/armpacks/", "1.0.1"),
    ("N32H48x", "https://www.nsing.com.sg/public/uploads/armpacks/", "1.0.1"),
    ("N32L40x", "https://www.nsing.com.sg/public/uploads/armpacks/", "1.0.2"),
    ("N32L43x", "https://www.nsing.com.sg/public/uploads/armpacks/", "0.2.2"),
]

ROOT = pathlib.Path(__file__).resolve().parent.parent
OUT_DIR = ROOT / "svd"


def read_index() -> list[tuple[str, str, str, str]]:
    """(vendor_pack, base_url, version, pdsc_text) for every NSING N32 pack."""
    found: dict[str, tuple[str, str, str, str]] = {}

    for index_dir in WEB_INDEX_DIRS:
        if not index_dir.is_dir():
            continue
        for pdsc in sorted(index_dir.glob("NSING.N32*_DFP.pdsc")):
            text = pdsc.read_text(encoding="utf-8", errors="ignore")
            url = re.search(r"<url>([^<]+)</url>", text)
            release = re.search(r'<release[^>]*version="([^"]+)"', text)
            if not url or not release:
                continue
            pack = pdsc.stem                     # NSING.N32H47x_DFP
            series = pack.split(".")[1].removesuffix("_DFP")  # N32H47x
            found.setdefault(series, (pack, url.group(1).strip(), release.group(1), text))

    for series, url, version in FALLBACK:
        found.setdefault(series, (f"NSING.{series}_DFP", url, version, ""))

    return [found[key] for key in sorted(found)]


def fetch(url: str, dest: pathlib.Path, retries: int = 4) -> bool:
    for attempt in range(1, retries + 1):
        try:
            print(f"    GET {url} (try {attempt})")
            with urllib.request.urlopen(url, timeout=120) as response:
                data = response.read()
            dest.write_bytes(data)
            print(f"    -> {len(data) / 1024 / 1024:.1f} MiB")
            return True
        except (urllib.error.URLError, TimeoutError, OSError) as exc:
            print(f"    ! {exc}")
    return False


def parse_pdsc(text: str) -> list[dict]:
    """Per-device: name, package, pin count, IO count, svd path."""
    devices = []
    for match in re.finditer(r'<device Dname="([^"]+)"(.*?)</device>', text, re.S):
        name, body = match.group(1), match.group(2)

        package = pin_count = io_count = None
        for kind in ("QFN", "QFP", "BGA", "CSP"):
            feature = re.search(rf'<feature type="{kind}"\s+count="(\d+)"\s+name="([^"]+)"', body)
            if feature:
                pin_count = int(feature.group(1))
                package = feature.group(2)
                break
        ios = re.search(r'<feature type="IOs"\s+n="(\d+)"', body)
        if ios:
            io_count = int(ios.group(1))

        svd = re.search(r'<debug\s+svd="([^"]+)"', body)
        devices.append(
            {
                "part": name,
                "package": package,
                "pinCount": pin_count,
                "ioCount": io_count,
                "svd": svd.group(1).replace("\\", "/") if svd else None,
            }
        )
    return devices


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--only", help="restrict to one series, e.g. N32H47x")
    parser.add_argument("--list", action="store_true", help="show the plan and exit")
    parser.add_argument("--keep-packs", action="store_true",
                        help="keep the downloaded .pack archives")
    args = parser.parse_args()

    plan = read_index()
    if args.only:
        plan = [entry for entry in plan if entry[0].startswith(f"NSING.{args.only}")]
    if not plan:
        print("没有找到任何 N32 pack 索引", file=sys.stderr)
        return 1

    if args.list:
        for pack, url, version, _ in plan:
            print(f"{pack}  {version}  {url}{pack}.{version}.pack")
        return 0

    OUT_DIR.mkdir(parents=True, exist_ok=True)
    cache = OUT_DIR / ".cache"
    cache.mkdir(exist_ok=True)

    manifest: dict[str, dict] = {}
    ok = failed = 0

    for pack, base, version, web_text in plan:
        series = pack.split(".")[1]
        archive = cache / f"{pack}.{version}.pack"
        target = OUT_DIR / pack

        print(f"[{series}] {pack} {version}")
        if not archive.is_file() or archive.stat().st_size == 0:
            if not fetch(f"{base}{pack}.{version}.pack", archive):
                failed += 1
                print("    跳过（下载失败）")
                continue

        try:
            with zipfile.ZipFile(archive) as zf:
                names = zf.namelist()
                svds = [n for n in names if n.lower().endswith(".svd")]
                pdscs = [n for n in names if n.lower().endswith(".pdsc")]
                if not svds:
                    print("    包内没有 SVD，跳过")
                    failed += 1
                    continue

                target.mkdir(parents=True, exist_ok=True)
                for name in svds:
                    dest = target / pathlib.Path(name).name
                    dest.write_bytes(zf.read(name))
                pdsc_text = ""
                for name in pdscs:
                    pdsc_text = zf.read(name).decode("utf-8", "ignore")
                    (target / "package.pdsc").write_text(pdsc_text, encoding="utf-8")
                    break
        except zipfile.BadZipFile:
            print("    不是有效的 zip，跳过")
            failed += 1
            continue

        extracted = sorted(p.name for p in target.glob("*.svd"))
        print(f"    SVD: {', '.join(extracted)}")

        for device in parse_pdsc(pdsc_text or web_text):
            if not device["svd"]:
                continue
            svd_file = pathlib.Path(device["svd"]).name
            device["svdPath"] = str((target / svd_file).resolve())
            device["series"] = series
            device["pack"] = pack
            manifest[device["part"]] = device
        ok += 1

    if not args.keep_packs:
        for leftover in cache.glob("*.pack"):
            leftover.unlink(missing_ok=True)
        try:
            cache.rmdir()
        except OSError:
            pass

    manifest_path = OUT_DIR / "manifest.json"
    manifest_path.write_text(
        json.dumps(
            {
                "comment": "Generated by scripts/fetch-n32-svds.py. Maps an exact part "
                           "number to the SVD the vendor assigned to it.",
                "devices": dict(sorted(manifest.items())),
            },
            ensure_ascii=False,
            indent=2,
        )
        + "\n",
        encoding="utf-8",
    )

    print()
    print(f"系列成功 {ok} / 失败 {failed}；型号映射 {len(manifest)} 条")
    print(f"输出: {OUT_DIR}")
    print(f"清单: {manifest_path}")
    return 0 if failed == 0 else 2


if __name__ == "__main__":
    sys.exit(main())
