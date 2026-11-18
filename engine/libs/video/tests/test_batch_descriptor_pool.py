from pathlib import Path
import subprocess
import tempfile
import unittest
from test_vk_sparse_constants import function_source


class BatchSets(unittest.TestCase):
    def test_slot_lifetime(self):
        tests = Path(__file__).resolve().parent
        src = (tests.parent / "rsx_vulkan_backend.c").read_text()
        end = src.index("} vk_batch_set_pool;") + len("} vk_batch_set_pool;")
        start = src.rindex("typedef struct {", 0, end)
        allocator = src[src.index("static VkDescriptorSet vk_batch_alloc_set(void)\n{"):]
        fixture = src[start:end] + "\nstatic vk_batch_set_pool s_batch_sets[2];\n"
        fixture += function_source(src, "vk_batch_complete_sets") + "\n" + function_source(allocator, "vk_batch_alloc_set")
        fixture += "\n" + function_source(src, "vk_eng_write_set")
        with tempfile.TemporaryDirectory() as tmp:
            work = Path(tmp)
            (work/"pool.inc").write_text(fixture)
            for flags in ([], ["-D__SWITCH__"]):
                subprocess.run(["cc", "-std=gnu17", "-O2", "-Wall", "-Wextra", "-Werror",
                                "-fsanitize=address,undefined", *flags, "-I", str(work), "-I", str(tests.parent.parent.parent / "include"),
                                str(tests/"test_batch_descriptor_pool.c"), "-o", str(work/"test")], check=True)
                subprocess.run([str(work/"test")], check=True)


if __name__ == "__main__":
    unittest.main()
