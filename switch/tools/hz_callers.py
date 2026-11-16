"""For profile samples whose pc is in a function matching a pattern: which thread they came from and
which function the link register points into (the caller, for a leaf; the function itself otherwise).

    hz_callers.py <build.elf> <regex> <profile.bin> [more profiles]   (inside the Switch dev container)"""
import bisect
import collections
import re
import struct
import subprocess
import sys

elf, pattern, profiles = sys.argv[1], re.compile(sys.argv[2]), sys.argv[3:]
NM = "/opt/devkitpro/devkitA64/bin/aarch64-none-elf-nm"
syms = []
for line in subprocess.run([NM, "-n", "--defined-only", elf], capture_output=True, text=True).stdout.splitlines():
    p = line.split(None, 2)
    if len(p) == 3 and p[1] in "tTwW":
        syms.append((int(p[0], 16), p[2]))
syms.sort()
starts = [a for a, _ in syms]
handler = next(a for a, n in syms if n == "__libnx_exception_handler")


def name(off):
    k = bisect.bisect_right(starts, off) - 1
    return syms[k][1] if k >= 0 else "?"


lrs, threads = collections.Counter(), collections.Counter()
for path in profiles:
    d = open(path, "rb").read()
    _, ref, _, _ = struct.unpack_from("<8sQQQ", d, 0)
    base = ref - handler
    for i in range((len(d) - 32) // 24):
        th, _, pc, lr = struct.unpack_from("<IIQQ", d, 32 + i * 24)
        if pattern.search(name(pc - base)):
            lrs[name(lr - base)[:100]] += 1
            threads[(path[-13:], th)] += 1
print("threads:", threads.most_common(6))
for n, c in lrs.most_common(12):
    print("%5d  lr in %s" % (c, n))
