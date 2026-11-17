# inFAMOUS 2 on the Nintendo Switch (homebrew, experimental)

The same native port (the game's PS3 code translated ahead of time into native code, the PS3 system libraries
reimplemented), built for the Nintendo Switch as a homebrew program (`.nro`). It adds a **Horizon layer**
(`switch/horizon`): threads, memory, the screen, audio and input on the Switch's OS, under the port's engine.
Rendering goes through Vulkan on Mesa's NVK driver for the Switch's GPU.

Nothing of the game is in this repository: you build it from **your own copy**, as on PC.

## Status

Playable, slow. Tested on one console with the CPU at 1785 MHz:

- **Frame rate:** about 9-10 fps in light scenes and 7-8 fps in heavy ones. Cutscenes run at real time, at a low
  frame rate. The port is limited by the CPU (all four cores busy), not the GPU: a lower resolution does not help.
- **Audio:** mostly smooth; a crackle in places.
- **Picture:** correct in the areas played (the opening, the first city areas, fights).
- **Known issues:**
  - Low frame rate, with slowdowns in heavy scenes.
  - If the console starts the game at a lower CPU clock (1224 MHz shows in the log), everything runs about 30%
    slower until the clock rises.
  - The first time an effect appears, its shaders are built (a short pause); later starts load them from the card.
- **Runs as:** a title takeover (start the homebrew menu from a game, for the full memory), with Atmosphère.

## What you need

- 💿 **Your copy of inFAMOUS 2** and **RPCS3** with the PS3 firmware, exactly as for the PC port (see the main
  README's *What you need*): the disc folder that holds `PS3_GAME`, and the 1.04 update installed in RPCS3.
- 🐧 **A Linux x86-64 PC** with the PC port's Linux requirements (`tools/setup.sh` lists them), **Docker**, and about
  60 GB free disk space while building.
- 🐳 **A Switch build image** (`SWITCH_DEV_IMAGE`): a Docker image with
  - devkitA64 and libnx (built and tested with libnx `146c3d1`),
  - devkitPro's `nacptool` and `elf2nro`,
  - Mesa's NVK Vulkan driver for the Switch built into the portlibs (`/opt/devkitpro/portlibs/switch/lib/libvulkan.a`
    and its headers; built and tested with [mesa-switch](https://github.com/danfromtico/mesa-switch) `d4a00ea`),
  - CMake, Ninja and Python 3.
- 🎮 **A Switch running Atmosphère**, with a microSD card that has about 20 GB free.

## Build

From a `git clone` of this repository:

```
# 1. Your game, the decrypted 1.04 EBOOT, the build tools and glslang's source: the PC Setup's first six steps.
tools/setup.sh --prepare-only --disc <your game folder> --rpcs3 <RPCS3's data folder>

# 2. glslang for the Switch, the game translated for the Switch (15-60 min), compiled (long) and linked.
SWITCH_DEV_IMAGE=<your Switch build image> switch/build.sh

# 3. The card folder: switch/infamous2-port/ with the program, your EBOOT, settings and game data (~18 GB).
switch/build.sh sdcard <your SD card's root, or any folder to copy to the card>
```

Each step can also be run on its own: `switch/build.sh glslang | lift | compile | link`. `JOBS=<n>` sets the job count.
After a code change, `compile` and `link` are enough; after a change to the engine's SPU or PPU translation, run
`lift` again. `switch/build.sh sdcard <dir> --program-only` copies only the program and EBOOT (for updates).

The result is `work/build-switch/Infamous2Recomp.nro` (with logs) and `Infamous2Recomp-quiet.nro` (the same program,
no log files: its file name turns logging off).

## Play

Start **inFAMOUS 2 port** from the homebrew menu, opened through a game (title takeover). The first start shows a
text screen while it loads; the game follows.

- **Settings:** `switch/infamous2-port/env.txt` on the card (from `switch/env.txt.example`, where each setting is
  explained): native SPU jobs, audio, the frame and game-step limits, profiling.
- **Logs:** `switch/infamous2-port/logs/` (`stderr.log` has the port's status lines every 10 s: frame rate, CPU
  clock, cores busy, audio). They are what to attach to a bug report.

## How it is built

| Part | What it does |
|------|--------------|
| `switch/horizon/` | The Horizon layer: `main`, threads and their priorities, TLS, memory mapping, the window (NVK surface), audio output (audout), input, logs, self-tests of the SPU and VMX float paths, a sampling profiler. |
| `switch/cmake/` | The clang toolchain for aarch64 (devkitA64's headers and libnx) and glslang's lookup for the engine. |
| `switch/scripts/lift.sh` | Translates the game with the PC port's own tools, with the x86 SIMD paths off and `switch/natives-switch.txt`. |
| `switch/natives-switch.txt`, `switch/natives/` | The native SPU job rewrites used on the Switch (Edge Geometry with NEON paths, jobs 201, 204, 208, 209, 210, 211, 216, 218; the start-up data clear of jobs 212 and 210). |
| `switch/scripts/compile.sh`, `link.sh` | Compile with clang, link with devkitA64's g++ and libnx, wrap as NRO. |
| `switch/tools/` | Readers for the console's sampling profiles (`hz_*.py`, run inside the build image) and timeline traces (`ftrace_crit.py`). |

The engine changes for the Switch are in the engine itself, behind `__SWITCH__` (or on by default only there; their
environment switches still work everywhere). A few general ones apply to every build: a two-way SPU branch cache
with a check mode (`PS3_SPU_IBCACHE_VERIFY=1`), debug switches read once instead of on every semaphore, event and
condition call, and an opt-in check mode for the Edge index kernel (`PS3_NATIVE_EDGE_IDX=2`). Some aarch64 code
paths (NEON forms of a few SPU and VMX helpers) apply to any aarch64 build.

## Credits

Built on this repository's port and engine, Thiago's natives (`ThiagoFraga761/infamous2-pc`, MIT), devkitPro, libnx,
Mesa (NVK) and SIMDe.
