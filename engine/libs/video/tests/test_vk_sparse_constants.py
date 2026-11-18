#!/usr/bin/env python3
"""Run the production Vulkan bind/window functions without a Vulkan device."""
import argparse
import pathlib
import subprocess


def function_source(source, name):
    import re
    match = re.search(r"^static [^\n]*\b" + name + r"\(", source, re.M)
    if not match:
        raise ValueError("missing production function: " + name)
    body = source.index("{", match.start())
    depth, end = 1, body + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[match.start():end]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--output-dir", type=pathlib.Path, required=True)
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    video = pathlib.Path(__file__).resolve().parent.parent
    source = (video / "rsx_vulkan_backend.c").read_text()
    start = source.index("static int s_creuse_on = -1;")
    end = source.index("static void vk_eng_vs_constants_gen", start)
    fixture = source[start:end]
    for name in ("vk_eng_vs_constants_gen", "vk_eng_bind_vs_constants",
                 "vk_bind_vs_range_body", "vk_eng_bind_vs_constants_range", "vk_eng_bind_vs_constants_sparse",
                 "vk_eng_bind_ps_constants", "vk_eng_cwin"):
        function = "\n" + function_source(source, name) + "\n"
        fixture += ("\n#ifdef __SWITCH__\n" + function + "#endif\n"
                    if name.endswith("_sparse") else function)
    args.output_dir.mkdir(parents=True, exist_ok=True)
    (args.output_dir / "vk_sparse_fixture.inc").write_text(fixture)
    for platform in ("switch", "desktop"):
        binary = args.output_dir / ("test_vk_sparse_" + platform)
        command = ["cc", "-std=gnu11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                   "-I", str(video), "-I", str(video.parent.parent / "include"),
                   "-I", str(args.output_dir)]
        if platform == "switch":
            command += ["-D__SWITCH__"]
        if args.sanitize:
            command += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
        command += [str(video / "tests/test_vk_sparse_constants.c"), "-o", str(binary)]
        subprocess.run(command, check=True)
        for reuse, byte_reuse in (("1", "unset"), ("1", "1"), ("1", "0"), ("0", "1")):
            subprocess.run([str(binary), reuse, byte_reuse], check=True)


if __name__ == "__main__":
    main()
