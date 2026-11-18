#!/usr/bin/env python3
import argparse
from pathlib import Path
import subprocess

parser = argparse.ArgumentParser()
parser.add_argument("--output-dir", type=Path, required=True)
args = parser.parse_args()
source = Path(__file__).resolve().parents[1] / "sys_memory.c"
text = source.read_text()
start = text.index("sys_mem_alloc_info       g_sys_mem_allocs")
end = text.index("\n}", text.index("int64_t sys_memory_free(")) + 2
fixture = text[start:end].replace('#include <pthread.h>', '')
args.output_dir.mkdir(parents=True, exist_ok=True)
(args.output_dir / "memory_fixture.inc").write_text(fixture)
binary = args.output_dir / "memory_allocation"
subprocess.run(["cc", "-D__SWITCH__", "-std=gnu11", "-Wall", "-Wextra", "-Werror", "-g",
                "-fsanitize=address,undefined", "-pthread", "-I", str(args.output_dir),
                str(source.parent / "tests/test_memory_allocation.c"), "-o", str(binary)], check=True)
for reuse in ("1", "0"):
    subprocess.run([str(binary), reuse], check=True)
