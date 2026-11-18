#include "rsx_constant_plan.h"
#include "vk_staging_copy.h"
#include <assert.h>
#include <stdio.h>
#include <time.h>
#include <stdlib.h>

typedef u64 VkDeviceSize;
typedef uintptr_t VkBuffer;
enum { ROW_BYTES = 16, VS_BYTES = 8224, PS_OFFSET = 8448, PS_BYTES = 272,
       ARENA_BYTES = 262144 };
#define VK_LOG(...) fprintf(stderr, __VA_ARGS__)
static int batch_on = 1;
static struct {
    struct { void* ptr; VkDeviceSize size; VkBuffer buf; } uarena;
    VkDeviceSize ucur; u8* shadow;
} s_batch;
static struct { int on; VkDeviceSize ps_off; struct { void* ptr; size_t size; } ubo; } s_g;
static struct {
    int vs_ranged, vs_gen_ok;
    const u8* vs_rows;
    u32 vs_gen, vs_first, vs_count, vs_bytes, vs_tail_off, vs_tail_bytes, warned;
    u8 vs_tail[64]; u32 ps_ver, ps_bytes;
#ifdef __SWITCH__
    rsx_constant_plan vs_sparse;
#endif
} s_e2;
static u32 s_dset_gen = 1;
static int vk_batch_on(void) { return batch_on; }
static u8* vk_batch_shadow(void) { return s_batch.shadow; }
#include "vk_sparse_fixture.inc"

static u8 arena[ARENA_BYTES], shadow[PS_OFFSET + PS_BYTES], shared[PS_OFFSET];
static u8 rows[8192], tail[32];
static u32 rng = 0x98765432;
static u32 random_word(void) { rng = rng * 1664525u + 1013904223u; return rng; }

static void bind_plan(const rsx_constant_plan* p, u32 generation)
{
    vk_eng_vs_constants_gen(NULL, generation);
#ifdef __SWITCH__
    if (p) {
        vk_eng_bind_vs_constants_sparse(NULL, rows, 256, 212, tail, sizeof tail, VS_BYTES, p);
        return;
    }
#else
    (void)p;
#endif
    vk_eng_bind_vs_constants_range(NULL, rows, 256, 212, tail, sizeof tail, VS_BYTES);
}

static VkDeviceSize draw(u32 slot, const rsx_constant_plan* p)
{
    const VkDeviceSize lead = s_e2.vs_first * 16u / 256u * 256u;
    const VkDeviceSize offset = slot * 16384u + lead, before = slot * 16384u;
    VkDeviceSize base, ps;
    s_batch.ucur = before + PS_OFFSET + PS_BYTES;
    assert(!vk_eng_cwin(offset, lead, PS_OFFSET + PS_BYTES, s_dset_gen, before, &base, &ps));
#ifdef __SWITCH__
    if (p && rsx_constant_plan_valid(p, s_e2.vs_first, s_e2.vs_count)) {
        for (u32 i = 0; i < p->count; i++)
            assert(!memcmp(arena + base + p->spans[i].first * 16u,
                           rows + p->spans[i].first * 16u, p->spans[i].count * 16u));
    } else
#else
    (void)p;
#endif
    assert(!memcmp(arena + base + s_e2.vs_first * 16u,
                   rows + s_e2.vs_first * 16u, s_e2.vs_count * 16u));
    assert(!memcmp(arena + base + 8192u, tail, sizeof tail));
    assert(!memcmp(arena + ps, shadow + PS_OFFSET, PS_BYTES));
    return base;
}

int main(int argc, char** argv)
{
    assert(argc == 3);
    assert(!setenv("PS3RECOMP_VK_CONST_BYTE_REUSE", argv[2], 1));
    assert(!setenv("PS3RECOMP_VK_CONST_REUSE", argv[1], 1));
    assert(!setenv("PS3RECOMP_VK_SPARSE_VERIFY", "1", 1));
    assert(!setenv("PS3RECOMP_VK_CONSTANT_TIMING", "1", 1));
    memset(arena, 0xA5, sizeof arena); memset(shadow, 0x6B, sizeof shadow);
    for (size_t i = 0; i < sizeof rows; i++) rows[i] = (u8)random_word();
    const u32 special_bits[] = {0x7FC12345u, 0x80000000u, 0x7F800000u, 0xFF800000u};
    memcpy(rows + 256u * 16u, special_bits, sizeof special_bits);
    memset(tail, 0x42, sizeof tail);
    s_batch.uarena.ptr = arena; s_batch.uarena.size = sizeof arena; s_batch.uarena.buf = 1;
    s_batch.shadow = shadow; s_g.on = 1; s_g.ps_off = PS_OFFSET; s_g.ubo.ptr = shared; s_g.ubo.size = sizeof shadow; s_e2.ps_ver = 1;
    u8 fragment[PS_BYTES];
    memcpy(fragment, shadow + PS_OFFSET, sizeof fragment);
    vk_eng_bind_ps_constants(NULL, fragment, sizeof fragment);
    rsx_constant_plan a = {2, 5, {{256, 4}, {467, 1}}};
    rsx_constant_plan b = {3, 6, {{256, 4}, {300, 1}, {467, 1}}};
    bind_plan(&a, 1); VkDeviceSize base = draw(0, &a);
#ifdef __SWITCH__
    assert(arena[base + 300u * 16u] == 0xA5); /* holes really remain poisoned */
#endif
    bind_plan(&a, 1);
    assert((draw(1, &a) == base) == (argv[1][0] != '0'));
    bind_plan(&b, 1); VkDeviceSize changed = draw(1, &b);
    (void)base; (void)changed;
#ifdef __SWITCH__
    assert(changed != base); /* same interval/generation, different initialized rows */
#endif
    bind_plan(NULL, 1); draw(2, NULL); /* sparse -> dense cannot reuse holes */
    bind_plan(&a, 1); draw(3, &a);
    /* Invalid plans safely retain the dense contract. */
    b.spans[1].first = 258; bind_plan(&b, 1); draw(4, NULL);
    b.spans[1].first = 300;
#ifdef __SWITCH__
    const rsx_constant_plan original = a;
    bind_plan(&a, 2); a.spans[1].first = 466;
    draw(5, &original); /* caller mutation after bind must not alter pending plan */
    a = original;
    vk_eng_vs_constants_gen(NULL, 2);
    vk_eng_bind_vs_constants_sparse(NULL, rows, 200, 268, tail, sizeof tail, VS_BYTES, &a);
    draw(6, &a); /* changed interval with the same sparse rows and generation */
#endif
#ifdef __SWITCH__
    if (argv[1][0] != '0' && argv[2][0] != '0') {
        ++s_dset_gen;
        bind_plan(&a, 100);
        const VkDeviceSize old_vs = draw(0, &a), old_ps = s_cm.ps_abs;
        fragment[PS_BYTES - 1] ^= 1;
        vk_eng_bind_ps_constants(NULL, fragment, sizeof fragment);
        fragment[PS_BYTES - 1] ^= 1;
        vk_eng_bind_ps_constants(NULL, fragment, sizeof fragment);
        rows[310 * 16u] ^= 1; /* outside both sparse spans */
        memset(arena + 16384u, 0xB7, 16384u);
        bind_plan(&a, 101);
        assert(draw(1, &a) == old_vs && s_cm.ps_abs == old_ps);
        assert(s_batch.ucur == 16384u);
        for (size_t i = 16384u; i < 32768u; ++i) assert(arena[i] == 0xB7);
        vk_eng_bind_ps_constants(NULL, NULL, 0);
        vk_eng_bind_ps_constants(NULL, fragment, sizeof fragment);
        assert(draw(1, &a) == old_vs && s_cm.ps_abs == old_ps);
        rows[467 * 16u + 15u] ^= 1;
        bind_plan(&a, 102);
        assert(draw(2, &a) != old_vs && s_cm.ps_abs == old_ps);
        shadow[PS_OFFSET + PS_BYTES - 1] ^= 1;
        ++s_e2.ps_ver;
        VkDeviceSize last_vs = s_cm.vs_base;
        bind_plan(&a, 102);
        assert(draw(3, &a) == last_vs && s_cm.ps_abs != old_ps);
        bind_plan(NULL, 103);
        last_vs = draw(4, NULL);
        rows[467 * 16u] ^= 1;
        vk_eng_vs_constants_gen(NULL, 104);
        vk_eng_bind_vs_constants_range(NULL, rows, 256, 4, tail, sizeof tail, VS_BYTES);
        assert(draw(5, NULL) == last_vs);
        bind_plan(NULL, 104);
        assert(draw(6, NULL) != last_vs); /* widened range includes the changed row */
        tail[31] ^= 1;
        last_vs = s_cm.vs_base;
        bind_plan(NULL, 104);
        assert(draw(7, NULL) != last_vs);
        last_vs = s_cm.vs_base;
        vk_eng_bind_vs_constants_range(NULL, rows, 256, 212, tail, sizeof tail, VS_BYTES);
        assert(draw(8, NULL) == last_vs); /* unversioned rows require byte equality every time */
        rows[260 * 16u + 3] ^= 1;
        assert(draw(9, NULL) != last_vs);
        last_vs = s_cm.vs_base;
        const VkDeviceSize last_ps = s_cm.ps_abs;
        ++s_batch.uarena.buf;
        assert(draw(10, NULL) != last_vs && s_cm.ps_abs != last_ps);
        last_vs = s_cm.vs_base;
        ++s_dset_gen; /* even the same buffer handle can now name recycled space */
        assert(draw(11, NULL) != last_vs);
        ++s_dset_gen;
    } else if (argv[1][0] != '0') {
        ++s_dset_gen;
        bind_plan(&a, 500);
        const VkDeviceSize previous = draw(0, &a);
        bind_plan(&a, 501);
        assert(draw(1, &a) != previous); /* byte fallback independently disabled */
        ++s_dset_gen;
    }
#endif
    for (u32 n = 0; n < 10000; n++) {
        const u32 row = random_word() % 512u;
        rows[row * 16u + random_word() % 16u] ^= 0x39;
        if (!(n % 7u)) tail[n % sizeof tail] ^= 0x73;
        if (!(n % 11u)) { shadow[PS_OFFSET + n % PS_BYTES] ^= 0x51; s_e2.ps_ver++; }
        if (!(n % 15u)) { s_dset_gen++; memset(arena, 0xA5, sizeof arena); }
        if (!(n % 29u)) s_batch.uarena.buf++;
        const rsx_constant_plan* p = n % 3u ? (n % 2u ? &a : &b) : NULL;
        bind_plan(p, n + 2); draw(n % 15u, p);
    }
#ifdef __SWITCH__
    for (unsigned i = 0; i < 2048; ++i) { bind_plan(&a, 12001); draw(0, &a); }
    for (int i = 0; i < VK_CONST_BIN_COUNT; ++i) {
        if (i == VK_CONST_HIT && argv[1][0] == '0') assert(!s_const_time.bins[i].samples);
        else assert(s_const_time.bins[i].samples && s_const_time.bins[i].ns);
    }
#endif
    /* Unbatched/full/malformed binds clear sparse state and retain fallback. */
    batch_on = 0; bind_plan(&a, 20000);
    assert(!memcmp(shared + 256 * 16u, rows + 256 * 16u, 212 * 16u));
    batch_on = 1;
    vk_eng_bind_vs_constants(NULL, rows, sizeof rows); assert(!s_e2.vs_ranged);
    vk_eng_bind_vs_constants_range(NULL, rows, UINT32_MAX, 1, tail, sizeof tail, VS_BYTES);
    assert(!s_e2.vs_bytes && !s_e2.vs_ranged);
    bind_plan(&a, 30000);
    VkDeviceSize x, y; s_batch.uarena.size = PS_OFFSET;
    const VkDeviceSize cursor = s_batch.ucur;
    assert(vk_eng_cwin(0, 4096, PS_OFFSET + PS_BYTES, s_dset_gen, cursor, &x, &y) == -1);
    assert(s_batch.ucur == cursor);
#ifdef __SWITCH__
    assert(s_sparse.checks >= 10000 && !s_sparse.diffs);
#endif
    puts("Vulkan sparse bind/window: 10000 randomized draws passed");
    return 0;
}
