#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <stdatomic.h>
#include <unistd.h>

typedef struct { uint64_t gpr[32], lr; } ppu_context;
typedef struct { int active; uint32_t addr, size; int32_t container_id; uint32_t page_size; } sys_mem_alloc_info;
typedef struct { int unused; } sys_mem_container_info, sys_mmapper_shared_info;
#define SYS_MEMORY_ALLOC_MAX 1024
#define SYS_MEMORY_CONTAINER_MAX 16
#define SYS_MMAPPER_SHARED_MAX 64
#define SYS_MEMORY_PAGE_SIZE_1M 0x400
#define LV2_ARG_U32(c,n) ((uint32_t)(c)->gpr[3+(n)])
#define LV2_ARG_PTR(c,n) LV2_ARG_U32(c,n)
#define VM_ALIGN_UP(x,a) (((x)+(a)-1)&~((a)-1))
#define CELL_OK 0
#define CELL_EINVAL -22
#define CELL_ENOMEM -12
typedef struct { long cap, value; } ps3_lim;
#define PS3_LIM_INIT(name,limit) { .cap = (limit) }
static void ps3_lim_set(ps3_lim *p,long v) { p->value=v; }
static void ps3_lim_add(ps3_lim *p,long v) { p->value+=v; }
static void ps3_lim_full(ps3_lim *p,const char *fmt,...) { (void)p; (void)fmt; }
enum { BASE = 0x40000000u, MB = 1048576, BYTES = 256 * MB };
static unsigned char *memory;
static atomic_int fail_commit, commits;
static atomic_int mixed_failures, mixed_successes;
static void *vm_to_host(uint32_t addr) { assert(addr >= BASE && addr < BASE + BYTES); return memory + addr - BASE; }
uint64_t vm_read64(uint64_t addr) { (void)addr; return 0; }
static int vm_commit(uint32_t addr,uint32_t size)
{
    assert(addr >= BASE && (uint64_t)addr + size <= BASE + BYTES);
    int attempt = atomic_fetch_add(&commits,1);
    usleep(100);
    return atomic_load(&fail_commit) || (atomic_load(&mixed_failures) && (attempt & 1))
        ? CELL_ENOMEM : CELL_OK;
}
#include "memory_fixture.inc"

static ppu_context request(uint32_t size)
{
    ppu_context ctx = {0};
    ctx.gpr[3] = size;
    ctx.gpr[4] = SYS_MEMORY_PAGE_SIZE_1M;
    return ctx;
}
static void *allocate_thread(void *unused)
{
    (void)unused;
    ppu_context ctx = request(MB);
    int64_t result = sys_memory_allocate(&ctx);
    if (atomic_load(&mixed_failures)) {
        assert(result == CELL_OK || result == CELL_ENOMEM);
        if (result == CELL_OK) atomic_fetch_add(&mixed_successes,1);
    } else assert(result == CELL_OK);
    return NULL;
}
int main(int argc,char **argv)
{
    assert(argc==2);
    assert(!setenv("PS3_MEM_REUSE",argv[1],1));
    memory=calloc(1,BYTES); assert(memory);
    ppu_context ctx=request(MB);
    atomic_store(&fail_commit,1);
    for (int i=0;i<300;i++) assert(sys_memory_allocate(&ctx)==CELL_ENOMEM);
    assert(s_total_allocated==0);
    assert(g_sys_mem_bump_ptr==BASE);
    assert(commits==300);
    for (int i=0;i<SYS_MEMORY_ALLOC_MAX;i++) assert(!g_sys_mem_allocs[i].active);

    atomic_store(&fail_commit,0);
    assert(sys_memory_allocate(&ctx)==CELL_OK);
    assert(g_sys_mem_allocs[0].addr==BASE);
    assert(s_total_allocated==MB);
    memset(memory,0xA5,MB);
    ppu_context release={0}; release.gpr[3]=BASE;
    assert(sys_memory_free(&release)==CELL_OK);
    assert(!s_total_allocated);
    int previous=commits;
    atomic_store(&fail_commit,1);
    if (argv[1][0]=='1') {
        assert(sys_memory_allocate(&ctx)==CELL_OK);
        assert(commits==previous);
        for (int i=0;i<MB;i++) assert(!memory[i]);
    } else {
        assert(sys_memory_allocate(&ctx)==CELL_ENOMEM);
        assert(!s_total_allocated);
        assert(g_sys_mem_bump_ptr==BASE+MB);
    }
    atomic_store(&fail_commit,0);
    pthread_t threads[8];
    for (int i=0;i<8;i++) assert(!pthread_create(&threads[i],NULL,allocate_thread,NULL));
    for (int i=0;i<8;i++) assert(!pthread_join(threads[i],NULL));
    unsigned active=0;
    for (int i=0;i<SYS_MEMORY_ALLOC_MAX;i++) if (g_sys_mem_allocs[i].active) {
        active++;
        for (int j=0;j<i;j++) if (g_sys_mem_allocs[j].active)
            assert(g_sys_mem_allocs[i].addr!=g_sys_mem_allocs[j].addr);
    }
    assert(active==(argv[1][0]=='1'?9u:8u));
    assert(s_total_allocated==active*MB);
    uint32_t cursor = g_sys_mem_bump_ptr;
    atomic_store(&mixed_failures,1);
    for (int i=0;i<8;i++) assert(!pthread_create(&threads[i],NULL,allocate_thread,NULL));
    for (int i=0;i<8;i++) assert(!pthread_join(threads[i],NULL));
    assert(mixed_successes==4);
    assert(s_total_allocated==(active+4)*MB);
    assert(g_sys_mem_bump_ptr==cursor+4*MB);
    free(memory);
    puts("allocation failure/retry/reuse/concurrency passed");
}
