#define _GNU_SOURCE
#include "../rsx_fp_decompiler.h"
#include <assert.h>
#include <fenv.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include "fp_collect_reference.inc"
static rsx_fp_constant_block actual,expected;
static u32 seed=0x7a428de1;
static unsigned cases;
static u32 random_word(void){seed^=seed<<13;seed^=seed>>17;seed^=seed<<5;return seed;}
static void encode(u8 *p,u32 w){p[0]=(u8)(w>>8);p[1]=(u8)w;p[2]=(u8)(w>>24);p[3]=(u8)(w>>16);}
static void check(const u8 *bytes,u32 size,int lean){
 memset(&actual,0xa5,sizeof actual);memset(&expected,0xa5,sizeof expected);
 if(lean)expected.count=0;else memset(&expected,0,sizeof expected);
 feclearexcept(FE_ALL_EXCEPT);g_rsx_fp_nonfinite_constants=UINT64_MAX-3u;
 int reference=reference_collect(bytes,size,&expected);int flags=fetestexcept(FE_ALL_EXCEPT);unsigned long long counter=g_rsx_fp_nonfinite_constants;
 feclearexcept(FE_ALL_EXCEPT);g_rsx_fp_nonfinite_constants=UINT64_MAX-3u;
 int result=lean?rsx_fp_collect_constants_lean(bytes,size,&actual):rsx_fp_collect_constants(bytes,size,&actual);
 assert(result==reference);assert(flags==fetestexcept(FE_ALL_EXCEPT));assert(counter==g_rsx_fp_nonfinite_constants);assert(!memcmp(&actual,&expected,sizeof actual));++cases;
}
int main(void){
 const int rounding[]={FE_TONEAREST,FE_UPWARD,FE_DOWNWARD,FE_TOWARDZERO};
 u8 program[528];
 for(unsigned mode=0;mode<4;++mode){assert(!fesetround(rounding[mode]));
  for(unsigned n=0;n<4096;++n){
   for(unsigned k=0;k<sizeof program;k+=4)encode(program+k,random_word());
   unsigned offset=n&15u;u32 size=n%513u;
   check(program+offset,size,0);check(program+offset,size,1);
  }
 }
 assert(!fesetround(FE_TONEAREST));
 size_t size=(RSX_FP_MAX_INLINE_CONSTANTS+1u)*32u;u8 *large=calloc(1,size);assert(large);
 for(u32 n=0;n<=RSX_FP_MAX_INLINE_CONSTANTS;++n){encode(large+n*32+4,2);for(unsigned c=0;c<4;++c)encode(large+n*32+16+c*4,random_word());}
 for(u32 count=RSX_FP_MAX_INLINE_CONSTANTS-1;count<=RSX_FP_MAX_INLINE_CONSTANTS+1;++count){
  encode(large+(count-1)*32,1);check(large,count*32,0);check(large,count*32,1);encode(large+(count-1)*32,0);
 }
 free(large);
 size_t page=(size_t)sysconf(_SC_PAGESIZE);u8 *mem=mmap(NULL,page*3,PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);assert(mem!=MAP_FAILED);assert(!mprotect(mem+page,page,PROT_READ|PROT_WRITE));
 memset(mem+page*2-64,0,64);encode(mem+page*2-60,2);encode(mem+page*2-32,1);encode(mem+page*2-28,2);
 for(unsigned c=0;c<4;++c){encode(mem+page*2-48+c*4,random_word());encode(mem+page*2-16+c*4,random_word());}
 assert(!mprotect(mem+page,page,PROT_READ));for(u32 n=0;n<=64;++n)check(mem+page*2-64,n,1);assert(!munmap(mem,page*3));
 for(unsigned offset=0;offset<=128;offset+=4)for(int lean=0;lean<2;++lean){
  union {u32 align;u8 bytes[sizeof(rsx_fp_constant_block)+256];} a,e;
  for(unsigned i=0;i<sizeof a.bytes;++i)a.bytes[i]=e.bytes[i]=(u8)random_word();
  rsx_fp_constant_block *ao=(rsx_fp_constant_block*)(void*)(a.bytes+64),*eo=(rsx_fp_constant_block*)(void*)(e.bytes+64);
  if(lean)eo->count=0;else memset(eo,0,sizeof *eo);
  g_rsx_fp_nonfinite_constants=0;int er=reference_collect(e.bytes+offset,128,eo);unsigned long long ec=g_rsx_fp_nonfinite_constants;
  g_rsx_fp_nonfinite_constants=0;int ar=lean?rsx_fp_collect_constants_lean(a.bytes+offset,128,ao):rsx_fp_collect_constants(a.bytes+offset,128,ao);
  assert(ar==er);assert(ec==g_rsx_fp_nonfinite_constants);assert(!memcmp(a.bytes,e.bytes,sizeof a.bytes));++cases;
 }
 assert(rsx_fp_collect_constants_lean(NULL,16,&actual)==-1);assert(rsx_fp_collect_constants_lean(program,16,NULL)==-1);assert(rsx_fp_collect_constants(NULL,16,&actual)==-1);assert(rsx_fp_collect_constants(program,16,NULL)==-1);
 printf("PASS: %u full/lean fragment-collector cases; tails/counts/errors, bounds, FP flags and counter\n",cases);
}
