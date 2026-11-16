#!/bin/sh
# Container step: glslang (the guest shader translator, HLSL -> SPIR-V) cross-built for the Switch with devkitA64,
# from the source tools/setup.sh downloaded into deps/glslang-src, into deps/glslang-install-switch.
set -eu
R="$(cd "$(dirname "$0")/../.." && pwd)"
SRC="$R/deps/glslang-src"; B="$R/deps/glslang-build-switch"; I="$R/deps/glslang-install-switch"
if [ -f "$I/lib/cmake/glslang/glslang-config.cmake" ]; then echo "== glslang for the Switch: ready"; exit 0; fi
[ -d "$SRC" ] || { echo "== FAILED: no deps/glslang-src: run tools/setup.sh --prepare-only first"; exit 1; }
F="-march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -ftls-model=local-exec -ffunction-sections -fdata-sections -D__SWITCH__"
cmake -S "$SRC" -B "$B" -G Ninja -DCMAKE_TOOLCHAIN_FILE=/opt/devkitpro/cmake/Switch.cmake -DCMAKE_BUILD_TYPE=Release \
    "-DCMAKE_C_FLAGS=$F" "-DCMAKE_CXX_FLAGS=$F" -DCMAKE_INSTALL_PREFIX="$I" -DBUILD_SHARED_LIBS=OFF -DBUILD_EXTERNAL=OFF \
    -DENABLE_OPT=OFF -DENABLE_GLSLANG_BINARIES=OFF -DGLSLANG_TESTS=OFF -DENABLE_HLSL=ON -DENABLE_SPIRV=ON \
    -DGLSLANG_ENABLE_INSTALL=ON > "$R/deps/glslang-switch.log" 2>&1
cmake --build "$B" -j "${JOBS:-4}" >> "$R/deps/glslang-switch.log" 2>&1
cmake --install "$B" >> "$R/deps/glslang-switch.log" 2>&1
echo "== glslang for the Switch: built (deps/glslang-install-switch)"
