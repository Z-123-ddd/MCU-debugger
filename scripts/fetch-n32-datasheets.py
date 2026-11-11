#!/usr/bin/env python3
"""Download NSING MCU family datasheets into `data/datasheets/`.

Why
---
Building a real pinout needs the datasheet's pin-definition table: it is the only
place that says "pin 12 of LQFP48 is PA4". The CMSIS-SVD describes the die, the
.pdsc only gives counts, and schematic symbols are unreliable.

The vendor site is a JS app, but the download listing has a plain JSON endpoint
that we can query, so the URLs do not have to be hard-coded and stale:

    GET https://www.nsingtech.com/api/dow?keyword=N32G455
    -> {"data":{"data":[{"title":"N32G455系列数据手册",
                         "attachment":"/uploads/.../CN_DS_N32G455_...pdf", ...}]}}

Usage
-----
    python scripts/fetch-n32-datasheets.py                 # all known families
    python scripts/fetch-n32-datasheets.py --family N32H473
    python scripts/fetch-n32-datasheets.py --list          # show URLs only

Files land in `data/datasheets/<FAMILY>.pdf` (gitignored: vendor binary assets).
Re-running skips files that already look complete.
"""

from __future__ import annotations

import argparse
import json
import pathlib
import re
import subprocess
import sys
import urllib.parse

ROOT = pathlib.Path(__file__).resolve().parent.parent
OUT = ROOT / "data" / "datasheets"
BASE = "https://www.nsingtech.com"

#: Every family that has an SVD in svd/ (i.e. every family this app can render).
FAMILIES = [
    "N32G033", "N32G052", "N32G401", "N32G412", "N32G432", "N32G455",
    "N32H473", "N32H481", "N32H482", "N32H487", "N32H492", "N32H730",
]

#: Datasheet titles seen on the download centre ("系列数据手册" / "Datasheet").
TITLE_RE = re.compile(r"(数据手册|datasheet)", re.I)


def curl_json(url: str) -> dict:
    proc = subprocess.run(["curl", "-sS", "-L", "--max-time", "40", url],
                          capture_output=True)
    try:
        return json.loads(proc.stdout.decode("utf-8", "replace"))
    except Exception:
        return {}


def find_datasheet(family: str) -> tuple[str, str] | None:
    """Return (title, absolute_url) for the family datasheet, or None."""
    data = curl_json(f"{BASE}/api/dow?keyword={urllib.parse.quote(family)}")
    rows = (data.get("data") or {}).get("data") or []
    for row in rows:
        title = row.get("title") or ""
        attachment = row.get("attachment") or ""
        if not TITLE_RE.search(title):
            continue
        if not attachment.lower().endswith(".pdf"):
            continue
        # Prefer an entry that names this family (the search is fuzzy).
        if family not in attachment and family not in title:
            continue
        return title, BASE + attachment
    return None


def download(url: str, dst: pathlib.Path) -> bool:
    dst.parent.mkdir(parents=True, exist_ok=True)
    if dst.exists() and dst.stat().st_size > 500_000 and dst.read_bytes()[:4] == b"%PDF":
        print(f"[skip] {dst.name} already present ({dst.stat().st_size // 1024} KB)")
        return True
    subprocess.run(["curl", "-sS", "-L", "--max-time", "240",
                    "-A", "Mozilla/5.0", "-o", str(dst), url], capture_output=True)
    if not dst.exists():
        return False
    head = dst.read_bytes()[:4]
    if head != b"%PDF":
        # The vendor occasionally serves an HTML error page for a moved file.
        size = dst.stat().st_size
        dst.unlink()
        print(f"[FAIL] {dst.name}: not a PDF ({size} bytes served)", file=sys.stderr)
        return False
    print(f"[ok]   {dst.name} ({dst.stat().st_size // 1024} KB)")
    return True


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--family", help="only this family (e.g. N32H473)")
    parser.add_argument("--list", action="store_true", help="print URLs, do not download")
    args = parser.parse_args()

    families = [args.family] if args.family else FAMILIES
    ok, failed, missing = [], [], []

    for family in families:
        found = find_datasheet(family)
        if not found:
            missing.append(family)
            print(f"[--]   {family}: no datasheet entry in the download centre")
            continue
        title, url = found
        if args.list:
            print(f"[url]  {family}: {title}\n       {url}")
            continue
        print(f"[get]  {family}: {title}")
        (ok if download(url, OUT / f"{family}.pdf") else failed).append(family)

    if args.list:
        return 0

    print(f"\n{len(ok)} downloaded, {len(failed)} failed, {len(missing)} not listed -> {OUT}")
    for family in failed:
        print(f"  [failed] {family}", file=sys.stderr)
    for family in missing:
        print(f"  [missing] {family}", file=sys.stderr)
    return 0 if not failed else 1


if __name__ == "__main__":
    raise SystemExit(main())
