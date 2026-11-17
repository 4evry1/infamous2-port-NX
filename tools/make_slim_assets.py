"""The slim Setup's release-asset candidates (slim split, stage 4): what a player downloads from us
instead of 1.4 GB of build tools. Nothing here is game-derived: the engine exe has no lifted code
(that is the game module, built on the player's PC), the toolchain and Python are upstream files.

    python -I tools/make_slim_assets.py --engine-dir <engine build dir> --module <a built game module>
        [--llvm "C:\\Program Files\\LLVM"] --python-zip <python-X.Y.Z-embed-amd64.zip> --out <dir>
        [--llvm-license <llvm/LICENSE.TXT of the tag>] [--seven-zip "C:\\Program Files\\7-Zip\\7z.exe"]
        [--wheel <pycryptodome-X-cp39-abi3-win_amd64.whl>] [--release v0.6.0]

Writes into --out:
  Infamous2-engine-win-x64.zip     Infamous2Recomp.exe + SDL2.dll (the prebuilt engine)
  llvm-<ver>-slim-win-x64.7z       official LLVM: bin/clang.exe, bin/lld.exe, bin/llvm-profdata.exe,
                                   lib/clang/<maj>/include (the x86 headers), lib/clang/<maj>/lib/
                                   windows/clang_rt.profile-x86_64.lib (PGO), LICENSE.TXT. Windows'
                                   own tar.exe unpacks .7z (tar -xf), no 7-Zip needed on the player's PC.
  python-<ver>-embed-amd64.zip     python.org's embeddable Python, unchanged (its SHA-256 can be checked
                                   against python.org's)
  pycryptodome-<ver>-...whl        (--wheel) PyPI's pycryptodome wheel, unchanged: tools/ps3tool.py unpacks Sony's
                                   files in seconds with it instead of minutes (unpacked into the Python's
                                   Lib/site-packages)
  slim-manifest.json               names, sizes, SHA-256, how to unpack, the module interface version, the
                                   GitHub release the assets are attached to (--release). Setup reads the
                                   same file from the port's own tools/slim_assets.json (commit it there).
--module: a game module built from the same engine source (its interface words go into the manifest;
the module itself is never an asset).
"""
import argparse
import ctypes
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ENGINE_FILES = ("Infamous2Recomp.exe", "SDL2.dll")
# clang's own headers a Windows x86-64 module build reads; the rest (other CPUs, GPUs, sanitizers,
# module maps) is left out
HDR_SKIP = ("__clang_cuda", "__clang_hip", "__clang_spirv", "altivec", "amdgpu", "andes", "arm",
            "hexagon", "htm", "hvx", "larch", "lasx", "lsx", "module.modulemap", "msa", "nvptx", "opencl",
            "ptrauth", "riscv", "s390", "sifive", "spirv", "vecintrin", "velintrin", "wasm")


def sha256(p):
    h = hashlib.sha256()
    with open(p, "rb") as f:
        for b in iter(lambda: f.read(1 << 20), b""):
            h.update(b)
    return h.hexdigest().upper()


def module_interface(dll):
    """abi, layout words and build string of a built game module: its one export, called without
    binding (ps3_game_module() only fills its own info)"""
    class Info(ctypes.Structure):
        _fields_ = [("abi", ctypes.c_uint32), ("size", ctypes.c_uint32), ("layout", ctypes.c_uint32 * 8),
                    ("kit", ctypes.c_char_p), ("build", ctypes.c_char_p)]
    h = ctypes.WinDLL(os.path.abspath(dll))
    f = h.ps3_game_module
    f.restype = ctypes.POINTER(Info)
    i = f().contents
    return {"abi": i.abi, "layout": [int(x) for x in i.layout], "build": (i.build or b"").decode()}


def engine_kit():
    try:
        r = subprocess.run(["git", "-C", os.path.join(ROOT, "engine"), "rev-parse", "--short=12", "HEAD"],
                           capture_output=True, text=True, timeout=20)
        d = subprocess.run(["git", "-C", ROOT, "status", "--porcelain", "--", "engine", "port_src", "patches", "cheats"],
                           capture_output=True, text=True, timeout=60).stdout.strip()
        return r.stdout.strip() + ("+" if d else "")
    except OSError:
        return "unknown"


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--engine-dir", required=True)
    ap.add_argument("--module", required=True)
    ap.add_argument("--llvm", default=r"C:\Program Files\LLVM")
    ap.add_argument("--llvm-license", help="llvm/LICENSE.TXT of the release tag (the installer has none)")
    ap.add_argument("--python-zip", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--seven-zip", default=r"C:\Program Files\7-Zip\7z.exe")
    ap.add_argument("--wheel", help="pycryptodome's cp39-abi3 win_amd64 wheel from PyPI (optional)")
    ap.add_argument("--release", help="the GitHub release tag the assets are attached to, e.g. v0.6.0")
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    assets = []

    # ---- the engine
    eng = os.path.join(a.out, "Infamous2-engine-win-x64.zip")
    with zipfile.ZipFile(eng, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        for n in ENGINE_FILES:
            z.write(os.path.join(a.engine_dir, n), n)
    assets.append({"role": "engine", "name": os.path.basename(eng), "unpack": "zip", "into": "tools-cache\\slim\\engine",
                   "contents": list(ENGINE_FILES),
                   "files_sha256": {n: sha256(os.path.join(a.engine_dir, n)) for n in ENGINE_FILES}})

    # ---- the toolchain
    clang = os.path.join(a.llvm, "bin", "clang.exe")
    ver = re.search(r"clang version (\S+)", subprocess.run([clang, "--version"], capture_output=True, text=True).stdout).group(1)
    major = ver.split(".")[0]
    stage = tempfile.mkdtemp(prefix="slimtc-")
    try:
        os.makedirs(os.path.join(stage, "bin"))
        for n in ("clang.exe", "lld.exe", "llvm-profdata.exe"):
            shutil.copy2(os.path.join(a.llvm, "bin", n), os.path.join(stage, "bin", n))
        src_inc = os.path.join(a.llvm, "lib", "clang", major, "include")
        dst_inc = os.path.join(stage, "lib", "clang", major, "include")
        os.makedirs(dst_inc)
        hdrs = sorted(f for f in os.listdir(src_inc)
                      if os.path.isfile(os.path.join(src_inc, f)) and not f.startswith(HDR_SKIP))
        for f in hdrs:
            shutil.copy2(os.path.join(src_inc, f), os.path.join(dst_inc, f))
        rt = os.path.join("lib", "clang", major, "lib", "windows", "clang_rt.profile-x86_64.lib")
        os.makedirs(os.path.dirname(os.path.join(stage, rt)))
        shutil.copy2(os.path.join(a.llvm, rt), os.path.join(stage, rt))
        lic = a.llvm_license or os.path.join(a.llvm, "LICENSE.TXT")
        if not os.path.isfile(lic):
            sys.exit("no LLVM license text: --llvm-license <llvm/LICENSE.TXT of the release tag>")
        shutil.copy2(lic, os.path.join(stage, "LICENSE.TXT"))
        tc = os.path.join(a.out, "llvm-%s-slim-win-x64.7z" % ver)
        if os.path.exists(tc):
            os.remove(tc)
        p = subprocess.run([a.seven_zip, "a", "-t7z", "-mx=9", "-m0=LZMA2", "-mmt=on", tc, "."], cwd=stage,
                           capture_output=True, text=True)
        if p.returncode:
            sys.exit("7z failed: " + p.stdout[-2000:] + p.stderr[-2000:])
        raw = sum(os.path.getsize(os.path.join(d, f)) for d, _, fs in os.walk(stage) for f in fs)
    finally:
        shutil.rmtree(stage, ignore_errors=True)
    assets.append({"role": "toolchain", "name": os.path.basename(tc), "unpack": "7z (Windows tar.exe: tar -xf <file> -C <dir>)",
                   "into": "tools-cache\\slim\\llvm", "version": ver, "unpacked_bytes": raw,
                   "contents": ["bin/clang.exe", "bin/lld.exe", "bin/llvm-profdata.exe",
                                "lib/clang/%s/include/ (%d x86 headers)" % (major, len(hdrs)), rt.replace("\\", "/"), "LICENSE.TXT"],
                   "use": "PORT_LLVM=<that folder> (tools/build_module.py: clang.exe --driver-mode=cl, lld.exe -flavor link)"})

    # ---- Python
    py = os.path.join(a.out, os.path.basename(a.python_zip))
    if os.path.abspath(py) != os.path.abspath(a.python_zip):
        shutil.copy2(a.python_zip, py)
    pv = re.search(r"python-([\d.]+)-embed", os.path.basename(py))
    assets.append({"role": "python", "name": os.path.basename(py), "unpack": "zip", "into": "tools-cache\\slim\\python",
                   "version": pv.group(1) if pv else "?",
                   "after_unpack": "delete python3XX._pth (with it the lifter scripts cannot import their sibling "
                                   "modules: no script folder on sys.path); then run tools with -E -s or -I",
                   "upstream": "https://www.python.org/ftp/python/%s/%s" % (pv.group(1) if pv else "?", os.path.basename(py))})

    if a.wheel:
        whl = os.path.join(a.out, os.path.basename(a.wheel))
        if os.path.abspath(whl) != os.path.abspath(a.wheel):
            shutil.copy2(a.wheel, whl)
        wv = re.search(r"pycryptodome-([\d.]+)-", os.path.basename(whl))
        assets.append({"role": "pycrypto", "name": os.path.basename(whl), "unpack": "zip", "into": "tools-cache\\slim\\python\\Lib\\site-packages",
                       "version": wv.group(1) if wv else "?", "optional": True,
                       "use": "tools/ps3tool.py is fast with it (Sony's packages in seconds, not minutes)",
                       "upstream": "https://pypi.org/project/pycryptodome/%s/" % (wv.group(1) if wv else "")})

    for x in assets:
        if a.release:
            x["release"] = a.release
        f = os.path.join(a.out, x["name"])
        x["size"] = os.path.getsize(f)
        x["sha256"] = sha256(f)

    mi = module_interface(a.module)
    man = {
        "format": 1,
        "made": time.strftime("%Y-%m-%d %H:%M:%S"),
        "release": a.release or "",
        "engine_kit": engine_kit(),
        "module": {"abi": mi["abi"], "layout": mi["layout"],
                   "file": {"windows": "Infamous2Game.dll", "linux": "libInfamous2Game.so"},
                   "entry": "ps3_game_module", "env_override": "PS3_GAME_MODULE",
                   "next_to": "Infamous2Recomp.exe (the engine finds the module beside itself)",
                   "built_by": "tools/rebuild_win.py with PORT_ENGINE_EXE=<engine exe> PORT_LLVM=<toolchain> "
                               "(steps inputs..compile; compile = tools/build_module.py)",
                   "sample_build": mi["build"]},
        "total_download_bytes": sum(x["size"] for x in assets),
        "assets": assets,
    }
    with open(os.path.join(a.out, "slim-manifest.json"), "w", encoding="utf-8") as f:
        json.dump(man, f, indent=1)
    for x in assets:
        print("%-36s %10d  %s" % (x["name"], x["size"], x["sha256"]))
    print("total %.1f MB; module abi %d layout %s; engine kit %s" % (man["total_download_bytes"] / 1048576, mi["abi"],
                                                                  mi["layout"], man["engine_kit"]))


if __name__ == "__main__":
    main()
