#!/bin/bash
# Host step (Linux x86-64): translate the game for the Switch into work/port-switch, with the PPU x86-SIMD paths off
# (they emit x86 intrinsics) and the Switch natives list (switch/natives-switch.txt). Needs what
# tools/setup.sh --prepare-only makes: game/, eboot/, tools-cache/llvm, work/venv, deps/.
#   switch/scripts/lift.sh            the lift and the post steps
#   switch/scripts/lift.sh post-only  the post steps only, on an existing lift
# JOBS (default: half the CPU threads, at most 8) sets the lifter's and the compiler's job counts.
set -eu
R="$(cd "$(dirname "$(readlink -f "$0")")/../.." && pwd)"
P="$R/work/port-switch"
fail() { echo "== FAILED: $*"; exit 1; }
[ -x "$R/work/venv/bin/python" ] || fail "no work/venv: run tools/setup.sh --prepare-only first"
[ -x "$R/tools-cache/llvm/bin/clang" ] || fail "no tools-cache/llvm: run tools/setup.sh --prepare-only first"
[ -d "$R/game" ] && [ -d "$R/eboot" ] || fail "no game/ or eboot/: run tools/setup.sh --prepare-only first"
jobs="${JOBS:-$(( $(nproc) / 2 ))}"; [ "$jobs" -gt 8 ] && jobs=8; [ "$jobs" -lt 1 ] && jobs=1
export PORT_LLVM="$R/tools-cache/llvm" PORT_SDL2_DIR="$R/deps/sdl2-cmake" PORT_JOBS="$jobs" PORT_LIFT_JOBS="$jobs"
export PORT_DIR="$P" PORT_SPU_NATIVES_FILE="$R/switch/natives-switch.txt"
export PPU_VMX_SSE=0 PPU_VMX_FAST=0 PPU_VMX_CLEAN=0 PPU_VMX_INT=0 PPU_FP_SINGLE=0 PPU_FP_CVT=0
cd "$R"
if [ "${1:-}" != "post-only" ]; then
    mkdir -p "$R/work/logs"
    echo "== lift for the Switch -> work/port-switch (log: work/logs/lift-switch.log; 15-60 min)"
    "$R/work/venv/bin/python" -I tools/rebuild.py --only inputs,port,firmware,ppu,spu,jobs > "$R/work/logs/lift-switch.log" 2>&1 \
        || fail "the lift (see work/logs/lift-switch.log)"
fi
# The native SPU rewrites as TUs of their own, built like the lifted SPU TUs (the port's CMake globs spu/*/spu_recomp.c).
# job210 and job208 are Switch-only (switch/natives): the PC build compiles every natives/spu/*.c on its own.
for f in natives/spu/job204_native.c natives/spu/edge223_native.c natives/spu/job216_bsp_native.c \
         switch/natives/job210_native.c switch/natives/job208_native.c; do
    W="$P/spu/zz_native_$(basename "$f" .c)"
    mkdir -p "$W"
    printf '%s\n' '/* native SPU rewrite, built like the lifted SPU TUs */' "#include \"$R/$f\"" > "$W/spu_recomp.c"
done
"$R/work/venv/bin/python" -I "$R/switch/scripts/lift_post.py" "$P/spu" || fail "the post check"
echo "== lift ready: next switch/build.sh compile"
