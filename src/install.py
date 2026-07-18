# SPDX-License-Identifier: GPL-3.0-or-later
"""Install / uninstall the DXGI proxy into the Assetto Corsa folder.

Copies build\\dxgi.dll and build\\dxgi_real.dll next to acs.exe. Uninstall removes
exactly those two files and nothing else.

  python install.py --status
  python install.py --install
  python install.py --uninstall

Refuses to run while acs.exe is up: the DLL would be locked and a half-installed proxy
means AC fails to start.
"""
import argparse
import hashlib
import os
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
BUILD = os.path.join(HERE, "build")
FILES = ("dxgi.dll", "dxgi_real.dll")

def find_ac_dir():
    """Find the AC install: Steam registry + library folders, then common paths."""
    try:
        import re
        import winreg
        with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE,
                            r"SOFTWARE\WOW6432Node\Valve\Steam") as k:
            steam = winreg.QueryValueEx(k, "InstallPath")[0]
        libs = [steam]
        vdf = os.path.join(steam, "steamapps", "libraryfolders.vdf")
        if os.path.isfile(vdf):
            with open(vdf, encoding="utf-8", errors="replace") as fh:
                libs += [p.replace("\\\\", "\\")
                         for p in re.findall(r'"path"\s+"([^"]+)"', fh.read())]
        for lib in libs:
            cand = os.path.join(lib, "steamapps", "common", "assettocorsa")
            if os.path.isfile(os.path.join(cand, "acs.exe")):
                return cand
    except OSError:
        pass
    for cand in (r"C:\Program Files (x86)\Steam\steamapps\common\assettocorsa",
                 r"D:\SteamLibrary\steamapps\common\assettocorsa"):
        if os.path.isfile(os.path.join(cand, "acs.exe")):
            return cand
    return None

AC_DIR = None   # resolved in main()
LOG = None

def _sha(path):
    if not os.path.isfile(path):
        return None
    h = hashlib.sha256()
    with open(path, "rb") as fh:
        for chunk in iter(lambda: fh.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()[:16]

def _ac_running():
    try:
        out = subprocess.run(["tasklist", "/FI", "IMAGENAME eq acs.exe"],
                             capture_output=True, text=True, timeout=20).stdout
        return "acs.exe" in out.lower()
    except (OSError, subprocess.SubprocessError):
        return False

def status():
    print(f"AC folder: {AC_DIR}")
    for f in FILES:
        src, dst = os.path.join(BUILD, f), os.path.join(AC_DIR, f)
        s, d = _sha(src), _sha(dst)
        state = "not installed" if d is None else ("current" if s == d else "STALE")
        print(f"  {f:<16} build={s or '-':<16} installed={d or '-':<16} {state}")
    print(f"  log: {'present' if os.path.isfile(LOG) else 'none'} ({LOG})")
    print(f"  acs.exe running: {_ac_running()}")

def install():
    if _ac_running():
        print("acs.exe is running — close it first (the DLL would be locked)")
        return 1
    for f in FILES:
        src = os.path.join(BUILD, f)
        if not os.path.isfile(src):
            print(f"missing build artifact: {src}\nrun build.bat first")
            return 1
    for f in FILES:
        shutil.copy2(os.path.join(BUILD, f), os.path.join(AC_DIR, f))
        print(f"installed {f}")
    # acre.ini: only place the default if the user doesn't already have one
    ini_dst = os.path.join(AC_DIR, "acre.ini")
    if not os.path.isfile(ini_dst):
        shutil.copy2(os.path.join(HERE, "acre.ini"), ini_dst)
        print("installed acre.ini (default: mode=dlaa)")
    else:
        print("acre.ini already present — left as-is")
    if os.path.isfile(LOG):
        os.remove(LOG)
        print("cleared old acre_proxy.log")
    return 0

def uninstall():
    if _ac_running():
        print("acs.exe is running — close it first")
        return 1
    for f in FILES:
        p = os.path.join(AC_DIR, f)
        if os.path.isfile(p):
            os.remove(p)
            print(f"removed {f}")
        else:
            print(f"(absent) {f}")
    return 0

def main():
    global AC_DIR, LOG
    ap = argparse.ArgumentParser()
    ap.add_argument("--install", action="store_true")
    ap.add_argument("--uninstall", action="store_true")
    ap.add_argument("--status", action="store_true")
    ap.add_argument("--dir", help="Assetto Corsa folder (default: auto-detect via Steam)")
    a = ap.parse_args()
    AC_DIR = a.dir or find_ac_dir()
    if not AC_DIR or not os.path.isfile(os.path.join(AC_DIR, "acs.exe")):
        print("could not find the Assetto Corsa folder; pass it with --dir")
        return 1
    LOG = os.path.join(AC_DIR, "acre_proxy.log")
    if a.uninstall:
        return uninstall()
    if a.install:
        rc = install()
        status()
        return rc
    status()
    return 0

if __name__ == "__main__":
    sys.exit(main())
