import os
from pathlib import Path
import re
import subprocess
import tempfile
import unittest


class TargetAlias(unittest.TestCase):
    def test_aliases(self):
        video = Path(__file__).resolve().parent.parent
        source = (video / "rsx_vulkan_backend.c").read_text()
        function = re.search(r"static int vk_eng_samples_target\(.*?\n\}", source, re.S)
        self.assertIsNotNone(function)
        capacity = re.search(r"^#define VK_ENG_MAX_OBJ[^\n]+", source, re.M)
        self.assertIsNotNone(capacity)
        harness = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include "rsx_commands.h"
CAPACITY
enum { VK_ENG_VIEW = 7 };
typedef struct { u32 kind, surface; } vk_eng_obj;
PRODUCTION
static int original(u32 mask, const u32 *textures, const vk_eng_obj *objects,
                    u32 depth, const u32 *targets, u32 count) {
    int self = 0;
    for (u32 u = 0; u < RSX_MAX_TEXTURES && !self; ++u) {
        if (!((mask >> u) & 1u)) continue;
        u32 t = textures[u];
        if (!t || t >= VK_ENG_MAX_OBJ) continue;
        if (objects[t].kind == VK_ENG_VIEW) t = objects[t].surface;
        if (t == depth) self = 1;
        for (u32 i = 0; i < count; ++i) if (t == targets[i]) self = 1;
    }
    return self;
}
static u32 seed = 0x12345678;
static u32 next(void) { seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5; return seed; }
static vk_eng_obj objects[VK_ENG_MAX_OBJ];
int main(void) {
    u32 textures[RSX_MAX_TEXTURES], targets[4] = {7, 11, 17, 0};
    unsigned hits = 0;
    for (u32 unit = 0; unit < RSX_MAX_TEXTURES; ++unit) {
        textures[unit] = unit + 1;
        objects[unit + 1].kind = unit % 3 ? 0 : VK_ENG_VIEW;
        objects[unit + 1].surface = targets[unit % 4];
    }
    textures[0] = 0; textures[1] = VK_ENG_MAX_OBJ; textures[2] = UINT32_MAX;
    for (u32 mask = 0; mask < 65536; ++mask) {
        for (u32 count = 0; count <= 4; ++count) {
            u32 full_mask = mask | (next() & 0xffff0000u);
            int result = vk_eng_samples_target(full_mask, textures, objects, 23, targets, count);
            assert(result == original(full_mask, textures, objects, 23, targets, count));
            hits += result;
        }
    }
    for (u32 n = 0; n < 100000; ++n) {
        u32 handle = next() % VK_ENG_MAX_OBJ;
        objects[handle].kind = next() & 1 ? VK_ENG_VIEW : 0;
        objects[handle].surface = next() % 32;
        textures[next() % RSX_MAX_TEXTURES] = n % 5 ? handle : next();
        targets[next() % 4] = next() % 32;
        u32 mask = next(), depth = next() % 32, count = next() % 5;
        int result = vk_eng_samples_target(mask, textures, objects, depth, targets, count);
        assert(result == original(mask, textures, objects, depth, targets, count));
        hits += result;
    }
    assert(hits && hits < 427680);
    puts("427680 target-alias cases passed");
}
'''.replace("CAPACITY", capacity.group()).replace("PRODUCTION", function.group())
        with tempfile.TemporaryDirectory(prefix="vk-target-alias-") as directory:
            work = Path(directory)
            (work / "test.c").write_text(harness)
            subprocess.run([os.environ.get("CC", "cc"), "-std=gnu17", "-O2", "-Wall", "-Wextra", "-Werror",
                            "-fsanitize=address,undefined", "-I", str(video),
                            "-I", str(video.parent.parent / "include"),
                            str(work / "test.c"), "-o", str(work / "test")], check=True)
            subprocess.run([str(work / "test")], check=True)


if __name__ == "__main__":
    unittest.main()
