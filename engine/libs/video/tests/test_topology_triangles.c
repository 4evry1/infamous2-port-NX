#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
typedef uint32_t u32;
enum { RSX_PRIMITIVE_TRIANGLES=5, RSX_PRIMITIVE_TRIANGLE_STRIP=6,
       RSX_PRIMITIVE_TRIANGLE_FAN=7, RSX_PRIMITIVE_QUADS=8,
       RSX_PRIMITIVE_QUAD_STRIP=9, RSX_PRIMITIVE_POLYGON=10 };
#include "rsx_restart_cuts.h"
#include "production.inc"
#include "topology_reference.inc"
int main(void) {
    unsigned cases=0;
    u32 map[128], a[1024], b[1024];
    for(u32 i=0;i<128;++i) map[i]=(i*7919u)^0xdeadbeefu;
    for(u32 n=0;n<=128;++n) for(u32 prim=5;prim<=10;++prim)
    for(u32 mapped=0;mapped<2;++mapped) for(u32 c=0;c<4;++c) {
        u32 cuts[]={0,n/2,n};
        memset(a,0x5a,sizeof a); memset(b,0x5a,sizeof b);
        reference_topology(prim,n,cuts,c,mapped?map:NULL,a);
        rsx_draw_engine_write_topology_indices(prim,n,cuts,c,mapped?map:NULL,b);
        assert(!memcmp(a,b,sizeof a));++cases;
    }
    const size_t page=(size_t)sysconf(_SC_PAGESIZE);
    u32* src=mmap(NULL,page*2,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    u32* dst=mmap(NULL,page*2,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    assert(src!=MAP_FAILED && dst!=MAP_FAILED);
    assert(!mprotect((char*)src+page,page,PROT_NONE));
    assert(!mprotect((char*)dst+page,page,PROT_NONE));
    for(u32 n=0;n<=page/4;++n) for(u32 mapped=0;mapped<2;++mapped) {
        const u32 count=n-n%3;
        u32* input=(u32*)((char*)src+page)-count;
        u32* output=(u32*)((char*)dst+page)-count;
        for(u32 i=0;i<count;++i) {input[i]=i^0x80000000u;output[i]=0xababababu;}
        rsx_draw_engine_write_topology_indices(5,n,NULL,0,mapped?input:NULL,output);
        for(u32 i=0;i<count;++i) assert(output[i]==(mapped?input[i]:i));
        ++cases;
    }
    munmap(src,page*2);munmap(dst,page*2);
    for(u32 n=0;n<=120;++n) for(u32 from=0;from<4;++from) for(u32 to=0;to<4;++to) {
        for(u32 i=0;i<1024;++i)a[i]=b[i]=i*17u+1u;
        reference_topology(5,n,NULL,0,a+from,a+to);
        rsx_draw_engine_write_topology_indices(5,n,NULL,0,b+from,b+to);
        assert(!memcmp(a,b,sizeof a));++cases;
    }
    printf("PASS: %u topology cases, guarded ends and forward overlap semantics\n",cases);
}
