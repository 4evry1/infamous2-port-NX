import os
from pathlib import Path
import subprocess
import tempfile
import unittest

class InstructionBurst(unittest.TestCase):
    def test_reference_state_and_bounds(self):
        tests = Path(__file__).resolve().parent
        engine = tests.parents[2]
        with tempfile.TemporaryDirectory(prefix="rsx-instruction-burst-") as directory:
            binary = Path(directory) / "check"
            subprocess.run([os.environ.get("CC", "cc"), "-std=gnu17", "-O2", "-ffp-contract=off",
                            "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter", "-Wno-unknown-pragmas",
                            "-fsanitize=address,undefined", "-I", str(engine / "include"),
                            str(tests / "test_instruction_burst.c"), "-o", str(binary)], check=True)
            subprocess.run([str(binary)], check=True)

if __name__ == "__main__":
    unittest.main()
