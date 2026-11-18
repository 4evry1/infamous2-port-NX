import os
from pathlib import Path
import subprocess
import tempfile
import unittest


class ShadowIteration(unittest.TestCase):
    def test_sampled_units(self):
        video = Path(__file__).resolve().parent.parent
        source = (video / "rsx_draw_engine.c").read_text()
        start = source.index("    const u32 sampled_units = fixed ? 0 : fp->sampled_units;")
        end = source.index("    if (s_texgen_on", start)
        harness = r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "rsx_dispatch.h"
#include "rsx_fp_decompiler.h"
typedef struct { u32 sampled_units; } Program;
typedef struct { u32 mask; rsx_fp_shadow_desc descriptors[16]; } Result;
static Result evaluate(const rsx_dsp_texture *tex, const Program *fp, int fixed) {
    u32 shadow_mask = 0;
    rsx_fp_shadow_desc shadow[16] = {{0}};
BODY
    Result result = {shadow_mask, {{0}}};
    memcpy(result.descriptors, shadow, sizeof shadow);
    return result;
}
static u32 seed = 0x98765432;
static u32 next(void) { seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5; return seed; }
int main(void) {
    unsigned enabled = 0;
    for (u32 mask = 0; mask < 65536; ++mask) {
        Program program = {mask | (next() & 0xffff0000u)};
        rsx_dsp_texture tex[16] = {{0}};
        rsx_fp_set_shadow_rsx_order(mask & 1u);
        rsx_fp_set_shadow_d24((mask >> 1) % 3u);
        rsx_fp_set_shadow_soft((mask >> 3) & 3u);
        for (u32 unit = 0; unit < 16; ++unit) {
            tex[unit].format = 0x90 | (next() & 7);
            tex[unit].dimension = 1 + next() % 3;
            tex[unit].cubemap = next() & 1;
            tex[unit].mipmaps = 1 + next() % 4;
            tex[unit].filter = next();
            tex[unit].wrap = next();
            tex[unit].control0 = next();
        }
        for (int fixed = 0; fixed <= 1; ++fixed) {
            Result actual = evaluate(tex, &program, fixed);
            u32 expected_mask = 0;
            for (u32 unit = 0; unit < 16; ++unit) {
                rsx_fp_shadow_desc expected = {0};
                const rsx_dsp_texture *t = &tex[unit];
                if (!fixed && ((program.sampled_units >> unit) & 1u))
                    rsx_fp_shadow_desc_from_texture(&expected, t->format, t->dimension,
                        t->cubemap, t->mipmaps, t->filter, t->wrap, t->control0);
                assert(!memcmp(&expected, &actual.descriptors[unit], sizeof expected));
                if (expected.enabled) { expected_mask |= 1u << unit; ++enabled; }
            }
            assert(expected_mask == actual.mask);
        }
    }
    assert(enabled);
    printf("131072 shadow iteration cases; %u enabled descriptors\n", enabled);
}
'''.replace("BODY", source[start:end])
        with tempfile.TemporaryDirectory(prefix="rsx-shadow-iteration-") as directory:
            work = Path(directory)
            (work / "test.c").write_text(harness)
            subprocess.run([os.environ.get("CC", "cc"), "-std=gnu17", "-O2",
                            "-fsanitize=address,undefined", "-ffunction-sections", "-fdata-sections",
                            "-I", str(video), "-I", str(video.parent.parent / "include"),
                            str(work / "test.c"), str(video / "rsx_fp_decompiler.c"),
                            "-Wl,--gc-sections", "-lm", "-o", str(work / "test")], check=True)
            subprocess.run([str(work / "test")], check=True)


if __name__ == "__main__":
    unittest.main()
