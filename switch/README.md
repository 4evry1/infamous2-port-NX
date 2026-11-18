# inFAMOUS 2 on the Nintendo Switch (homebrew, experimental)

The same native port (the game's PS3 code translated ahead of time into native code, the PS3 system libraries
reimplemented), built for the Nintendo Switch as a homebrew program (`.nro`). It adds a **Horizon layer**
(`switch/horizon`): threads, memory, the screen, audio and input on the Switch's OS, under the port's engine.
Rendering goes through Vulkan on Mesa's NVK driver for the Switch's GPU.

Nothing of the game is in this repository: you build it from **your own copy**, as on PC.

## Status

**hz-0.116** builds two launchers for Atmosphère title takeover. The opening, bridge reveal and early city are playable. Heavy scenes still have uneven frame pacing and occasional audio crackle. The first appearance of an effect can pause while shaders compile.

A 20-second hardware capture on hz-0.111 measured **8.20 game FPS** in a stationary tower view with 6,720 draws per frame. The median frame took 119.9 ms. The render thread spent about 91 ms per frame processing graphics commands. This run used a 1785 MHz CPU and 960 × 540 internal rendering. The FPS figure counts frames produced by the game and excludes interpolated frames.

Since that capture, the renderer has reduced vertex-cache checking, recycled Vulkan descriptor sets and reused prior uniform-buffer offsets when shader constants match. A nearby tower view on hz-0.114 delivered 7.80 game FPS at 6,618 draws. CPU time spent processing graphics commands fell to 89.4 ms per frame, while time in the frame-clock wait rose to 31.8 ms. The constant-byte check found zero additional matches during the tower trace.

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

The build writes both launchers to `work/build-switch/`. To update an existing installation, keep the two original filenames in `switch/infamous2-port/` so the new launchers replace the old pair.

## Play

Open the homebrew menu through a game for title takeover. Choose **inFAMOUS 2 port** for a diagnostic run or **inFAMOUS 2 port (no logs)** for ordinary play. The logged launcher buffers routine diagnostics in RAM and writes them in batches. Both launchers retain immediate crash reporting. Settings are read from `switch/infamous2-port/env.txt` at startup.

Both launchers show the build version and two FPS readings in the upper-left corner. The HUD was verified during hz-0.115 gameplay on Switch. **BASE** counts completed guest frames. **TOTAL** counts frames accepted by the Switch window, including LSFG output. Both use the same elapsed-time window and refresh about once a second. Set `HZ_FPS_HUD=0` in `env.txt` to hide the display.

The hz-0.116 candidate uses version-based constant reuse by default. To also compare the previous upload byte by byte when versions differ, set `PS3RECOMP_VK_CONST_BYTE_REUSE=1` and relaunch.

### Resolution and optional frame generation

The default internal resolution is 1280 × 720. For the 960 × 540 configuration used in the tower capture, add the following lines to `env.txt`.

```ini
PS3RECOMP_RENDER_SCALE=0.75
PS3RECOMP_UPSCALE=fsr
```

FSR scales each rendered frame to the output size. LSFG can insert an interpolated frame between game frames. It requires the optional native SDK at build time and your own copy of `Lossless.dll` on the SD card. The port can run without either. [LSFG build options and DLL location](lsfg/README.md)

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
