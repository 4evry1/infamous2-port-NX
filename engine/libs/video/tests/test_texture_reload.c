#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef uint8_t u8;
typedef uint32_t u32;
typedef uint64_t u64;
typedef struct {
    u32 handle, cubemap, depth, location, offset, span, format, width, height;
    u64 content_hash, pack_content_hash, sparse_hash;
    u32 last_hash_frame, last_full_frame;
    u8 modded, wt_ok;
} eng_texture;
static struct { u32 frames, n_textures; eng_texture textures[5]; } g;
static struct { int on, init; u32 n, npacks, pool_used, idx_size; u32* idx; char dump_dir[64]; } s_mods;
static u8 bytes[128];
static int enable_pack, enable_dump;
static u64 requested_key;
static unsigned matches, stops;
static const u8* eng_guest_ptr(void* user, u32 location, u32 offset, u32 span) {
    (void)user;
    return location == 1 && offset == 7 && span <= sizeof bytes ? bytes : NULL;
}
static void eng_mods_preload_stop(void) { ++stops; }
static void eng_mods_init(void) {
    s_mods.init = 1; s_mods.on = enable_pack; s_mods.n = enable_pack;
    s_mods.npacks = enable_pack;
    if (enable_dump) strcpy(s_mods.dump_dir, "fixture");
}
static int eng_mod_find(u64 key) {
    if (key != requested_key) return 0;
    ++matches;
    return 1;
}
#include "production.inc"
static void reset(void) {
    memset(&g, 0, sizeof g); memset(&s_mods, 0, sizeof s_mods);
    enable_pack = enable_dump = 0; matches = stops = 0;
    g.frames = 100; g.n_textures = 5;
    for (u32 i = 0; i < g.n_textures; ++i) {
        eng_texture* e = &g.textures[i];
        e->handle = i + 1; e->depth = 1; e->location = 1; e->offset = 7;
        e->span = 128; e->format = 0x85; e->width = e->height = 4;
        e->pack_content_hash = 789 + i;
        e->content_hash = 123; e->sparse_hash = 456; e->wt_ok = 1;
        e->last_hash_frame = e->last_full_frame = 100;
    }
    g.textures[1].width = 8;  /* readable, different pack key */
    g.textures[2].offset = 8; /* unreadable */
    g.textures[3].cubemap = 1;
    g.textures[4].depth = 2;
}
static void marked(unsigned i) {
    assert(g.textures[i].content_hash != 123 && !g.textures[i].wt_ok);
    assert(g.textures[i].last_hash_frame == 99 && g.textures[i].last_full_frame == 92);
}
int main(void) {
    for (u32 i = 0; i < sizeof bytes; ++i) bytes[i] = (u8)(i * 37u + 3u);
    int readable = 0;
    const u64 legacy = eng_texture_pack_hash(1, 7, 128, &readable);
    assert(readable);
    requested_key = eng_texmod_key(legacy, 0x85, 4, 4);
    reset(); enable_pack = 1; eng_mods_reload();
    assert(stops == 1 && matches == 1);
    marked(0);
    for (unsigned i = 0; i < 5; ++i) {
        assert(g.textures[i].pack_content_hash == 789 + i);
        assert(g.textures[i].handle == i + 1);
    }
    for (unsigned i = 1; i < 5; ++i) assert(g.textures[i].content_hash == 123);
    reset(); s_mods.on = 1; g.textures[1].modded = 1; eng_mods_reload();
    marked(1); assert(g.textures[0].content_hash == 123 && matches == 0);
    reset(); enable_dump = 1; eng_mods_reload();
    for (unsigned i = 0; i < 3; ++i) marked(i);
    assert(matches == 0);
    for (unsigned i = 0; i < 5; ++i) assert(g.textures[i].pack_content_hash == 789 + i);
    assert(g.textures[3].content_hash == 123 && g.textures[4].content_hash == 123);
    puts("PASS: legacy pack reload match, unmatched/unreadable entries, removal, dump start, cube/volume exclusions");
}
