import os
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

class DescriptorReuseTests(unittest.TestCase):
    def test_recorded_state(self):
        tests = Path(__file__).resolve().parent
        source = (tests.parent / "rsx_vulkan_backend.c").read_text()
        function = re.search(r"static void vk_eng_bind_descriptor_set\(.*?\n\}", source, re.S)
        self.assertIsNotNone(function, "shared descriptor binding helper is required")
        end = source.index("} s_ecs;") + len("} s_ecs;")
        start = source.rfind("static struct {", 0, end)
        with tempfile.TemporaryDirectory(prefix="vk-descriptor-reuse-") as directory:
            work = Path(directory)
            (work / "descriptor_under_test.h").write_text(source[start:end] + "\n" + function.group())
            binary = work / "check"
            subprocess.run([os.environ.get("CC", "cc"), "-std=gnu17", "-O2", "-Wall", "-Wextra", "-Werror",
                            "-fsanitize=address,undefined", "-I", str(work), str(tests / "test_descriptor_reuse.c"),
                            "-o", str(binary)], check=True)
            subprocess.run([str(binary)], check=True)

if __name__ == "__main__":
    unittest.main()
