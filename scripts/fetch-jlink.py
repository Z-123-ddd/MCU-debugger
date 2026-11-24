#!/usr/bin/env python3
"""Stage the J-Link pieces the installer needs, so a machine without SEGGER software
still ends up with a working application.

Two very different things are fetched here, and the difference matters:

1. **Nationstech's device support pack** -- the `JLinkDevices.xml` files plus the 37 flash
   algorithms (`.FLM`) that teach J-Link about N32 chips. SEGGER's own installer does NOT
   contain these, and without them J-Link cannot program a single N32 part: `JLink.exe`
   answers "unknown device". They are vendor-supplied and marked
   `LoaderType="FLASH_ALGO_TYPE_OPEN"`, i.e. redistributable. Nationstech publishes them as
   a plain zip we can download, so this is automated.

2. **SEGGER's J-Link Software and Documentation Pack** -- the DLL, the CLI and the GDB
   server. This is commercial software: we do not ship it, and we cannot download it
   automatically (segger.com answers non-browser requests with an empty 202, so any
   scripted fetch fails). The packager downloads it once by hand, accepting SEGGER's
   licence, and drops it in `tools/jlink-installer/`. The installer then runs it silently
   *on the target machine* with `/S` -- their copy, their licence, nothing redistributed.

Usage
-----
    python scripts/fetch-jlink.py                 # device pack + status of the SEGGER side
    python scripts/fetch-jlink.py --list          # show what would be fetched
"""

from __future__ import annotations

import argparse
import pathlib
import sys
import urllib.request
import zipfile

ROOT = pathlib.Path(__file__).resolve().parent.parent
OUT = ROOT / "tools" / "jlink-devices"
INSTALLER_DIR = ROOT / "tools" / "jlink-installer"

#: Nationstech's J-Link device pack. One zip, two layout variants inside:
#: "JLinkV6.4 to V7.6" and "JLinkV7.7 and above" (J-Link changed where it looks for the
#: database). The installer picks the matching one from the installed version.
DEVICE_PACK_URL = (
    "https://www.nsingtech.com/uploads/Microcontrollers/Common_Platform/"
    "Soft_Development_Platform/JLink_tool_adds_Nations_chip_V1.7.0.zip"
)
DEVICE_PACK_NAME = "JLink_tool_adds_Nations_chip_V1.7.0.zip"

#: The two variants, mapped to the directory name used inside the zip.
VARIANTS = {
    "v64": "JLinkV6.4 to V7.6",
    "v77": "JLinkV7.7 and above",
}

SEGGER_HELP = """\
SEGGER 的 J-Link 软件包没有自动下载：segger.com 对非浏览器请求返回空的 202，
脚本无法获取，而且它是商业软件，随我们的安装包分发需要 SEGGER 授权。

请手动做一次（只做一次）：
  1. 打开 https://www.segger.com/downloads/jlink/
  2. 下载 "J-Link Software and Documentation Pack" 的 Windows 64-bit 安装包
     （文件名形如 JLink_Windows_Vxxx_x64.exe，需接受 SEGGER 的许可协议）
  3. 把它放进：{dir}

之后 scripts\\package.bat 会把它作为可选负载打进安装包；安装时若目标机器没有
J-Link，安装程序会以 /S 静默运行它（在目标机器上安装，不发生再分发）。
"""


def fetch(url: str, dest: pathlib.Path, label: str) -> bool:
    if dest.exists() and dest.stat().st_size > 0:
        print(f"[SKIP] {label} 已存在: {dest}")
        return True
    dest.parent.mkdir(parents=True, exist_ok=True)
    print(f"[INFO] 下载 {label}\n       {url}")
    try:
        req = urllib.request.Request(url, headers={"User-Agent": "Mozilla/5.0"})
        with urllib.request.urlopen(req, timeout=120) as response:
            data = response.read()
    except Exception as exc:  # noqa: BLE001 - report and let the caller decide
        print(f"[ERROR] 下载失败: {exc}")
        return False
    if len(data) < 1024:
        print(f"[ERROR] 下载内容只有 {len(data)} 字节，不是有效压缩包")
        return False
    dest.write_bytes(data)
    print(f"[OK] {dest} ({len(data)} 字节)")
    return True


def extract_variants(zip_path: pathlib.Path) -> bool:
    try:
        archive = zipfile.ZipFile(zip_path)
    except Exception as exc:  # noqa: BLE001
        print(f"[ERROR] 打不开压缩包: {exc}")
        return False

    names = archive.namelist()
    ok = True
    for key, prefix in VARIANTS.items():
        members = [n for n in names if n.startswith(prefix + "/") and not n.endswith("/")]
        if not members:
            print(f"[WARN] 压缩包里没有 {prefix}/ 这一支，跳过")
            ok = False
            continue
        target = OUT / key
        for name in members:
            relative = name[len(prefix) + 1 :]
            if not relative:
                continue
            destination = target / relative
            destination.parent.mkdir(parents=True, exist_ok=True)
            with archive.open(name) as source:
                destination.write_bytes(source.read())
        flm = list((target / "Devices").rglob("*.FLM"))
        print(f"[OK] {key}: {len(members)} 个文件（{len(flm)} 个 FLM）")

    # A guide PDF ships alongside; it explains the manual copy for older tooling and is
    # worth keeping next to the data rather than in the source tree.
    for name in names:
        if name.lower().endswith(".pdf"):
            (OUT / pathlib.Path(name).name).write_bytes(archive.read(name))
            print(f"[OK] 说明文档: {pathlib.Path(name).name}")
            break
    return ok


def report_segger() -> bool:
    found = sorted(INSTALLER_DIR.glob("*.exe")) if INSTALLER_DIR.exists() else []
    if found:
        for path in found:
            size_mb = path.stat().st_size / (1024 * 1024)
            print(f"[OK] SEGGER 安装包: {path.name} ({size_mb:.1f} MB) -> 会打进安装包")
        return True
    print()
    print("[WARN] 没有找到 SEGGER 的 J-Link 安装包")
    print(SEGGER_HELP.format(dir=INSTALLER_DIR))
    return False


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--list", action="store_true", help="只显示将要获取的内容")
    args = parser.parse_args()

    if args.list:
        print(f"器件支持包: {DEVICE_PACK_URL}")
        for key, prefix in VARIANTS.items():
            print(f"  -> tools/jlink-devices/{key}/  (来自 {prefix}/)")
        print(f"SEGGER 安装包: 需手动放入 {INSTALLER_DIR}")
        return 0

    # The device pack is ours to keep; the SEGGER installer directory stays for SEGGER's
    # own file only, so the two payloads cannot be confused by the packaging script.
    OUT.mkdir(parents=True, exist_ok=True)
    INSTALLER_DIR.mkdir(parents=True, exist_ok=True)
    cache = OUT / DEVICE_PACK_NAME
    device_ok = fetch(DEVICE_PACK_URL, cache, "国民技术 J-Link 器件支持包")
    if device_ok:
        device_ok = extract_variants(cache)

    segger_ok = report_segger()

    print()
    if not device_ok:
        print("[FAIL] 器件支持包未就绪：没有它 J-Link 认不出任何 N32 型号")
        return 1
    if not segger_ok:
        print("[INFO] 器件库已就绪；缺少 SEGGER 安装包时，安装程序会提示用户自行安装")
    else:
        print("[OK] 安装所需的两部分都就绪")
    return 0


if __name__ == "__main__":
    sys.exit(main())
