import os
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

class TopologyTriangles(unittest.TestCase):
    def test_equivalence_and_bounds(self):
        tests = Path(__file__).resolve().parent
        source = (tests.parent / "rsx_draw_engine.c").read_text()
        functions = []
        for prefix, name in [("static u32", "eng_topology_vertex"), ("void", "rsx_draw_engine_write_topology_indices")]:
            match = re.search(prefix + " " + name + r"\(.*?\n\}", source, re.S)
            self.assertIsNotNone(match)
            functions.append(match.group())
        with tempfile.TemporaryDirectory(prefix="rsx-topology-") as directory:
            work = Path(directory)
            (work / "production.inc").write_text("\n".join(functions))
            binary = work / "test"
            subprocess.run([os.environ.get("CC", "cc"), "-std=gnu17", "-O2", "-Wall", "-Wextra", "-Werror",
                            "-fsanitize=address,undefined", "-I", str(work), "-I", str(tests.parent),
                            str(tests / "test_topology_triangles.c"), "-o", str(binary)], check=True)
            subprocess.run([str(binary)], check=True)

if __name__ == "__main__":
    unittest.main()
