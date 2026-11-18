#!/usr/bin/env python3
"""Host gates; wrap in the project's shared heavy lock. No GPU/game proof."""
import argparse
import os
import pathlib
import subprocess


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--output-dir", type=pathlib.Path, required=True)
    parser.add_argument("--seeds", default="1,7,19")
    args = parser.parse_args()
    video = pathlib.Path(__file__).resolve().parent.parent
    engine = video.parent.parent
    out = args.output_dir.resolve()
    out.mkdir(parents=True, exist_ok=True)
    sources = ["rsx_draw_engine.c", "rsx_dispatch.c", "rsx_vertex_compact.c",
               "rsx_vertex_fetch.c", "rsx_texture_layout.c", "rsx_fp_decompiler.c",
               "rsx_vp_decompiler.c", "rsx_button_icons.c", "rsx_wtrack.c", "rsx_split.c"]
    for platform in ("switch", "desktop"):
        flags = ["cc", "-std=gnu11", "-O1", "-g", "-fno-strict-aliasing",
                 "-ffp-contract=off", "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                 "-I" + str(engine / "include"), "-I" + str(video),
                 "-I" + str(engine / "libs/system"), "-I" + str(engine / "runtime/platform")]
        if platform == "switch":
            flags += ["-D__SWITCH__"]
        for name in ("test_sparse_constants", "test_vp_const_usage", "test_rsx_draw_delta"):
            binary = out / (name + "-" + platform)
            inputs = ([video / x for x in sources] + [video / "tests/delta_test_stubs.c"]
                      if name == "test_rsx_draw_delta" else [video / "rsx_vp_decompiler.c"])
            test_flags = flags
            if name == "test_rsx_draw_delta":
                probed = out / ("engine-probed-" + platform + ".c")
                engine_source = (video / "rsx_draw_engine.c").read_text()
                call = "rsx_vertex_decode_index_run(run,"
                assert engine_source.count(call) == 1
                engine_source = engine_source.replace(call, "rsx_test_decode_index_run(run,")
                probed.write_text(
                    '#include "rsx_vertex_compact.h"\n'
                    'static unsigned rsx_test_bounds_calls;\n'
                    'static void rsx_test_decode_index_run(const u8* p, rsx_vertex_ref* r, u32 n, int w, u32 b, rsx_vertex_ref_bounds* bounds) {\n'
                    '    ++rsx_test_bounds_calls; rsx_vertex_decode_index_run(p, r, n, w, b, bounds);\n}\n'
                    'unsigned rsx_test_index_bounds_calls(void) { return rsx_test_bounds_calls; }\n' +
                    engine_source +
                    "\nu32 rsx_test_index_capacity(void) { return g.index_cap; }\n" +
                    "int rsx_test_refs_remapped(void) { return dc.refs_remapped; }\n" +
                    "u32 rsx_test_remap_capacity(void) { return dc.ref_remap.occurrence_capacity; }\n" +
                    "unsigned long long rsx_test_index_cache_bytes(void) { return s_ic_bytes; }\n")
                inputs = [probed if p == video / "rsx_draw_engine.c" else p for p in inputs]
                test_flags = flags + ["-DRSX_TEST_ENGINE_PROBES"]
            command = test_flags + [str(video / "tests" / (name + ".c"))] + list(map(str, inputs)) + ["-lm", "-o", str(binary)]
            result = subprocess.run(command, capture_output=True, text=True)
            (out / (name + "-" + platform + "-build.log")).write_text(result.stdout + result.stderr)
            result.check_returncode()
            env = {k: v for k, v in os.environ.items() if not k.startswith(("PS3RECOMP_", "PS3_TOGGLE", "DELTA_"))}
            # The existing engine harness retains global allocations at exit.
            # Address/UB checks remain active; this is not a leak-check gate.
            env["ASAN_OPTIONS"] = "detect_leaks=0"
            env["UBSAN_OPTIONS"] = "halt_on_error=1"
            if name != "test_rsx_draw_delta":
                result = subprocess.run([str(binary)], env=env, capture_output=True, text=True)
                (out / (name + "-" + platform + ".log")).write_text(result.stdout + result.stderr)
                result.check_returncode()
                print(platform, result.stdout.strip(), flush=True)
                continue
            modes = [({"PS3RECOMP_VK_SPARSE_CONSTANTS": "0"}),
                     ({"PS3RECOMP_VK_SPARSE_CONSTANTS": "1"}),
                     ({"PS3RECOMP_VK_SPARSE_CONSTANTS": "1", "PS3RECOMP_ENG_DELTA": "0"}),
                     ({"PS3RECOMP_VK_SPARSE_CONSTANTS": "1", "PS3RECOMP_ENG_FASTDRAW": "0"}),
                     ({"PS3RECOMP_VK_SPARSE_CONSTANTS": "1", "PS3RECOMP_ENG_DELTA_VERIFY": "1"}),
                     ({"PS3RECOMP_VK_SPARSE_CONSTANTS": "1", "PS3RECOMP_ENG_FASTDRAW_VERIFY": "1"}),
                     ({"PS3RECOMP_VK_SPARSE_CONSTANTS": "1", "PS3RECOMP_ENG_FASTDRAW_VERIFY": "2"}),
                     ({"PS3RECOMP_VK_SPARSE_CONSTANTS": "1", "PS3RECOMP_ENG_LAZY_REFS": "0"}),
                     ({"PS3RECOMP_VK_SPARSE_CONSTANTS": "1", "PS3RECOMP_ENG_TEX_TIMING": "0"}),
                     ({"PS3RECOMP_VK_SPARSE_CONSTANTS": "1", "PS3RECOMP_ENG_DIRECT_TRI_INDICES": "0"}),
                     ({"PS3RECOMP_VK_SPARSE_CONSTANTS": "1", "PS3RECOMP_ENG_INDEX_BOUNDS": "0"})]
            for seed in args.seeds.split(","):
                baseline = None
                for mode, extra in enumerate(modes):
                    log = out / f"engine-{platform}-{seed}-{mode}.log"
                    result = subprocess.run([str(binary), str(log), seed], env=env | extra,
                                            capture_output=True, text=True, timeout=900)
                    diagnostics = result.stdout + result.stderr
                    log.with_suffix(".stderr").write_text(diagnostics)
                    result.check_returncode()
                    assert "[delta-verify]" not in diagnostics and "[fastdraw-verify]" not in diagnostics, diagnostics[-2000:]
                    contents = log.read_bytes()
                    if baseline is None:
                        baseline = contents
                    assert contents == baseline, f"{platform} seed {seed} mode {mode} differs"
                print(f"{platform} seed {seed}: {len(modes)} engine modes byte-identical", flush=True)
                failure_baseline = None
                for direct in ("0", "1"):
                    log = out / f"engine-{platform}-{seed}-vbuf-failure-{direct}.log"
                    failure_env = env | {"PS3RECOMP_VK_SPARSE_CONSTANTS": "1",
                                         "PS3RECOMP_ENG_DIRECT_TRI_INDICES": direct, "DELTA_VBUF_FAIL": "1"}
                    result = subprocess.run([str(binary), str(log), seed], env=failure_env,
                                            capture_output=True, text=True, timeout=900)
                    log.with_suffix(".stderr").write_text(result.stdout + result.stderr)
                    result.check_returncode()
                    contents = log.read_bytes()
                    assert b"vbuf_create_failed " in contents
                    if failure_baseline is None:
                        failure_baseline = contents
                    assert contents == failure_baseline, "direct indices changed failed-buffer fallback"
                print(f"{platform} seed {seed}: failed-buffer fallback byte-identical", flush=True)


if __name__ == "__main__":
    main()
