"""Link the Switch build with devkitA64's g++ driver and libnx's switch.specs.

clang compiled the objects (the PPU lift needs clang's musttail), but clang's bare-metal link
driver knows nothing of libnx: wrong crt0, -lc++, no specs. This takes the object and library
list of the Infamous2Recomp target from ninja and hands it to aarch64-none-elf-g++.
Usage (inside the Switch dev container): switch_link.py <build dir> [extra objects/libs...]
Writes <build dir>/link.log, and <build dir>/undefined.txt (one symbol per line, with a count)."""
import collections
import os
import re
import shlex
import subprocess
import sys

build = sys.argv[1]
extra = sys.argv[2:]
DKP = "/opt/devkitpro"
cmd = subprocess.run(["ninja", "-C", build, "-t", "commands", "Infamous2Recomp"],
                     capture_output=True, text=True).stdout.strip().splitlines()[-1]
tokens = shlex.split(cmd.split("&&")[1] if "&&" in cmd else cmd)
keep = []
for t in tokens[1:]:
    if t.endswith((".obj", ".o", ".a")) or t.startswith(("-l", "-L", "-Wl,")):
        if t in ("-lc++", "-ldrm_nouveau") and not os.path.exists(f"{DKP}/portlibs/switch/lib/lib{t[2:]}.a"):
            continue
        keep.append(t)
out = os.path.join(build, "Infamous2Recomp.elf")
if os.path.exists(out):
    os.remove(out)   # never package a stale ELF from an earlier link
link = [f"{DKP}/devkitA64/bin/aarch64-none-elf-g++", f"-specs={DKP}/libnx/switch.specs",
        "-march=armv8-a+crc+crypto", "-mtune=cortex-a57", "-mtp=soft", "-fPIE",
        f"-Wl,-Map,{out}.map", "-o", out, *keep, *extra,
        f"-L{DKP}/portlibs/switch/lib", f"-L{DKP}/libnx/lib", "-lnx", "-lm"]
p = subprocess.run(link, cwd=build, capture_output=True, text=True, errors="replace")
log = p.stdout + p.stderr
open(os.path.join(build, "link.log"), "w").write(" ".join(link[:8]) + " ...\n" + log)
und = collections.Counter(re.findall(r"undefined reference to `([^']+)'", log))
with open(os.path.join(build, "undefined.txt"), "w") as f:
    for sym, n in sorted(und.items()):
        f.write(f"{n:6d} {sym}\n")
other = [l for l in log.splitlines() if "undefined reference" not in l and ": in function" not in l
         and "more undefined references" not in l]
print(f"link exit={p.returncode}, {len(keep)} inputs, {len(und)} undefined symbols")
for l in other[:25]:
    print("  ", l[-220:])
sys.exit(p.returncode or (1 if und else 0))
