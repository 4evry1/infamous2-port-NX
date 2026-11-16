#!/bin/bash
# Host step: lay out the SD card folder switch/infamous2-port/ under <dir> (a mounted SD card's root, or any folder
# you copy to the card yourself), from what this checkout built and prepared:
#   Infamous2Recomp.nro, Infamous2Recomp-quiet.nro   the program (work/build-switch)
#   EBOOT.elf                                        your decrypted 1.04 EBOOT (eboot/EBOOT_upd104.elf)
#   env.txt                                          settings (switch/env.txt.example; an existing one is kept)
#   game/disc/PS3_GAME, game/dev_hdd0, game/dev_flash, game/gamedata   your game, update and firmware files (game/)
#   sdcard.sh <dir> [--program-only]   --program-only: the NROs and EBOOT.elf only (the game data is ~18 GB)
set -eu
R="$(cd "$(dirname "$(readlink -f "$0")")/../.." && pwd)"
[ $# -ge 1 ] || { echo "usage: $0 <SD card root or folder> [--program-only]"; exit 2; }
D="$1/switch/infamous2-port"
B="$R/work/build-switch"
fail() { echo "== FAILED: $*"; exit 1; }
[ -f "$B/Infamous2Recomp.nro" ] || fail "no work/build-switch/Infamous2Recomp.nro: run switch/build.sh link first"
[ -f "$R/eboot/EBOOT_upd104.elf" ] || fail "no eboot/EBOOT_upd104.elf: run tools/setup.sh --prepare-only first"
mkdir -p "$D"
cp -f "$B/Infamous2Recomp.nro" "$B/Infamous2Recomp-quiet.nro" "$D/"
cp -f "$R/eboot/EBOOT_upd104.elf" "$D/EBOOT.elf"
[ -f "$D/env.txt" ] || cp "$R/switch/env.txt.example" "$D/env.txt"
echo "== program: $D"
if [ "${2:-}" = "--program-only" ]; then exit 0; fi
mkdir -p "$D/game/disc"
copy() {   # copy <source dir> <destination parent>; never deletes anything already on the card
    local name; name="$(basename "$1")"
    if [ -e "$2/.$name.done" ]; then echo "  skip $name (already copied)"; return; fi
    if [ -e "$2/$name" ]; then
        # There already, but not marked as finished by this script: an earlier copy, or one made by hand. It may
        # hold save data (dev_hdd0), so it is left alone; delete it yourself to have it copied again.
        echo "  skip $name (already there; delete $2/$name to copy it again)"; return
    fi
    [ -d "$1" ] || fail "no $1"
    echo "  copying $name ($(du -shL "$1" | cut -f1))"
    rm -rf "${2:?}/.$name.part"   # only this script's own unfinished copy
    cp -rL "$1" "$2/.$name.part" && mv "$2/.$name.part" "$2/$name" && touch "$2/.$name.done"
}
copy "$R/game/disc/PS3_GAME" "$D/game/disc"
copy "$R/game/dev_hdd0" "$D/game"
copy "$R/game/dev_flash" "$D/game"
[ -d "$R/game/gamedata" ] && copy "$R/game/gamedata" "$D/game"
echo "== SD card folder ready: $D"
