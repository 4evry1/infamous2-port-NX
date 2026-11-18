from pathlib import Path
import subprocess
import tempfile
import unittest
ROOT = Path(__file__).resolve().parents[4]
class ProbeTiming(unittest.TestCase):
    def test_measurement_preserves_probe_contract(self):
        source = (ROOT / "engine/libs/video/rsx_draw_engine.c").read_text()
        start = source.index("static eng_vc_entry* eng_vc_probe_measured(")
        end = source.index("\n}\n", start) + 3
        fixture = (Path(__file__).parent / "vertex_probe_timing.c").read_text()
        with tempfile.TemporaryDirectory(prefix="vertex-probe-") as tmp:
            p = Path(tmp)
            (p / "production.inc").write_text(source[start:end])
            (p / "test.c").write_text(fixture)
            subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined", str(p / "test.c"), "-o", str(p / "test")], check=True)
            subprocess.run([str(p / "test")], check=True)
    def test_both_fetch_paths_use_measured_boundary(self):
        source = (ROOT / "engine/libs/video/rsx_draw_engine.c").read_text()
        start = source.index("static void dc_fetch(")
        end = source.index("\n}\n", start)
        fetch = source[start:end]
        self.assertEqual(fetch.count("eng_vc_probe_measured("), 2)
        self.assertNotIn("eng_vc_probe(", fetch)
if __name__ == "__main__": unittest.main()
