"""Post step of the Switch lift (switch/scripts/lift.sh): check that every native in switch/natives-switch.txt got
its hook in the lifted SPU modules, and add the Edge index kernel's check call.

    python -I switch/scripts/lift_post.py <work/port-switch/spu>

The index kernel's check run (PS3_NATIVE_EDGE_IDX=2, natives/spu/edge223_native.c) compares the native's result
with what the lifted 0xC7B8 left; the compare runs after each of the two lifted calls of 0xC7B8 returns, so a
call to edge_idx_check_after is added after both (idempotent)."""
import os
import sys

spu = sys.argv[1]
want = {   # module -> the hooks (spu_native_entry) its lift must carry
    "jobmod_3af9ed8fe0d4_e14ff0": ["0x12C60u"],
    "jobmod_d0d15ec83992_ea2a0": ["0xD0A8u", "0xA3C8u", "0xA798u", "0x4448u", "0x99E8u", "0xC7B8u"],
    "jobmod_867e5d2348b3_e2898": ["0x4358u", "0x698Cu"],
    "jobmod_7393701cbd39_e1390": ["0x4030u"],
    "jobmod_867e48f161e0_e10e8": ["0x4030u"],
    "jobmod_87926738de5c_e4640": ["0x45F8u"],
    "jobmod_9cb47b0cc0eb_e60c0": ["0x68D8u"],
    "jobmod_87c8e5f63136_e2f28": ["0x701Cu"],
    "jobmod_ab654d30fd90_efe70": ["0x16320u", "0x17280u", "0x1C510u", "0x17780u", "0x18B80u", "0x5814u"],
}
bad = []
for mod, addrs in want.items():
    p = os.path.join(spu, mod, "spu_recomp.c")
    if not os.path.exists(p):
        bad.append("no lifted module " + mod)
        continue
    src = open(p, errors="replace").read()
    for a in addrs:
        if "if (spu_native_entry(ctx, %s))" % a not in src:
            bad.append("%s: no hook at %s" % (mod, a))
if bad:
    sys.exit("FAIL: " + "; ".join(bad))

edge = os.path.join(spu, "jobmod_d0d15ec83992_ea2a0", "spu_recomp.c")
src = open(edge, errors="replace").read()
call = "jobmod_d0d15ec83992_ea2a0_spu_func_0000C7B8(ctx); spu_drain_call(ctx, 0xD0A0);"
check = " { extern void edge_idx_check_after(spu_context*); edge_idx_check_after(ctx); }"
if check not in src:
    n = src.count(call)
    if n != 2:
        sys.exit("FAIL: expected two lifted calls of 0xC7B8, found %d" % n)
    src = src.replace(call, call + check)
    open(edge, "w").write(src)
print("natives hooked:", {m: len(a) for m, a in want.items()}, "; index check calls:", src.count(check))
