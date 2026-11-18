"""Exercise the production memo replacement block with a fault-injected allocator."""
import os
from pathlib import Path
import subprocess
import tempfile

video = Path(__file__).resolve().parent.parent
source = (video / "rsx_draw_engine.c").read_text()
start = source.index("    u8* copy =", source.index("static const eng_fp_memo* eng_fp_current"))
end = source.index("    /* (rsx5) PS3RECOMP_ENG_PROG_CMEMO", start)
block = source[start:end]
harness = r'''
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
typedef uint8_t u8;
typedef uint32_t u32;
typedef uint64_t u64;
typedef struct { u8 *bytes; u32 size, loc, off, used; } memo;
static u32 s_memo_clock, s_fp_sz[1], s_fp_use[1];
static u64 s_fp_key[1];
static unsigned calls, fail_alloc;
static void *checked_realloc(void *p, size_t size) {
    ++calls;
    if (fail_alloc) return NULL;
    return realloc(p, size);
}
#define realloc checked_realloc
static memo *replace(memo *m, const u8 *uc, u32 size, u32 loc, u32 off) {
    const u32 v = 0;
    const u64 key = ((u64)loc << 32) | off;
PRODUCTION
    return m;
}
#undef realloc
int main(void) {
    memo m = {0};
    u8 input[4096];
    unsigned cases = 0, avoided = 0;
    /* Fresh allocation, equal size, grow, shrink, and repeat with failures. */
    const u32 sizes[] = {16,16,32,32,16,4096,4096,64,64,16};
    for (unsigned round = 0; round < 1024; ++round) {
        for (unsigned j = 0; j < sizeof sizes / sizeof sizes[0]; ++j) {
            for (unsigned fail = 0; fail < 2; ++fail) {
                const u32 size = sizes[j];
                for (u32 k = 0; k < size; ++k) input[k] = (u8)(k + round + j + fail);
                memo old = m;
                u8 saved[4096];
                if (m.bytes) memcpy(saved, m.bytes, m.size);
                u32 clock = s_memo_clock, sz = s_fp_sz[0], use = s_fp_use[0];
                u64 key = s_fp_key[0];
                const int reuse = m.bytes && m.size == size;
                calls = 0; fail_alloc = fail;
                memo *result = replace(&m, input, size, round & 1, j * 16);
                assert(calls == (unsigned)!reuse);
                if (fail && !reuse) {
                    assert(!result);
                    assert(m.bytes == old.bytes && m.size == old.size);
                    assert(m.loc == old.loc && m.off == old.off && m.used == old.used);
                    if (m.bytes) assert(!memcmp(m.bytes, saved, m.size));
                    assert(s_memo_clock == clock && s_fp_sz[0] == sz);
                    assert(s_fp_use[0] == use && s_fp_key[0] == key);
                } else {
                    assert(result == &m && m.size == size);
                    assert(!memcmp(m.bytes, input, size));
                    assert(m.loc == (round & 1) && m.off == j * 16);
                    assert(m.used == clock + 1 && s_memo_clock == clock + 1);
                    assert(s_fp_sz[0] == size && s_fp_use[0] == m.used);
                    assert(s_fp_key[0] == (((u64)m.loc << 32) | m.off));
                    if (reuse) { assert(m.bytes == old.bytes); ++avoided; }
                }
                ++cases;
            }
        }
    }
    free(m.bytes);
    /* Nonzero semantic size with a null pointer must still allocate. */
    m = (memo){.size = 16}; calls = 0; fail_alloc = 1;
    assert(!replace(&m, input, 16, 1, 16) && calls == 1 && !m.bytes);
    fail_alloc = 0; calls = 0;
    assert(replace(&m, input, 16, 1, 16) && calls == 1);
    free(m.bytes);
    printf("%u memo replacement cases passed; %u allocator calls avoided\n", cases + 2, avoided);
}
'''.replace("PRODUCTION", block)
with tempfile.TemporaryDirectory(prefix="fp-memo-reuse-") as tmp:
    work = Path(tmp)
    (work / "test.c").write_text(harness)
    subprocess.run([os.environ.get("CC", "cc"), "-std=gnu17", "-O2", "-Wall", "-Wextra", "-Werror",
                    "-fsanitize=address,undefined", str(work / "test.c"), "-o", str(work / "test")], check=True)
    subprocess.run([str(work / "test")], check=True)
