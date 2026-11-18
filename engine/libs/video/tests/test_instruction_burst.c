#define _GNU_SOURCE
#include <assert.h>
#include <stdio.h>
#include <sys/mman.h>
#include <unistd.h>
#include "../rsx_dispatch.c"
#include "plain_register_reference.inc"
static rsx_dispatch actual, expected;
static void mode_set(u32 mode) {
    s_ed_on = mode == 1;
    s_clog_lo = mode == 2 ? 999 : mode == 3 ? -2 : -1;
    s_clog_hi = 999;
    s_plain_ready = mode != 4;
    s_burst_lean = mode != 5;
}
static unsigned cases;
static void compare(u32 method, int incr, u32 count, u32 base, u32 mode, const u32 *words, int alias) {
    memset(&actual, 0, sizeof actual); memset(&expected, 0, sizeof expected);
    actual.no_seen = expected.no_seen = mode & 1;
    actual.vp_gen = expected.vp_gen = UINT32_MAX;
    actual.regs[M_VP_UPLOAD_FROM_ID >> 2] = expected.regs[M_VP_UPLOAD_FROM_ID >> 2] = base;
    memset(actual.seen, 0xff, sizeof actual.seen); memset(expected.seen, 0xff, sizeof expected.seen);
    u64 ad[RSX_DSP_DIRTY_WORDS + 1] = {0}, ed[RSX_DSP_DIRTY_WORDS + 1] = {0};
    if (mode & 2) { actual.dirty = ad; expected.dirty = ed; }
    const u32 *aw = words, *ew = words;
    if (alias) {
        u32 *as = alias == 1 ? actual.regs + (M_VP_UPLOAD_FROM_ID >> 2) : actual.vp;
        u32 *es = alias == 1 ? expected.regs + (M_VP_UPLOAD_FROM_ID >> 2) : expected.vp;
        for (u32 i=0;i<count;++i) as[i]=es[i]=words[i];
        aw=as; ew=es;
    }
    for (unsigned repeat=0;repeat<2;++repeat) {
        mode_set(mode); rsx_dispatch_burst(&actual,method,incr,count?aw:NULL,count);
        mode_set(mode); reference_rsx_dispatch_burst(&expected,method,incr,count?ew:NULL,count);
        assert(!memcmp(actual.vp,expected.vp,sizeof actual.vp));
        assert(!memcmp(actual.regs,expected.regs,sizeof actual.regs));
        assert(!memcmp(actual.seen,expected.seen,sizeof actual.seen));
        assert(!memcmp(ad,ed,sizeof ad));
        assert(actual.vp_gen==expected.vp_gen);
    }
    ++cases;
}
int main(void) {
    unsetenv("RSX_CONST_LOG"); unsetenv("RSX_EMPTY_DRAW_LOG");
    reference_plain_init(); plain_init();
    const size_t page=(size_t)sysconf(_SC_PAGESIZE);
    u8 *mem=mmap(NULL,page*3,PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    assert(mem!=MAP_FAILED); assert(mprotect(mem+page,page,PROT_READ|PROT_WRITE)==0);
    const u32 bases[]={0,1,RSX_DSP_VP_WORDS/4-1,RSX_DSP_VP_WORDS/4,UINT32_MAX,0x40000000u};
    for(u32 start=0;start<34;++start)
    for(u32 count=0;count<=34;++count) {
        u32 *words=(u32*)(mem+page*2-count*4u);
        for(u32 i=0;i<count;++i) words[i]=__builtin_bswap32(0x12000000u+i+cases);
        for(u32 bi=0;bi<sizeof bases/sizeof bases[0];++bi)
        for(u32 mode=0;mode<8;++mode)
        for(int incr=0;incr<2;++incr)
            compare((M_VP_UPLOAD_INST+start*4u)|0xfff00003u,incr,count,bases[bi],mode,words,0);
    }
    u32 words[34]; for(u32 i=0;i<34;++i) words[i]=__builtin_bswap32(i+1u);
    for(int alias=1;alias<=2;++alias)
    for(u32 start=0;start<4;++start)
    for(u32 count=1;count<=34;++count)
    for(u32 mode=0;mode<8;++mode)
        compare(M_VP_UPLOAD_INST+start*4u,1,count,0,mode,words,alias);
    assert(munmap(mem,page*3)==0);
    printf("PASS: %u instruction header routing cases; repeated, wrapping/bounded cursor, diagnostics/init, masks, alias and protected source ends\n",cases);
}
