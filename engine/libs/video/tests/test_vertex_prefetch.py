"""Check the cache predictor at the tower executable's two vertex-row alignments."""
from pathlib import Path
import subprocess
import tempfile
import unittest


class CachePrediction(unittest.TestCase):
    def test_hot_lines(self):
        video = Path(__file__).resolve().parents[1]
        source = (video / "rsx_draw_engine.c").read_text()
        def entry_type(name):
            end = source.index(name + ";", source.index("/* ---- vertex cache ---")) + len(name) + 1
            return source[source.rindex("typedef struct {", 0, end):end]

        start = source.rindex("#ifdef __SWITCH__", 0, source.index("#define ENG_CACHE_PREDICT("))
        ways = next(line for line in source.splitlines() if line.startswith("#define ENG_PAIR_WAYS "))
        end = source.index("while (0)", start) + len("while (0)")
        with tempfile.TemporaryDirectory() as tmp:
            work = Path(tmp)
            p = work / "test.c"
            p.write_text("""
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#define __SWITCH__ 1
typedef uint8_t u8;
typedef uint64_t u64;
typedef uint16_t u16;
typedef uint32_t u32;
static int enabled = 1;
static int eng_prefetch_on(void) { return enabled; }
static uintptr_t hints[2];
static unsigned n;
#define __builtin_prefetch(p,r,l) ((void)(assert(n < 2), hints[n++] = (uintptr_t)(p)))
""" + ways + "\n" + entry_type("eng_vc_entry") + "\n" + entry_type("eng_ic_entry") + "\n" + source[start:end] + """
static struct { _Alignas(64) u64 pad; eng_vc_entry rows[10]; } table;
static eng_ic_entry ix[4];
int main(void) {
    eng_vc_entry *v = table.rows, *prev = NULL;
    unsigned missed_by_tail = 0;
    for (unsigned i = 1; i <= 8; ++i) {
        v[0].next = i+1; n=0;
        ENG_CACHE_PREDICT(v, prev, &v[0], ENG_VC_PREFETCH_LAST);
        assert(n==2 && hints[0]==(uintptr_t)&v[i]);
        const uintptr_t hot = (uintptr_t)&v[i].wbox_ok / 64;
        assert(hints[0]/64==hot || hints[1]/64==hot);
        if (hints[0]/64!=hot && ((uintptr_t)(&v[i]+1)-1)/64!=hot) ++missed_by_tail;
    }
    assert(missed_by_tail==4);
    eng_ic_entry *iprev=NULL;
    ix[0].next=3; n=0;
    ENG_CACHE_PREDICT(ix, iprev, &ix[0], ENG_IC_PREFETCH_LAST);
    assert(n==1 && hints[0]==(uintptr_t)&ix[2]);
    v[0].next=0; n=0;
    ENG_CACHE_PREDICT(v, prev, &v[0], ENG_VC_PREFETCH_LAST);
    assert(n==0);
    v[0].next=3; enabled=0;
    ENG_CACHE_PREDICT(v, prev, &v[0], ENG_VC_PREFETCH_LAST);
    assert(n==0);
}
""")
            subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", str(p), "-o", str(work/"test")], check=True)
            subprocess.run([str(work/"test")], check=True)


if __name__ == "__main__":
    unittest.main()
