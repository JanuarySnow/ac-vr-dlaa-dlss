# SPDX-License-Identifier: GPL-3.0-or-later
"""
  python package_release.py            # version = git describe
  python package_release.py v0.2.0     # explicit version
"""
import shutil
import subprocess
import sys
import zipfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
SRC = ROOT / "src"
DIST = HERE / "dist"

FILES = [
    (SRC / "build" / "dxgi.dll", "dxgi.dll"),
    (SRC / "acre.ini", "acre.ini"),
    (HERE / "install.bat", "install.bat"),
    (ROOT / "LICENSE", "LICENSE"),
    (ROOT / "EXCEPTIONS.md", "EXCEPTIONS.md"),
    (ROOT / "redist" / "nvngx_dlss.dll", "nvngx_dlss.dll"),
    (ROOT / "redist" / "nvidia_dlss_license.txt", "nvidia_dlss_license.txt"),
]

def version():
    if len(sys.argv) > 1:
        return sys.argv[1]
    try:
        return subprocess.run(
            ["git", "describe", "--tags", "--always", "--dirty"],
            cwd=ROOT, capture_output=True, text=True, check=True,
        ).stdout.strip()
    except (OSError, subprocess.CalledProcessError):
        return "dev"

def build():
    print("[1/3] building dxgi.dll")
    r = subprocess.run(["cmd", "/c", str(SRC / "build.bat")], cwd=SRC)
    if r.returncode != 0:
        print("build failed"); sys.exit(1)

def assemble():
    print("[2/3] assembling release files")
    if DIST.exists():
        shutil.rmtree(DIST)
    DIST.mkdir(parents=True)
    for src, name in FILES:
        if not src.is_file():
            print(f"missing: {src}"); sys.exit(1)
        shutil.copy2(src, DIST / name)
        print(f"  {name}")

def zip_it(ver):
    print("[3/3] zipping")
    out = HERE / f"ac-dlss-vr-{ver}.zip"
    if out.exists():
        out.unlink()
    with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as zf:
        for f in DIST.iterdir():
            zf.write(f, f.name)
    size_kb = out.stat().st_size / 1024
    print(f"\n{out}  ({size_kb:.0f} KB)")
    return out

def main():
    build()
    assemble()
    zip_it(version())

if __name__ == "__main__":
    main()
