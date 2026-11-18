import os
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

class ContentHashTests(unittest.TestCase):
    def test_changed_content_and_bounded_reads(self):
        tests = Path(__file__).resolve().parent
        engine = tests.parents[2]
        source = (tests.parent / "rsx_draw_engine.c").read_text()
        match = re.search(r"static u64 eng_hash_span\(.*?\n\}", source, re.S)
        self.assertIsNotNone(match)
        with tempfile.TemporaryDirectory(prefix="rsx-content-hash-") as directory:
            work = Path(directory)
            prefix = ""
            header = engine / "third_party/xxhash/xxhash.h"
            if header.exists():
                prefix = '#define XXH_INLINE_ALL\n#define XXH_NAMESPACE ps3recomp_\n#include "' + str(header) + '"\n'
            prefix += '#include "' + str(tests.parent / "rsx_hash_seeds.h") + '"\n'
            (work / "span_hash_under_test.h").write_text(prefix + match.group() + "\n")
            for mode, extra_flags in (("native", []), ("scalar", ["-DXXH_VECTOR=0"])):
                executable = work / ("check-" + mode)
                subprocess.run([os.environ.get("CC", "cc"), "-O2", "-Wall", "-Wextra", "-Werror",
                                "-fsanitize=address,undefined", "-I", str(work)] + extra_flags +
                                [str(tests / "test_content_hash.c"), "-o", str(executable)], check=True)
                subprocess.run([str(executable)], check=True)

if __name__ == "__main__":
    unittest.main()
