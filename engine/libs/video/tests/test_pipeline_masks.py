import os
from pathlib import Path
import subprocess
import tempfile
import unittest


class PipelineMasks(unittest.TestCase):
    def test_texture_masks(self):
        video = Path(__file__).resolve().parent.parent
        source = (video / "rsx_draw_engine.c").read_text()
        start = source.index("    if (!fixed) for", source.index("static u32 eng_pipeline_get"))
        end = source.index("    /* Depth textures with comparison", start)
        body = source[start:end]
        harness = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "rsx_dispatch.h"
#include "rsx_texture_layout.h"
typedef struct { u32 gamma_cand, expand_cand; } Candidates;
typedef struct { u32 gamma_units, expand_units; u8 gamma[16], expand[16]; } Result;
static Result evaluate(const rsx_dsp_texture *tex, const Candidates *fp, int fixed) {
    u32 gamma_units = 0, expand_units = 0;
    u8 gamma_masks[16] = {0}, expand_masks[16] = {0};
BODY
    Result result = {gamma_units, expand_units, {0}, {0}};
    memcpy(result.gamma, gamma_masks, sizeof gamma_masks);
    memcpy(result.expand, expand_masks, sizeof expand_masks);
    return result;
}
static uint32_t seed = 1234567;
static uint32_t next(void) { seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5; return seed; }
int main(void) {
    unsigned nonzero_gamma = 0, nonzero_expand = 0;
    for (unsigned enabled = 0; enabled < 65536; ++enabled) {
        rsx_dsp_texture tex[16] = {{0}};
        Candidates cases[] = {{next(), next()}, {0, 0}, {0xffff0000u, 0xffff0000u},
                              {0xffffffffu, 0xffffffffu}, {0x5555, 0xaaaa}, {1, 0x8000}};
        for (unsigned unit = 0; unit < 16; ++unit) {
            tex[unit].enabled = (enabled >> unit) & 1;
            tex[unit].format = 0x80 | (next() & 31);
            tex[unit].remap = next(); tex[unit].filter = next(); tex[unit].wrap = next();
        }
        for (unsigned c = 0; c < sizeof cases / sizeof cases[0]; ++c) {
            Candidates candidates = cases[c];
            for (int fixed = 0; fixed <= 1; ++fixed) {
                Result actual = evaluate(tex, &candidates, fixed);
                u32 gamma_units = 0, expand_units = 0;
                for (unsigned unit = 0; unit < 16; ++unit) {
                    u8 gamma = 0, expand = 0;
                    const rsx_dsp_texture *t = &tex[unit];
                    if (!fixed && t->enabled) {
                        if ((candidates.gamma_cand >> unit) & 1)
                            gamma = rsx_texture_gamma_mask(t->format, t->remap, t->filter, t->wrap);
                        if ((candidates.expand_cand >> unit) & 1)
                            expand = rsx_texture_expand_mask(t->format, t->remap, t->filter, t->wrap);
                    }
                    assert(actual.gamma[unit] == gamma);
                    assert(actual.expand[unit] == expand);
                    if (gamma) { gamma_units |= 1u << unit; ++nonzero_gamma; }
                    if (expand) { expand_units |= 1u << unit; ++nonzero_expand; }
                }
                assert(actual.gamma_units == gamma_units);
                assert(actual.expand_units == expand_units);
            }
        }
    }
    assert(nonzero_gamma && nonzero_expand);
    printf("786432 pipeline-mask cases; nonzero gamma=%u expand=%u\n", nonzero_gamma, nonzero_expand);
}
'''.replace("BODY", body)
        with tempfile.TemporaryDirectory(prefix="rsx-pipeline-masks-") as directory:
            work = Path(directory)
            (work / "test.c").write_text(harness)
            subprocess.run([os.environ.get("CC", "cc"), "-std=gnu17", "-O2",
                            "-fsanitize=address,undefined", "-ffunction-sections", "-fdata-sections",
                            "-I", str(video), "-I", str(video.parent.parent / "include"),
                            str(work / "test.c"), str(video / "rsx_texture_layout.c"),
                            "-Wl,--gc-sections", "-lm", "-o", str(work / "test")], check=True)
            subprocess.run([str(work / "test")], check=True)


if __name__ == "__main__":
    unittest.main()
