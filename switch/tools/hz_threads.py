"""Per-thread view of console sampling profiles (logs/profile-N.bin): for each chosen thread, its top
functions by pc, and for samples sitting in a kernel call (svc*), which function called it (lr).

    hz_threads.py <build.elf> <threads, e.g. 0,1,13> <profile.bin> [more profiles]
(inside the Switch dev container, for devkitA64's nm)"""
import bisect
import collections
import struct
import subprocess
import sys

elf, wanted, profiles = sys.argv[1], {int(x) for x in sys.argv[2].split(",")}, sys.argv[3:]
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
    return syms[k][1][:80] if k >= 0 else "?"


for path in profiles:
    d = open(path, "rb").read()
    _, ref, _, _ = struct.unpack_from("<8sQQQ", d, 0)
    base = ref - handler
    pcs = collections.defaultdict(collections.Counter)
    waits = collections.defaultdict(collections.Counter)
    total = collections.Counter()
    for i in range((len(d) - 32) // 24):
        th, _, pc, lr = struct.unpack_from("<IIQQ", d, 32 + i * 24)
        if th not in wanted:
            continue
        n = name(pc - base)
        total[th] += 1
        pcs[th][n] += 1
        if n.startswith("svc"):
            waits[th][n + " <- " + name(lr - base)] += 1
    print("=====", path)
    for th in sorted(wanted):
        t = total[th]
        if not t:
            continue
        print("-- thread %d: %d samples" % (th, t))
        for n, v in pcs[th].most_common(8):
            print("   %5.1f%%  %s" % (100.0 * v / t, n))
        for n, v in waits[th].most_common(4):
            print("   wait %5.1f%%  %s" % (100.0 * v / t, n))
