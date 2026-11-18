#!/usr/bin/env python3
"""Extract the production slab transaction; simulate heap pressure and kernel failures."""
from pathlib import Path
import subprocess
import tempfile
root = Path(__file__).resolve().parents[1]
source = (root / "src/hz_mman.c").read_text()
enums = source[source.index("enum {"):source.index("#define HZ_WINDOW_MIN_BYTES")]
start = source.index("static bool slab_acquire(")
end = source.index("/* Put memory behind", start)
with tempfile.TemporaryDirectory(prefix="hz-slab-test-") as tmp:
    path = Path(tmp)
    (path / "production.inc").write_text(enums + source[start:end])
    exe = path / "test"
    subprocess.run(["cc", "-std=c11", "-O1", "-g", "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined", "-I", str(path), str(root / "tests/test_slab_allocation.c"), "-o", str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
