#!/bin/sh
# Container step: configure the Switch lift (work/port-switch) with switch/cmake/switch-clang.cmake and compile every
# object into work/build-switch, keeping going past errors. Nothing is linked here: the port's own final clang link of
# Infamous2Recomp is expected to fail (clang's bare-metal driver knows nothing of libnx); switch/scripts/link.sh is
# the real link.
#   compile.sh [jobs]
# PORT_SPU_ARCH carries the SPU_LS_FASTPATH and round-toward-zero-only float paths (spu_helpers.h has the arm64
# float helpers); PORT_LIFT_OPT is plain -O2.
set -u
R="$(cd "$(dirname "$0")/../.." && pwd)"
LIFT="$R/work/port-switch"
B="$R/work/build-switch"
export PORT_LLVM="$R/tools-cache/llvm"
[ -d "$LIFT" ] || { echo "== FAILED: no work/port-switch: run switch/build.sh lift first"; exit 1; }
mkdir -p "$B"
if [ ! -f "$B/build.ninja" ]; then
    cmake -S "$LIFT" -B "$B" -G Ninja \
        -DCMAKE_TOOLCHAIN_FILE="$R/switch/cmake/switch-clang.cmake" -DCMAKE_BUILD_TYPE=Release -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
        -DPS3RECOMP_DIR="$R/engine" -DPython3_EXECUTABLE=/usr/bin/python3 \
        -DPS3RECOMP_RSX_VULKAN=ON -DPS3RECOMP_VULKAN_INCLUDE_DIR=/opt/devkitpro/portlibs/switch/include \
        -Dglslang_DIR="$R/deps/glslang-install-switch/lib/cmake/glslang" \
        -DCMAKE_PROJECT_INCLUDE="$R/switch/cmake/switch-project-include.cmake" \
        "-DPORT_LIFT_OPT=-O2" "-DPORT_SPU_ARCH=-DSPU_LS_FASTPATH=1;-DSPU_FLOAT_RZ_ONLY=1" \
        "-DCMAKE_C_FLAGS_RELEASE=-O2 -DNDEBUG" "-DCMAKE_CXX_FLAGS_RELEASE=-O2 -DNDEBUG" > "$B/configure.log" 2>&1 \
        || { echo "== FAILED: configure (see work/build-switch/configure.log)"; exit 1; }
fi
echo "== compiling (log: work/build-switch/build.log; the first build takes a long while)"
ninja -C "$B" -j "${1:-4}" -k 0 > "$B/build.log" 2>&1
rc=$?
# Only the final link may fail; any other FAILED target is a real error.
other=$(grep '^FAILED: ' "$B/build.log" | grep -v -c 'Infamous2Recomp[^/]*$' || true)
if [ "$other" != "0" ]; then
    grep '^FAILED: ' "$B/build.log" | grep -v 'Infamous2Recomp[^/]*$' | head -n 20
    echo "== FAILED: $other objects did not compile (see work/build-switch/build.log)"
    exit 1
fi
if [ "$rc" != "0" ] && ! grep -q '^FAILED: ' "$B/build.log"; then
    tail -n 20 "$B/build.log"; echo "== FAILED: ninja exit $rc (see work/build-switch/build.log)"; exit 1
fi
echo "== compiled: next switch/build.sh link"
