#!/bin/bash
# inFAMOUS 2 for the Nintendo Switch (homebrew NRO): build it from your own copy of the game.
#
#   tools/setup.sh --prepare-only --disc <folder holding PS3_GAME> --rpcs3 <RPCS3 data folder>   (once, see README)
#   SWITCH_DEV_IMAGE=<image> switch/build.sh [all | glslang | lift | compile | link]
#   switch/build.sh sdcard <SD card root or folder> [--program-only]
#
# Steps (all = glslang, lift, compile, link):
#   glslang  glslang cross-built for the Switch -> deps/glslang-install-switch          (container, minutes)
#   lift     the game translated for the Switch -> work/port-switch                     (this machine, 15-60 min)
#   compile  every object compiled for aarch64 -> work/build-switch                     (container, long)
#   link     the Horizon layer + the port linked -> work/build-switch/Infamous2Recomp.nro (container, minutes)
#   sdcard   the card folder switch/infamous2-port laid out under <dir>                 (this machine)
# The container steps run in SWITCH_DEV_IMAGE, a Docker image with devkitA64, libnx and Mesa's NVK Vulkan driver
# for the Switch built into the portlibs (libvulkan.a): see switch/README.md. JOBS sets the job count.
set -eu
R="$(cd "$(dirname "$(readlink -f "$0")")/.." && pwd)"
jobs="${JOBS:-$(( $(nproc) / 2 ))}"; [ "$jobs" -lt 1 ] && jobs=1
in_container() {   # in_container <script> [args...]
    [ -n "${SWITCH_DEV_IMAGE:-}" ] || { echo "== FAILED: set SWITCH_DEV_IMAGE to the Switch build image (switch/README.md)"; exit 1; }
    # The checkout, and the targets of deps/, tools-cache/ and work/ when they are symlinks to elsewhere.
    local mounts=(-v "$R:$R") d t
    for d in deps tools-cache work; do
        t="$(readlink -f "$R/$d" 2>/dev/null || true)"
        case "$t" in ""|"$R"/*) ;; *) mounts+=(-v "$t:$t") ;; esac
    done
    if [ -n "${SWITCH_LSFG_SDK:-}" ]; then
        [ -d "$SWITCH_LSFG_SDK/include" ] && [ -f "$SWITCH_LSFG_SDK/lib/liblsfg-vk.a" ] || {
            echo "== FAILED: SWITCH_LSFG_SDK must contain include/ and lib/liblsfg-vk.a"; exit 1;
        }
        mounts+=(-v "$SWITCH_LSFG_SDK:$SWITCH_LSFG_SDK:ro")
    fi
    docker run --rm --user "$(id -u):$(id -g)" -e HOME=/tmp -e JOBS="$jobs" -e SWITCH_LSFG_SDK "${mounts[@]}" -w "$R" "$SWITCH_DEV_IMAGE" \
        sh "$R/switch/scripts/$1" "${@:2}"
}
step="${1:-all}"
case "$step" in
glslang) in_container glslang.sh ;;
lift)    JOBS="$jobs" bash "$R/switch/scripts/lift.sh" "${@:2}" ;;
compile) in_container compile.sh "$jobs" ;;
link)    in_container link.sh ;;
sdcard)  bash "$R/switch/scripts/sdcard.sh" "${@:2}" ;;
all)
    in_container glslang.sh
    JOBS="$jobs" bash "$R/switch/scripts/lift.sh"
    in_container compile.sh "$jobs"
    in_container link.sh ;;
*) sed -n '2,17p' "$0"; exit 2 ;;
esac
