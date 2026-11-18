#define _GNU_SOURCE
#include "../rsx_dispatch.c"
#include <assert.h>
#include <stdio.h>
static rsx_dispatch actual;
static void mark(u64 *bits,u32 block){bits[block/64u]|=UINT64_C(1)<<(block%64u);bits[RSX_DSP_DIRTY_WORDS]|=UINT64_C(1)<<(block/64u);}
static unsigned cases;
static void check(int instruction,u32 base,u32 start,u32 count){
    memset(&actual,0,sizeof actual);
    u64 dirty[RSX_DSP_DIRTY_WORDS+1]={0},expected[RSX_DSP_DIRTY_WORDS+1]={0};
    u32 words[32];for(u32 k=0;k<32;++k)words[k]=__builtin_bswap32(k+1);
    actual.dirty=dirty;actual.no_seen=1;
    if(instruction){
        actual.regs[M_VP_UPLOAD_FROM_ID>>2]=base;
        u32 cursor=base;
        for(u32 k=0;k<count;++k){u32 lane=(start+k)&3u,pos=cursor*4u+lane;
            if(pos<RSX_DSP_VP_WORDS)mark(expected,RSX_DSP_BLK_VP0+pos/16u);
            if(lane==3u)++cursor;
        }
        if(count)mark(expected,M_VP_UPLOAD_FROM_ID>>6);
        burst_inst_run(&actual,M_VP_UPLOAD_INST+start*4u,words,count);
    }else{
        actual.regs[M_VP_UPLOAD_CONST_ID>>2]=base;
        for(u32 k=0;k<count;++k){u32 slot=base+(start+k)/4u;
            if(slot<RSX_DSP_NUM_CONSTANTS)mark(expected,RSX_DSP_BLK_CONST0+slot/4u);
        }
        burst_const_run(&actual,M_VP_UPLOAD_CONST+start*4u,words,count);
    }
    if(memcmp(dirty,expected,sizeof dirty)){
        fprintf(stderr,"dirty mismatch instruction=%d base=%08x start=%u count=%u\n",instruction,base,start,count);abort();
    }
    ++cases;
}
int main(void){
    check(0,UINT32_MAX,0,8);check(1,UINT32_MAX,0,8);
    const u32 bases[]={0,1,510,511,512,543,544,545,0x3fffffffu,0x40000000u,0x7fffffffu,0x80000000u,UINT32_MAX-7,UINT32_MAX-3,UINT32_MAX-1,UINT32_MAX};
    for(int instruction=0;instruction<2;++instruction)
    for(unsigned b=0;b<sizeof bases/sizeof bases[0];++b)
    for(u32 start=0;start<32;++start)
    for(u32 count=0;count<=32-start;++count)check(instruction,bases[b],start,count);
    printf("PASS: %u grouped constant/program dirty-coverage cases across cursor wrap and storage limits\n",cases);
}
