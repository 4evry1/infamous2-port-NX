import os
from pathlib import Path
import subprocess
import tempfile

engine = Path(__file__).resolve().parents[3]
with tempfile.TemporaryDirectory(prefix="fp-collector-") as tmp:
    binary = Path(tmp) / "test"
    subprocess.run([os.environ.get("CC", "cc"), "-std=gnu17", "-O2", "-fno-strict-aliasing", "-frounding-math",
                    "-ffp-contract=off", "-ffunction-sections", "-fdata-sections", "-fsanitize=address,undefined",
                    "-I", str(engine / "include"), str(Path(__file__).with_suffix(".c")),
                    str(engine / "libs/video/rsx_fp_decompiler.c"), "-Wl,--gc-sections", "-lm", "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
