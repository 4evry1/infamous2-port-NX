import os
from pathlib import Path
import subprocess
import tempfile

root=Path(__file__).resolve().parents[3]
with tempfile.TemporaryDirectory(prefix="fifo-ready-") as tmp:
    binary=Path(tmp)/"test"
    subprocess.run([os.environ.get("CC","cc"),"-std=gnu17","-O2","-ffunction-sections","-fdata-sections",
                    "-fsanitize=address,undefined","-pthread","-I",str(root/"include"),
                    str(Path(__file__).with_suffix(".c")),"-Wl,--gc-sections","-o",str(binary)],check=True)
    subprocess.run([str(binary)],check=True,timeout=20)
