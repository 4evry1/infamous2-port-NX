from pathlib import Path
import subprocess
import tempfile
import unittest
ROOT = Path(__file__).resolve().parents[3]
class PresentBoundary(unittest.TestCase):
    def test_original_waits_are_consumed_by_exactly_one_owner(self):
        source = (ROOT / "engine/libs/video/rsx_vulkan_backend.c").read_text()
        start = source.index("static VkResult vk_present_processed(")
        end = source.index("static void vk_swapchain_lost(", start)
        fixture = (Path(__file__).parent / "present_boundary.c").read_text()
        with tempfile.TemporaryDirectory(prefix="rsx-present-") as tmp:
            path = Path(tmp)
            (path / "production.inc").write_text(source[start:end])
            (path / "test.c").write_text(fixture)
            exe = path / "test"
            subprocess.run(["cc", "-std=c11", "-O1", "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined", "-D__SWITCH__", "-I", str(ROOT / "engine/libs/video"), str(path / "test.c"), "-o", str(exe)], check=True)
            subprocess.run([str(exe)], check=True)

if __name__ == "__main__":
    unittest.main()
