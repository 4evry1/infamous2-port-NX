#define _GNU_SOURCE
#include "../cellGcmSys.c"
#include <assert.h>
#include <sys/mman.h>
#include <stdatomic.h>
#include <pthread.h>
#include <sched.h>
u8 *vm_base;
u32 ppu_vm_size;
static void put_word(u32 address,u32 word) {
    u32 be=__builtin_bswap32(word); memcpy(vm_base+address,&be,4);
}
static void reset_state(void) {
    s_gcm_initialized=1; s_gcm_context_ea=1; s_gcm_syscall_mode=0;
    s_kw_more=0; s_flip_pending=0; s_kw_put=0x100;
    s_sema_live=0; s_sema_offset=0x870; s_sema_wait_val=7;
    s_park_off=0; s_fifo_getoff=0x100; s_kw_park_word=0x20000100;
    put_word(GCM_CONTROL_GUEST_ADDR,s_kw_put);
    put_word(GCM_LABEL_GUEST_BASE+0x870,6);
    s_ea_address_table[0]=1; put_word(0x100100,s_kw_park_word);
}
static atomic_int locked, release_lock;
static void *hold_fifo_lock(void *unused) {
    (void)unused; AcquireSRWLockExclusive(&s_gcm_fifo_lock);
    atomic_store(&locked,1);
    while(!atomic_load(&release_lock)) sched_yield();
    ReleaseSRWLockExclusive(&s_gcm_fifo_lock); return NULL;
}
int main(void) {
    const size_t size=(size_t)1<<32;
    vm_base=mmap(NULL,size,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_NORESERVE,-1,0);
    assert(vm_base!=MAP_FAILED); ppu_vm_size=UINT32_MAX;
    for(unsigned mask=0;mask<64;++mask) {
        reset_state(); s_kw_more=!!(mask&1); s_flip_pending=!!(mask&2);
        if(mask&4)put_word(GCM_CONTROL_GUEST_ADDR,s_kw_put+4);
        s_sema_live=!!(mask&8);
        if(mask&16)put_word(GCM_LABEL_GUEST_BASE+0x870,7);
        if(mask&32){s_park_off=s_fifo_getoff;put_word(0x100100,0x20000200);}
        int ready=!!((mask&7)||((mask&24)==24)||(mask&32));
        assert(cellGcm_fifo_work_ready()==ready);
        assert(cellGcm_fifo_work_ready()==ready);
        assert(s_gcm_kick_ev==NULL);
    }
    reset_state();s_gcm_initialized=0;s_kw_more=1;assert(!cellGcm_fifo_work_ready());
    reset_state();s_gcm_context_ea=0;s_kw_more=1;assert(!cellGcm_fifo_work_ready());
    s_gcm_syscall_mode=1;assert(cellGcm_fifo_work_ready());
    reset_state();s_park_off=s_fifo_getoff;assert(!cellGcm_fifo_work_ready());
    s_ea_address_table[0]=0xffff;assert(!cellGcm_fifo_work_ready());
    reset_state();s_park_off=s_fifo_getoff+4;put_word(0x100104,0);assert(!cellGcm_fifo_work_ready());
    reset_state();s_kw_more=1;pthread_t thread;
    assert(!pthread_create(&thread,NULL,hold_fifo_lock,NULL));
    while(!atomic_load(&locked))sched_yield();
    assert(!cellGcm_fifo_work_ready());
    atomic_store(&release_lock,1);assert(!pthread_join(thread,NULL));
    assert(cellGcm_fifo_work_ready());
    assert(TryAcquireSRWLockExclusive(&s_gcm_fifo_lock));ReleaseSRWLockExclusive(&s_gcm_fifo_lock);
    assert(!munmap(vm_base,size));
    puts("PASS: FIFO readiness states, repeat queries, absent context, mapped/parked commands and real lock contention");
}
