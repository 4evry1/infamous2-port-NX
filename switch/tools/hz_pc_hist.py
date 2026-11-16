"""Where inside one function the profile samples land: a histogram of pc offsets, joined with the
function's disassembly (devkitA64 objdump), hottest instructions first, plus the source lines objdump -l
gives when the build has line info.

    hz_pc_hist.py <build.elf> <function name> <profile.bin> [more]   (inside the Switch dev container)"""
import bisect
import collections
import re
import struct
import subprocess
import sys

elf, fn, profiles = sys.argv[1], sys.argv[2], sys.argv[3:]
B = "/opt/devkitpro/devkitA64/bin/aarch64-none-elf-"
syms = []
for line in subprocess.run([B + "nm", "-n", "-S", "--defined-only", elf], capture_output=True, text=True).stdout.splitlines():
    p = line.split()
    if len(p) == 4 and p[2] in "tTwW":
        syms.append((int(p[0], 16), int(p[1], 16), p[3]))
syms.sort()
handler = next(a for a, _, n in syms if n == "__libnx_exception_handler")
start, size = next((a, s) for a, s, n in syms if n == fn)
hist, total = collections.Counter(), 0
for path in profiles:
    d = open(path, "rb").read()
    _, ref, _, _ = struct.unpack_from("<8sQQQ", d, 0)
    base = ref - handler
    for i in range((len(d) - 32) // 24):
        _, _, pc, _ = struct.unpack_from("<IIQQ", d, 32 + i * 24)
        off = pc - base
        if start <= off < start + size:
            hist[off - start] += 1
            total += 1
dis = subprocess.run([B + "objdump", "-d", "-l", "--no-show-raw-insn", "--start-address=%#x" % start,
                      "--stop-address=%#x" % (start + size), elf], capture_output=True, text=True).stdout.splitlines()
ins, src = {}, ""
for l in dis:
    m = re.match(r"\s*([0-9a-f]+):\s+(.*)", l)
    if m:
        ins[int(m.group(1), 16) - start] = (m.group(2).strip()[:60], src)
    elif l.startswith("/") or re.match(r"^\S+:\d+", l):
        src = l.strip()[-70:]
print("%s: %d bytes, %d samples" % (fn, size, total))
for off, c in hist.most_common(25):
    i, s = ins.get(off, ("?", ""))
    print("%5.1f%%  +0x%04x  %-50s %s" % (100.0 * c / total, off, i, s))
