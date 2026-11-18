#!/usr/bin/env python3
"""Check the production fast-walker page guard against a 64-bit span model."""
import os
from pathlib import Path
import re
import subprocess
import tempfile

source = (Path(__file__).resolve().parents[1] / "cellGcmSys.c").read_text()
start = source.index("const u32 hi = type == 0 ? method + (count - 1u) * 4u : method;")
end = source.index("if (g < put) s_last_ring_off", start)
guards = re.findall(r"if \(([^;]+)\) break;", source[start:end])
page_guard = next(guard for guard in guards if "g + count * 4u" in guard)
program = r"""
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
static unsigned long long cases;
static int production_reject(uint32_t g, uint32_t count) {
    uint32_t pg = g >> 20;
    return PAGE_GUARD;
}
static void check(uint32_t g, uint32_t count) {
    uint64_t end = (uint64_t)(g & 0xfffffu) + (uint64_t)count * 4u;
    int expected = end >= 0x100000u;
    if (production_reject(g, count) != expected) {
        fprintf(stderr, "wrong guard: g=%08x count=%u\n", g, count);
        abort();
    }
    ++cases;
}
int main(void) {
    const uint32_t pages[] = {0, 0x100000, 0x7ff00000, 0x80000000, 0xfff00000};
    for (unsigned p = 0; p < sizeof(pages)/sizeof(pages[0]); ++p) {
        for (uint32_t off = 0; off < 0x100000; off += 4) {
            check(pages[p] + off, 1);
            check(pages[p] + off, 2047);
        }
        for (uint32_t off = 0xfe000; off < 0x100000; off += 4)
            for (uint32_t count = 1; count <= 2047; ++count)
                check(pages[p] + off, count);
    }
    printf("%llu production FIFO page cases passed\n", cases);
}
""".replace("PAGE_GUARD", page_guard)
with tempfile.TemporaryDirectory(prefix="fifo-fast-page-") as tmp:
    path = Path(tmp)
    (path / "test.c").write_text(program)
    subprocess.run([os.environ.get("CC", "cc"), "-O2", "-Wall", "-Wextra",
                    "-fsanitize=address,undefined", str(path / "test.c"),
                    "-o", str(path / "test")], check=True)
    subprocess.run([str(path / "test")], check=True)
