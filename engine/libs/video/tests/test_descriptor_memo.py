import os
from pathlib import Path
import subprocess
import tempfile
import unittest
from test_vk_sparse_constants import function_source


class DescriptorMemo(unittest.TestCase):
    def test_placeholder_sampler(self):
        video = Path(__file__).resolve().parent.parent
        source = (video / "rsx_vulkan_backend.c").read_text()
        end = source.index("static VkDescriptorSet vk_batch_alloc_set(void);")
        start = source.rfind("typedef struct {", 0, end)
        fixture = source[start:end]
        for name in ("vk_eng_bind_textures", "vk_eng_image_infos", "vk_eng_batch_set"):
            fixture += "\n" + function_source(source, name) + "\n"
        hash_start = source.index("        u64 dh = 0x9E3779B97F4A7C15ull")
        hash_end = source.index("        dslot =", hash_start)
        fixture += "\nstatic u64 test_recent_hash(void) {\n" + source[hash_start:hash_end] + "return dh;\n}\n"
        with tempfile.TemporaryDirectory(prefix="vk-descriptor-memo-") as directory:
            work = Path(directory)
            (work / "memo_under_test.inc").write_text(fixture)
            binary = work / "test"
            subprocess.run([os.environ.get("CC", "cc"), "-std=gnu17", "-O2", "-Wall", "-Wextra", "-Werror",
                            "-fsanitize=address,undefined", "-I", str(work), "-I", str(video),
                            "-I", str(video.parent.parent / "include"),
                            str(video / "tests/test_descriptor_memo.c"), "-o", str(binary)], check=True)
            for recent in ("0", "1"):
                for volume in ("0", "1", "2"):
                    for intervening_draw in ("0", "1"):
                        for unit in ("0", "7", "15"):
                            subprocess.run([str(binary), recent, volume, intervening_draw, unit], check=True,
                                           env={k: v for k, v in os.environ.items() if not k.startswith("PS3RECOMP_")})


if __name__ == "__main__":
    unittest.main()
