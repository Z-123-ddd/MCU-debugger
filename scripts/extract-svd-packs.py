#!/usr/bin/env python3
"""Extract SVD files from N32 DFP packs already present in `svd/`.

`scripts/fetch-n32-svds.py` downloads the packs; this script turns whatever `.pack`
archives are sitting in `svd/` into a usable tree plus `svd/manifest.json`:

    svd/<VENDOR_SERIES>/<file>.svd
    svd/manifest.json          part number -> svd path, package, IO/pin counts

Keeping the two steps separate means packs can also be dropped in by hand (which
is handy when the vendor site is slow), and re-running is idempotent.

Usage:
    python scripts/extract-svd-packs.py            # extract everything in svd/
    python scripts/extract-svd-packs.py --clean    # drop previously extracted .svd first
"""

import argparse
import json
import pathlib
import re
import shutil
import sys
import zipfile

ROOT = pathlib.Path(__file__).resolve().parent.parent
SVD_DIR = ROOT / "svd"


def series_dir_name(pack_stem: str) -> str:
    """'Nations.N32H47x_DFP.1.2.0' -> 'N32H47x_DFP.1.2.0' (vendor prefix dropped)."""
    parts = pack_stem.split(".")
    if len(parts) >= 2:
        return ".".join(parts[1:])
    return pack_stem


def parse_pdsc(text: str) -> list[dict]:
    """Per-device package/pin/IO facts and the SVD the vendor assigned to it."""
    devices: list[dict] = []
    for match in re.finditer(r'<device Dname="([^"]+)"(.*?)</device>', text, re.S):
        name, body = match.group(1), match.group(2)

        package = pin_count = io_count = None
        packages: list[str] = []

        # Two pack generations exist:
        #   older -> structured <feature type="QFN" count="48" name="UQFN48"/>
        #            and <feature type="IOs" n="42"/>
        #   newer -> the same facts only as prose inside <description>:
        #            "- Package:UQFN48/LQFP48" and "- I/O: 42"
        for kind in ("QFN", "QFP", "BGA", "CSP"):
            feature = re.search(rf'<feature type="{kind}"\s+count="(\d+)"\s+name="([^"]+)"', body)
            if feature:
                pin_count = int(feature.group(1))
                package = feature.group(2)
                packages = [feature.group(2)]
                break
        ios = re.search(r'<feature type="IOs"\s+n="(\d+)"', body)
        if ios:
            io_count = int(ios.group(1))

        if package is None or io_count is None:
            description = re.search(r"<description>(.*?)</description>", body, re.S)
            prose = description.group(1) if description else ""
            prose = prose.replace("&#xA;", " ").replace("&#xD;", " ")

            if package is None:
                # "- Package:UQFN48/LQFP48" lists every package the part is offered
                # in. A single name is unambiguous; with several we must not guess
                # (the IO count belongs to only one of them, e.g.
                # N32H473VE = LQFP100/LQFP128 with I/O 107 -> the 107 is the 128-pin
                # variant). Record all candidates and leave the pin count unknown.
                pkg = re.search(r"Package:\s*([A-Za-z0-9/._-]+)", prose)
                if pkg:
                    candidates = [c.strip() for c in pkg.group(1).split("/") if c.strip()]
                    packages = candidates
                    # The pin count is in the package name; it stays unambiguous as
                    # long as every candidate agrees (UQFN48/LQFP48 -> 48) and is
                    # genuinely ambiguous when they differ (LQFP100/LQFP128).
                    counts = set()
                    for candidate in candidates:
                        digits = re.search(r"(\d+)$", candidate)
                        if digits:
                            counts.add(int(digits.group(1)))
                    if len(counts) == 1:
                        pin_count = counts.pop()
                    if len(candidates) == 1:
                        package = candidates[0]

            if io_count is None:
                io = re.search(r"I/O:\s*(\d+)", prose)
                if io:
                    io_count = int(io.group(1))

        svd = re.search(r'<debug\s+svd="([^"]+)"', body)
        devices.append(
            {
                "part": name,
                "package": package,
                "pinCount": pin_count,
                "ioCount": io_count,
                "packages": packages,
                "svd": pathlib.Path(svd.group(1).replace("\\", "/")).name if svd else None,
            }
        )
    return devices


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--clean", action="store_true",
                        help="remove previously extracted .svd files first")
    args = parser.parse_args()

    packs = sorted(SVD_DIR.glob("*.pack"))
    if not packs:
        print(f"{SVD_DIR} 里没有 .pack 文件", file=sys.stderr)
        return 1

    if args.clean:
        removed = 0
        for stale in SVD_DIR.rglob("*.svd"):
            stale.unlink()
            removed += 1
        print(f"清掉 {removed} 个已解压的 .svd")

    manifest: dict[str, dict] = {}
    extracted_total = 0

    for pack in packs:
        target = SVD_DIR / series_dir_name(pack.stem)
        target.mkdir(parents=True, exist_ok=True)

        try:
            with zipfile.ZipFile(pack) as archive:
                names = archive.namelist()
                svds = [n for n in names if n.lower().endswith(".svd")]
                pdscs = [n for n in names if n.lower().endswith(".pdsc")]
                if not svds:
                    print(f"[skip] {pack.name}: 包内没有 SVD")
                    continue

                for name in svds:
                    data = archive.read(name)
                    (target / pathlib.Path(name).name).write_bytes(data)
                    extracted_total += 1

                pdsc_text = ""
                for name in pdscs:
                    pdsc_text = archive.read(name).decode("utf-8", "ignore")
                    (target / "package.pdsc").write_text(pdsc_text, encoding="utf-8")
                    break
        except zipfile.BadZipFile:
            print(f"[skip] {pack.name}: 不是有效的 zip")
            continue

        names_out = sorted(p.name for p in target.glob("*.svd"))
        print(f"[ok  ] {pack.stem}: {len(names_out)} 个 SVD -> svd/{target.name}/")

        for device in parse_pdsc(pdsc_text):
            if not device["svd"]:
                continue
            svd_path = target / device["svd"]
            if not svd_path.is_file():
                continue
            device["svdPath"] = str(svd_path.relative_to(ROOT)).replace("\\", "/")
            device["pack"] = pack.name
            manifest[device["part"]] = device

    manifest_path = SVD_DIR / "manifest.json"
    manifest_path.write_text(
        json.dumps(
            {
                "comment": "Generated by scripts/extract-svd-packs.py. Maps an exact part "
                           "number to the SVD and package facts the vendor assigned to it.",
                "devices": dict(sorted(manifest.items())),
            },
            ensure_ascii=False,
            indent=2,
        )
        + "\n",
        encoding="utf-8",
    )

    print()
    print(f"解压 {extracted_total} 个 SVD，型号映射 {len(manifest)} 条")
    print(f"清单: {manifest_path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
