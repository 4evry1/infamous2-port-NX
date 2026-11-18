#define _GNU_SOURCE
#include "../rsx_fp_decompiler.h"
#include <assert.h>
#include <fenv.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
static uint32_t seed=0x83d75a29u;
static uint32_t random_word(void){seed^=seed<<13;seed^=seed>>17;seed^=seed<<5;return seed;}
static void encode(u8 *p,u32 word){p[0]=(u8)(word>>8);p[1]=(u8)word;p[2]=(u8)(word>>24);p[3]=(u8)(word>>16);}
static void reference(const u8 *src,u32 *out){for(unsigned c=0;c<4;++c)out[c]=rsx_fp_sanitize_constant_word(rsx_fp_read_word(src+c*4));}
static unsigned cases;
static void check(const u8 *src){
    u32 actual[4],expected[4];
    feclearexcept(FE_ALL_EXCEPT);g_rsx_fp_nonfinite_constants=UINT64_MAX-2u;reference(src,expected);unsigned long long counter=g_rsx_fp_nonfinite_constants;int flags=fetestexcept(FE_ALL_EXCEPT);
    feclearexcept(FE_ALL_EXCEPT);g_rsx_fp_nonfinite_constants=UINT64_MAX-2u;rsx_fp_read_constant_vec4(src,actual);assert(flags==fetestexcept(FE_ALL_EXCEPT));
    assert(!memcmp(actual,expected,sizeof actual));assert(counter==g_rsx_fp_nonfinite_constants);++cases;
}
int main(void){
    const u32 edges[]={0,0x80000000,1,0x807fffff,0x00800000,0x3f800000,0xbf800000,0x7f7fffff,0xff7fffff,0x7f800000,0xff800000,0x7fc01234,0xff800001};
    const int rounding[]={FE_TONEAREST,FE_UPWARD,FE_DOWNWARD,FE_TOWARDZERO};
    u8 storage[32];
    for(unsigned rm=0;rm<4;++rm){
        assert(!fesetround(rounding[rm]));
        for(unsigned a=0;a<sizeof edges/sizeof edges[0];++a)
        for(unsigned b=0;b<sizeof edges/sizeof edges[0];++b){
            encode(storage,edges[a]);encode(storage+4,edges[b]);encode(storage+8,~edges[a]);encode(storage+12,~edges[b]);check(storage);
        }
        float limit=3.0e38f;u32 bound;memcpy(&bound,&limit,4);
        for(int delta=-4;delta<=4;++delta){u32 value=bound+(u32)delta;encode(storage,value);encode(storage+4,value|0x80000000u);encode(storage+8,value);encode(storage+12,value|0x80000000u);check(storage);}
        for(unsigned i=0;i<262144;++i){unsigned offset=i&15u;for(unsigned c=0;c<4;++c)encode(storage+offset+c*4,random_word());check(storage+offset);}
    }
    assert(!fesetround(FE_TONEAREST));
    const size_t page=(size_t)sysconf(_SC_PAGESIZE);u8 *mem=mmap(NULL,page*3,PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    assert(mem!=MAP_FAILED);assert(!mprotect(mem+page,page,PROT_READ|PROT_WRITE));
    for(unsigned c=0;c<4;++c)encode(mem+page*2-16+c*4,random_word());
    assert(!mprotect(mem+page,page,PROT_READ));check(mem+page*2-16);assert(!munmap(mem,page*3));
    for(unsigned source_offset=0;source_offset<=31;++source_offset)
    for(unsigned destination_offset=0;destination_offset<=32;destination_offset+=4){
        union{u32 align;u8 bytes[64];}actual,expected;
        for(unsigned i=0;i<64;++i)actual.bytes[i]=expected.bytes[i]=(u8)random_word();
        g_rsx_fp_nonfinite_constants=0;reference(expected.bytes+source_offset,(u32*)(void*)(expected.bytes+destination_offset));unsigned long long counter=g_rsx_fp_nonfinite_constants;
        g_rsx_fp_nonfinite_constants=0;rsx_fp_read_constant_vec4(actual.bytes+source_offset,(u32*)(void*)(actual.bytes+destination_offset));
        assert(!memcmp(actual.bytes,expected.bytes,64));assert(counter==g_rsx_fp_nonfinite_constants);++cases;
    }
    printf("PASS: %u fragment constant vec4 cases; bits/counters/rounding, protected source end, overlap\n",cases);
}
