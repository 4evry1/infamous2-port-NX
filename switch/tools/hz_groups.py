"""Busy profile samples grouped by owner: each lifted SPU module (jobmod_/relocmod_/libsre_/wwsjob_
prefix), the guest PPU code (func_), the SPU runtime (spu_*, mfc_*, gm_*), the renderer (rsx_*, eng_*,
vk*), and the rest. Leaf helpers are charged to the group of their caller (lr) when the caller is lifted.

    hz_groups.py <build.elf> <profile.bin> [more]   (inside the Switch dev container)"""
import bisect, collections, re, struct, subprocess, sys
elf, profiles = sys.argv[1], sys.argv[2:]
syms = []
for line in subprocess.run(["/opt/devkitpro/devkitA64/bin/aarch64-none-elf-nm", "-n", "--defined-only", elf],
                           capture_output=True, text=True).stdout.splitlines():
    p = line.split(None, 2)
    if len(p) == 3 and p[1] in "tTwW":
        syms.append((int(p[0], 16), p[2]))
syms.sort(); starts = [a for a, _ in syms]
handler = next(a for a, n in syms if n == "__libnx_exception_handler")
def name(off):
    k = bisect.bisect_right(starts, off) - 1
    return syms[k][1] if k >= 0 else "?"
def group(n):
    m = re.match(r"(jobmod_[0-9a-f]+_[0-9a-f]+|relocmod_[0-9a-f]+|libsre_\w+?_[0-9A-F]+|wwsjob_\w+?_[0-9A-F]{8})_spu_func", n)
    if m: return m.group(1)
    if "func_" in n and "ppu_context" in n: return "PPU guest code"
    if n.startswith(("spu_", "mfc_", "gm_", "_ZL11vmx", "_ZL12vmx")) or "spu__" in n: return "runtime: " + ("vmx" if "vmx" in n else "spu/mfc")
    if n.startswith(("rsx_", "eng_", "vk", "burst_", "sink_", "gcm_")): return "renderer"
    return "other"
busy = 0; g = collections.Counter(); lifted = collections.Counter()
for path in profiles:
    d = open(path, "rb").read(); _, ref, _, _ = struct.unpack_from("<8sQQQ", d, 0); base = ref - handler
    for i in range((len(d) - 32) // 24):
        _, _, pc, lr = struct.unpack_from("<IIQQ", d, 32 + i * 24)
        n = name(pc - base)
        if n.startswith("svc"): continue
        busy += 1
        k = group(n)
        if k.startswith("runtime"):
            c = group(name(lr - base))
            if c.startswith(("jobmod", "relocmod", "libsre", "wwsjob")): k = c + " (+helpers)"; lifted[c] += 1
        g[k.replace(" (+helpers)", "")] += 1
print(busy, "busy samples")
for k, v in g.most_common(22):
    print("%5.1f%%  %s%s" % (100.0 * v / busy, k, "  (of which helpers %.1f%%)" % (100.0 * lifted[k] / busy) if lifted[k] else ""))
