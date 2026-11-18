# Optional Switch frame generation

Build with `SWITCH_LSFG_SDK` pointing to the pinned native SDK containing `include/` and `lib/liblsfg-vk.a`. Without it the Horizon stub keeps ordinary presentation. The archived hz-0.62 candidate includes the SDK; a later no-SDK acceptance link overwrites only work/build-switch outputs.

On the console, set `PS3RECOMP_LSFG=1` and `PS3RECOMP_VK_PRESENT=fifo` in env.txt. Supply your own `lsfg/Lossless.dll` under the inFAMOUS 2 application directory. Shader data is read by the native backend; Windows machine code is not executed. Neither the DLL nor external SDK is redistributed here. Set LSFG=0 for an immediate rollback of interpolation; restore the previous present mode for a complete baseline comparison.

The engine owns a small C presentation port, the Switch composition selects its implementation, and the adapter borrows the existing device/queue. The renderer serializes each transaction with queue work. Resources are drained and destroyed before replacing the swapchain or device. Once a transaction consumes the original waits it cannot fall back to presenting them again. Acquisition timeout requires reconnection; unsafe in-flight failures terminate rather than free pending resources.

This adapter is adapted from the project's Captain America Switch presenter and history policy. It uses 2x output, performance mode, 25% flow scale, at most 1280x720, and resets history across UI, shutdown hold, or a gap over 250 ms. Capability checks or a missing DLL leave normal presentation active. Logs report real and generated presents separately: generated output is not additional simulated game frames, and CPU processing intervals are not display timestamps.

Run `python3 -m unittest discover -s switch/lsfg/tests -p 'test_*.py' -v`. These tests cover history and the engine handoff, including mutation checks and ASan/UBSan. They do not emulate GPU synchronization or prove console smoothness. Compile/link both with and without the SDK. Hardware acceptance must verify startup, first generated present, sustained output, memory use, image quality and real-frame cost before treating the option as validated.
