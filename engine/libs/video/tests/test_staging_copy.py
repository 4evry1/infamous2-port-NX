import os
from pathlib import Path
import subprocess
import tempfile
import unittest


class StagingCopy(unittest.TestCase):
    def test_bytes_and_bounds(self):
        tests = Path(__file__).resolve().parent
        with tempfile.TemporaryDirectory(prefix="vk-staging-copy-") as directory:
            for mode in ("desktop", "switch"):
                binary = Path(directory) / mode
                command = [os.environ.get("CC", "cc"), "-std=gnu17", "-O2", "-Wall", "-Wextra", "-Werror",
                           "-fsanitize=address,undefined", "-I", str(tests.parent),
                           str(tests / "test_staging_copy.c"), "-o", str(binary)]
                if mode == "switch":
                    command.append("-D__SWITCH__")
                subprocess.run(command, check=True)
                subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    unittest.main()
