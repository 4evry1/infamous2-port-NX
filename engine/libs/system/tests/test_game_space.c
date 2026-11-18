/* macOS: clang -std=gnu17 -I include libs/system/tests/test_game_space.c
 * -Wl,-dead_strip -o /tmp/test_game_space && /tmp/test_game_space */
#include <sys/statvfs.h>
#include <assert.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
static unsigned queries;
static unsigned long long blocks = 3ULL * 1024 * 1024;
static unsigned long block_size = 4096;
static int denied;
static int test_statvfs(const char* path, struct statvfs* out)
{
    queries++;
    if (denied) { errno = EACCES; return -1; }
    if (strcmp(path, "/") != 0 && strcmp(path, ".") != 0) { errno = ENOENT; return -1; }
    memset(out, 0, sizeof(*out));
    out->f_frsize = block_size;
    out->f_bsize = 4096;
    out->f_bavail = blocks;
    return 0;
}
#define statvfs(path, out) test_statvfs(path, out)
#include "../cellGame.c"
const char* ppu_vfs_root = ".";
uint8_t* vm_base;
uint32_t ppu_vm_size;
int g_resv_store_active;
uint32_t g_ww_lo, g_ww_hi;
void ppu_resv_break_store(uint64_t ea) { (void)ea; }
void ps3_ww_report_inline(uint32_t a, uint64_t v, int w) { (void)a; (void)v; (void)w; }
int spu_coh_is_reserved(uint32_t a) { (void)a; return 0; }
void spu_coh_notify_write(uint32_t a) { (void)a; }
void spu_lockline_lock(void) {}
void spu_lockline_unlock(void) {}

uint64_t spu_lockline_mask_range(uint32_t a, uint32_t n) { (void)a; (void)n; return 0; }
void spu_lockline_lock_mask(uint64_t mask) { (void)mask; }
void spu_lockline_unlock_mask(uint64_t mask) { (void)mask; }
void spu_coh_notify_write_from_ppu(uint32_t a) { (void)a; }
ps3_guest_caller_fn g_ps3_guest_caller;
int main(void)
{
    vm_base = calloc(1, 65536); assert(vm_base);
    cellGame_set_content_path("/ps3recomp-missing-space-test/content/game");
    memcpy(vm_base + 0x100, "TEST00000", 10);
    assert(cellGameDataCheck(3, (void*)0x100, (void*)0x200) == CELL_GAME_RET_NONE);
    assert(queries == 4);
    assert(vm_read32(0x200) == 40u * 1024u * 1024u - 256u);
    assert(vm_read32(0x204) == 0 && vm_read32(0x208) == 0);
    assert(vm_base[0x200] == 2 && vm_base[0x201] == 0x7F);
    blocks = UINT64_MAX;
    assert(content_free_kb_raw() == INT32_MAX);
    blocks = 0; assert(content_free_kb() == 0);
    blocks = 256; block_size = 0; assert(content_free_kb() == 1024);
    denied = 1; queries = 0;
    assert(content_free_kb() == 0 && queries == 1);
    char root[] = "/tmp/i2-installed-size-XXXXXX";
    assert(mkdtemp(root) && chdir(root) == 0);
    cellGame_set_content_path(".");
    assert(mkdir("TEST00000", 0700) == 0);
    assert(mkdir("TEST00000/USRDIR", 0700) == 0);
    int fd = open("TEST00000/USRDIR/data", O_CREAT | O_WRONLY, 0600);
    assert(fd >= 0 && ftruncate(fd, 2552499687LL) == 0 && close(fd) == 0);
    assert(symlink("..", "TEST00000/USRDIR/cycle") == 0);
    denied = 0;
    blocks = 2542010368ULL / 4096;
    block_size = 4096;
    assert(cellGameDataCheck(3, (void*)0x100, (void*)0x200) == CELL_OK);
    assert(vm_read32(0x200) == 2482432 && vm_read32(0x204) == UINT32_MAX);
    assert(cellGameGetSizeKB((void*)0x300) == CELL_OK);
#ifdef CELL_GAME_TEST_INSTALLED_SIZE
    assert(vm_read32(0x300) == (2552499687ULL + 1023) / 1024);
#else
    assert(vm_read32(0x300) == 0);
#endif
    assert(unlink("TEST00000/USRDIR/data") == 0 && unlink("TEST00000/USRDIR/cycle") == 0);
    assert(cellGameGetSizeKB((void*)0x300) == CELL_OK && vm_read32(0x300) == 0);
    assert(cellGameGetSizeKB(NULL) == CELL_GAME_ERROR_PARAM);
    memcpy(vm_base + 0x100, "..", 3);
    assert(cellGameDataCheck(3, (void*)0x100, NULL) == CELL_OK);
#ifdef CELL_GAME_TEST_INSTALLED_SIZE
    assert(cellGameGetSizeKB((void*)0x300) == CELL_GAME_ERROR_PARAM);
#else
    assert(cellGameGetSizeKB((void*)0x300) == CELL_OK && vm_read32(0x300) == 0);
#endif
    memcpy(vm_base + 0x100, "MISSING", 8);
    assert(cellGameDataCheck(3, (void*)0x100, (void*)0x200) == CELL_GAME_RET_NONE);
    assert(vm_read32(0x204) == 0);
    assert(rmdir("TEST00000/USRDIR") == 0 && rmdir("TEST00000") == 0);
    assert(chdir("/") == 0 && rmdir(root) == 0);
    free(vm_base);
    puts("Game storage availability and guest-endian checks passed");
}
