import os
from pathlib import Path
import subprocess
import tempfile
import unittest

class PackedFetchTests(unittest.TestCase):
    def test_reference_equivalence_and_bounds(self):
        tests = Path(__file__).resolve().parent
        video = tests.parent
        engine = tests.parents[2]
        with tempfile.TemporaryDirectory(prefix="rsx-cmp-fetch-") as directory:
            for portable in (False, True):
                for name in ("test_vertex_fetch", "test_cmp_fetch", "test_vertex_ranges", "test_vertex_remap", "test_vertex_index_bounds", "test_vertex_plan_init", "test_cmp_sign", "test_divided_fetch"):
                    with self.subTest(portable=portable, name=name):
                        binary = Path(directory) / (name + str(portable))
                        command = [os.environ.get("CC", "cc"), "-std=gnu17", "-O2", "-ffp-contract=off",
                                   "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter", "-Wno-unknown-pragmas",
                                   "-fsanitize=address,undefined", "-I", str(engine / "include"), "-I", str(video)]
                        if name in ("test_cmp_sign", "test_divided_fetch"):
                            command += ["-frounding-math"]
                        if portable:
                            command += ["-U__SSE2__", "-U_M_X64", "-U_M_AMD64"]
                        command += [str(tests / (name + ".c")), str(video / "rsx_vertex_compact.c"),
                                    str(video / "rsx_dispatch.c"), "-lm", "-o", str(binary)]
                        subprocess.run(command, check=True)
                        subprocess.run([str(binary)], check=True)

if __name__ == "__main__":
    unittest.main()
