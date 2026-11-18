import os
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

class TextureHash(unittest.TestCase):
    def test_mutations_and_bounds(self):
        tests = Path(__file__).resolve().parent
        engine = tests.parents[2]
        source_path = Path(os.environ.get("RSX_HASH_SOURCE", tests.parent / "rsx_draw_engine.c"))
        source = source_path.read_text()
        functions = []
        for name in ("eng_texture_content_hash", "eng_texture_sparse_hash", "eng_texture_pack_hash"):
            match = re.search(r"static u64 " + name + r"\(.*?\n\}", source, re.S)
            self.assertIsNotNone(match, name)
            functions.append(match.group())
        with tempfile.TemporaryDirectory(prefix="rsx-texture-hash-") as directory:
            work = Path(directory)
            prefix = '#define XXH_INLINE_ALL\n#define XXH_NAMESPACE ps3recomp_\n#include "' + str(engine / "third_party/xxhash/xxhash.h") + '"\n'
            # Observe the targets of Switch prefetch hints, which sanitizers ignore.
            prefix += "#define __SWITCH__ 1\n#define __builtin_prefetch(addr, ...) record_hint(addr)\n"
            (work / "production.inc").write_text(prefix + "\n".join(functions))
            binary = work / "test"
            subprocess.run([os.environ.get("CC", "cc"), "-std=gnu17", "-O2", "-Wall", "-Wextra", "-Werror",
                            "-fsanitize=address,undefined", "-I", str(work),
                            str(tests / "test_texture_hash.c"), "-o", str(binary)], check=True)
            subprocess.run([str(binary)], check=True)

    def test_pack_reload_identity_and_invalidation(self):
        tests = Path(__file__).resolve().parent
        source = (tests.parent / "rsx_draw_engine.c").read_text()
        functions = []
        for kind, name in (("u64", "eng_texture_pack_hash"), ("u64", "eng_texmod_key"),
                           ("void", "eng_tex_mark_changed"), ("void", "eng_mods_reload")):
            match = re.search(r"static " + kind + " " + name + r"\(.*?\n\}", source, re.S)
            self.assertIsNotNone(match, name)
            functions.append(match.group())
        with tempfile.TemporaryDirectory(prefix="rsx-texture-reload-") as directory:
            work = Path(directory)
            (work / "production.inc").write_text("\n".join(functions))
            binary = work / "test"
            subprocess.run([os.environ.get("CC", "cc"), "-std=gnu17", "-O2", "-Wall", "-Wextra", "-Werror",
                            "-fsanitize=address,undefined", "-I", str(work),
                            str(tests / "test_texture_reload.c"), "-o", str(binary)], check=True)
            subprocess.run([str(binary)], check=True)

if __name__ == "__main__":
    unittest.main()
