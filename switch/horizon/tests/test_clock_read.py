"""Exercise the production read-only clock helper and service cleanup."""
from pathlib import Path
import subprocess
import tempfile
import unittest
ROOT = Path(__file__).resolve().parents[3]
class ClockRead(unittest.TestCase):
    def test_success_and_service_failures(self):
        source = (ROOT / "switch/horizon/src/hz_entry.c").read_text()
        start = source.index("static unsigned module_clock_mhz(")
        end = source.index("static unsigned cpu_mhz(", start)
        fixture = (Path(__file__).parent / "clock_read.c").read_text()
        with tempfile.TemporaryDirectory(prefix="hz-clocks-") as tmp:
            path = Path(tmp)
            (path / "production.inc").write_text(source[start:end])
            (path / "test.c").write_text(fixture)
            subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined", str(path / "test.c"), "-o", str(path / "test")], check=True)
            subprocess.run([str(path / "test")], check=True)
if __name__ == "__main__": unittest.main()
