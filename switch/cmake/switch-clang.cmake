# Switch toolchain for the port's CMake project: clang (the port's own tools-cache/llvm, needed for the PPU
# lift's musttail) targeting aarch64 with devkitA64's newlib/libstdc++ headers and libnx. It compiles every
# object; the final link is switch/scripts/link.sh (devkitA64 g++ + libnx + the Horizon layer), because clang's
# bare-metal driver knows nothing of libnx's crt0 and specs.
# Used inside the Switch build container (switch/build.sh); PORT_LLVM points at tools-cache/llvm.
set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR aarch64)
set(DKP /opt/devkitpro)
set(CMAKE_C_COMPILER   "$ENV{PORT_LLVM}/bin/clang")
set(CMAKE_CXX_COMPILER "$ENV{PORT_LLVM}/bin/clang++")
set(CMAKE_AR           "$ENV{PORT_LLVM}/bin/llvm-ar")
set(CMAKE_RANLIB       "$ENV{PORT_LLVM}/bin/llvm-ranlib")
set(CMAKE_C_COMPILER_TARGET   aarch64-none-elf)
set(CMAKE_CXX_COMPILER_TARGET aarch64-none-elf)
set(CMAKE_SYSROOT ${DKP}/devkitA64/aarch64-none-elf)
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
# The Horizon layer (switch/horizon): headers newlib+libnx lack, and a force-included prelude.
set(HZ ${CMAKE_CURRENT_LIST_DIR}/../horizon)
# The devkitA64 GCC version whose libstdc++ headers clang uses (SWITCH_GCC_VERSION, default 15.2.0).
if(DEFINED ENV{SWITCH_GCC_VERSION})
    set(_gccv "$ENV{SWITCH_GCC_VERSION}")
else()
    set(_gccv "15.2.0")
endif()
# -fno-math-errno: fma / sqrt / sqrtf become the FMADD / FSQRT instructions instead of libm calls (the same
# results; about 3,100 call sites in the build). The game never reads the C errno of a math call.
set(_common "--target=aarch64-none-elf --sysroot=${DKP}/devkitA64/aarch64-none-elf --no-default-config -march=armv8-a+crc+crypto -mtune=cortex-a57 -fno-math-errno -fPIE -D__SWITCH__ -isystem ${HZ}/include -include ${HZ}/horizon_pre.h -isystem ${DKP}/libnx/include -isystem ${DKP}/portlibs/switch/include")
set(CMAKE_C_FLAGS_INIT   "${_common}")
set(CMAKE_CXX_FLAGS_INIT "${_common} -isystem ${DKP}/devkitA64/aarch64-none-elf/include/c++/${_gccv} -isystem ${DKP}/devkitA64/aarch64-none-elf/include/c++/${_gccv}/aarch64-none-elf")
# glslang cross-built for the Switch (switch/scripts/glslang.sh -> deps/glslang-install-switch): the guest
# shader translator.
set(CMAKE_FIND_ROOT_PATH ${DKP}/portlibs/switch ${DKP}/libnx
    ${CMAKE_CURRENT_LIST_DIR}/../../deps/glslang-install-switch)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
