import os
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

class ArrayReferences(unittest.TestCase):
    def test_append_bounds_and_failure(self):
        tests = Path(__file__).resolve().parent
        source = (tests.parent / "rsx_draw_engine.c").read_text()
        functions = []
        for name in ("dc_reserve_refs", "dc_append_array_refs"):
            match = re.search(r"static int " + name + r"\(.*?\n\}", source, re.S)
            self.assertIsNotNone(match, name)
            functions.append(match.group())
        with tempfile.TemporaryDirectory(prefix="rsx-array-refs-") as directory:
            work = Path(directory)
            (work / "production.inc").write_text("\n".join(functions))
            binary = work / "test"
            subprocess.run([os.environ.get("CC", "cc"), "-std=gnu17", "-O2", "-Wall", "-Wextra", "-Werror",
                            "-fsanitize=address,undefined", "-I", str(work),
                            str(tests / "test_array_refs.c"), "-o", str(binary)], check=True)
            subprocess.run([str(binary)], check=True)

if __name__ == "__main__":
    unittest.main()
