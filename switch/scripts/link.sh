#!/bin/sh
# Container step: compile the Horizon layer (switch/horizon/src) with devkitA64, link the compiled port in
# work/build-switch against it and libnx, and wrap the result as two NROs: Infamous2Recomp.nro (with logs) and
# Infamous2Recomp-quiet.nro (the same program; its file name turns logging off).
set -u
R="$(cd "$(dirname "$0")/../.." && pwd)"
S="$R/switch"
B="$R/work/build-switch"
DKP=/opt/devkitpro
CC=$DKP/devkitA64/bin/aarch64-none-elf-gcc
O="$B/horizon"
E="$R/engine"
[ -f "$B/build.ninja" ] || { echo "== FAILED: no work/build-switch: run switch/build.sh compile first"; exit 1; }
mkdir -p "$O"
objs=""
lsfg_flags=""
if [ -n "${SWITCH_LSFG_SDK:-}" ]; then lsfg_flags="-DHZ_HAS_LSFG=1"; fi

for src in "$S"/horizon/src/*.c; do
    obj="$O/$(basename "$src" .c).o"
    "$CC" -c -O2 -g -std=gnu17 -Wall -Wextra -march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE \
        -ffunction-sections -D__SWITCH__ $lsfg_flags -isystem "$S/horizon/include" -isystem $DKP/libnx/include \
        -isystem $DKP/portlibs/switch/include -I "$E/libs/video" -I "$E/runtime/spu" -I "$E/runtime/ppu" \
        -o "$obj" "$src" || { echo "== FAILED: $src"; exit 1; }
    objs="$objs $obj"
done
if [ -n "${SWITCH_LSFG_SDK:-}" ]; then
    "$DKP/devkitA64/bin/aarch64-none-elf-g++" -c -O2 -g -std=gnu++23 -Wall -Wextra \
        -march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE -ffunction-sections -D__SWITCH__ \
        -isystem "$S/horizon/include" -isystem "$DKP/libnx/include" \
        -isystem "$DKP/portlibs/switch/include" -isystem "$SWITCH_LSFG_SDK/include" \
        -I "$E/libs/video" -o "$O/lsfg_processor.o" "$S/lsfg/lsfg_processor.cpp" || exit 1
    objs="$objs $O/lsfg_processor.o $SWITCH_LSFG_SDK/lib/liblsfg-vk.a"
fi
echo "== Horizon layer compiled:$(echo $objs | wc -w) objects"
# Mesa's libvulkan.a carries its own munmap, posix_memalign and syscall, which collide with the Horizon layer's.
# A private copy with those three weakened lets the layer's versions serve both the engine and the driver.
VK="$O/libvulkan_hz.a"
if [ ! -f "$VK" ] || [ $DKP/portlibs/switch/lib/libvulkan.a -nt "$VK" ]; then
    $DKP/devkitA64/bin/aarch64-none-elf-objcopy --weaken-symbol=munmap --weaken-symbol=posix_memalign \
        --weaken-symbol=syscall $DKP/portlibs/switch/lib/libvulkan.a "$VK" || { echo "== FAILED: weakening libvulkan.a"; exit 1; }
fi
python3 -I "$S/scripts/switch_link.py" "$B" $objs "$VK" -lexpat -lzstd -lz \
    -Wl,--wrap=main -Wl,--wrap=threadCreate -Wl,--wrap=pthread_attr_setstacksize \
    -Wl,--wrap=pthread_mutex_lock -Wl,--wrap=pthread_create -Wl,--wrap=pthread_detach -Wl,--wrap=pthread_exit -Wl,--wrap=open -Wl,--wrap=stat -Wl,--wrap=rename -Wl,--wrap=nwindowQueueBuffer \
    -Wl,--wrap=SDL_OpenAudioDevice -Wl,--wrap=SDL_PauseAudioDevice -Wl,--wrap=SDL_CloseAudioDevice \
    -Wl,--wrap=SDL_QueueAudio -Wl,--wrap=SDL_GetQueuedAudioSize \
    -Wl,--wrap=SDL_InitSubSystem -Wl,--wrap=SDL_QuitSubSystem -Wl,--wrap=SDL_GetCurrentVideoDriver \
    -Wl,--wrap=SDL_CreateWindow -Wl,--wrap=SDL_DestroyWindow -Wl,--wrap=SDL_GetWindowFlags \
    -Wl,--wrap=SDL_GetWindowSize -Wl,--wrap=SDL_Vulkan_GetDrawableSize -Wl,--wrap=SDL_SetWindowFullscreen \
    -Wl,--wrap=SDL_GetWindowDisplayIndex -Wl,--wrap=SDL_GetDesktopDisplayMode -Wl,--wrap=SDL_GetCurrentDisplayMode \
    -Wl,--wrap=SDL_Vulkan_GetInstanceExtensions -Wl,--wrap=SDL_Vulkan_CreateSurface -Wl,--wrap=SDL_PollEvent \
    -Wl,--wrap=SDL_SetRelativeMouseMode -Wl,--wrap=SDL_GetRelativeMouseMode -Wl,--wrap=SDL_SetWindowGrab \
    -Wl,--wrap=SDL_ShowCursor -Wl,--wrap=SDL_WarpMouseInWindow -Wl,--wrap=SDL_CaptureMouse \
    || { echo "== FAILED: link (see work/build-switch/link.log and undefined.txt)"; exit 1; }
if [ ! -f "$B/Infamous2Recomp.elf" ] || [ -s "$B/undefined.txt" ]; then
    echo "== FAILED: link (see work/build-switch/link.log and undefined.txt)"; exit 1
fi
# With a NACP and an icon: launchers that build forwarders refuse an NRO without them. The version shown in the
# homebrew menu is the one compiled in (HZ_VERSION in hz_build.h).
VERSION=$(sed -n 's/^#define HZ_VERSION "\(.*\)"/\1/p' "$S/horizon/include/hz_build.h")
$DKP/tools/bin/nacptool --create "inFAMOUS 2 port" "ps3recomp / Horizon layer" "${VERSION:-hz-unknown}" "$B/Infamous2Recomp.nacp" \
    && $DKP/tools/bin/elf2nro "$B/Infamous2Recomp.elf" "$B/Infamous2Recomp.nro" \
        --nacp="$B/Infamous2Recomp.nacp" --icon=$DKP/libnx/default_icon.jpg || exit 1
$DKP/tools/bin/nacptool --create "inFAMOUS 2 port (no logs)" "ps3recomp / Horizon layer" "${VERSION:-hz-unknown}" "$B/Infamous2Recomp-quiet.nacp" \
    && $DKP/tools/bin/elf2nro "$B/Infamous2Recomp.elf" "$B/Infamous2Recomp-quiet.nro" \
        --nacp="$B/Infamous2Recomp-quiet.nacp" --icon=$DKP/libnx/default_icon.jpg || exit 1
echo "== linked: work/build-switch/Infamous2Recomp.nro and Infamous2Recomp-quiet.nro ($VERSION); next switch/build.sh sdcard <dir>"
