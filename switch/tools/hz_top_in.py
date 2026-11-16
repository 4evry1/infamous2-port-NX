"""Top functions among busy samples whose name matches (or does not match) a pattern, with their callers.
    hz_top_in.py <build.elf> <regex> [--not] <profile.bin> [more]"""
import bisect, collections, re, struct, subprocess, sys
a = sys.argv[1:]; elf, pat = a[0], re.compile(a[1]); neg = "--not" in a; profiles = [x for x in a[2:] if x != "--not"]
syms = []
for line in subprocess.run(["/opt/devkitpro/devkitA64/bin/aarch64-none-elf-nm", "-n", "--defined-only", elf], capture_output=True, text=True).stdout.splitlines():
    p = line.split(None, 2)
    if len(p) == 3 and p[1] in "tTwW": syms.append((int(p[0], 16), p[2]))
syms.sort(); starts = [x for x, _ in syms]
handler = next(x for x, n in syms if n == "__libnx_exception_handler")
def name(o):
    k = bisect.bisect_right(starts, o) - 1
    return syms[k][1] if k >= 0 else "?"
busy = 0; c = collections.Counter(); callers = collections.defaultdict(collections.Counter)
for path in profiles:
    d = open(path, "rb").read(); _, ref, _, _ = struct.unpack_from("<8sQQQ", d, 0); base = ref - handler
    for i in range((len(d) - 32) // 24):
        _, _, pc, lr = struct.unpack_from("<IIQQ", d, 32 + i * 24)
        n = name(pc - base)
        if n.startswith("svc"): continue
        busy += 1
        if bool(pat.search(n)) != neg:
            c[n[:70]] += 1; callers[n[:70]][name(lr - base)[:50]] += 1
for n, v in c.most_common(20):
    top = ", ".join("%s %d%%" % (k, 100 * x // v) for k, x in callers[n].most_common(2))
    print("%5.1f%%  %-60s <- %s" % (100.0 * v / busy, n, top))
