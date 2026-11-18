"""Route the title's two FIFO publication stores through the instrumented accessor."""
from pathlib import Path
import re
import sys

def instrument(directory):
    counts = {"0013AC80": 0, "0013ACE0": 0}
    sites = re.compile(r"(?P<prefix>r11 = vm_read32\(r9 \+ 0x0\);\n        )"
                       r"(?:vm_write32|\(vm_write32\))(?P<args>\(r11 \+ 0x0, r10\);\nloc_(?P<site>0013AC80|0013ACE0):)")
    edits = []
    for path in directory.glob("ppu_recomp_*.cpp"):
        original = path.read_text()
        def redirect(match):
            counts[match["site"]] += 1
            return match["prefix"] + "(vm_write32)" + match["args"]
        updated = sites.sub(redirect, original)
        if updated != original:
            edits.append((path, updated))
    if not all(counts.values()):
        raise SystemExit(f"FIFO publication sites missing from this lift: {counts}")
    for path, updated in edits:
        path.write_text(updated)
    print(f"FIFO publication stores matched: {counts}")

if __name__ == "__main__":
    instrument(Path(sys.argv[1]))
