/* Targeted render-pass timestamps. The normal command recording wrappers enqueue
 * reset and write commands in order. Each slot is read only after its own fence. */
#define SHADOW_QUERY_CAP 256u
typedef struct {
    VkQueryPool pool;
    vk_buffer readback;
    u32 count, dropped;
    int open;
    uint64_t record_ns;
} shadow_query_slot;
static struct {
    int initialized, enabled;
    double ns_per_tick;
    shadow_query_slot slots[VK_FIF_MAX];
} shadow_queries;

static void vk_shadow_query_destroy(void)
{
    for (u32 i = 0; i < VK_FIF_MAX; ++i) {
        vk_destroy_idle_buffer(&shadow_queries.slots[i].readback);
        if (shadow_queries.slots[i].pool)
            pvkDestroyQueryPool(s_vk.device, shadow_queries.slots[i].pool, NULL);
    }
    memset(&shadow_queries, 0, sizeof shadow_queries);
}

static void vk_shadow_query_batch_begin(void)
{
    if (!shadow_queries.initialized) {
        shadow_queries.initialized = 1;
        const char* route = getenv("PS3RECOMP_ROUTE_TIMING");
        if (!route || strcmp(route, "1")) return;
        if (!s_vk.timestamp_bits || s_vk.timestamp_bits > 64) {
            fprintf(stderr, "[shadow-gpu] queue timestamps unavailable\n");
            return;
        }
        VkPhysicalDeviceProperties props;
        pvkGetPhysicalDeviceProperties(s_vk.phys, &props);
        shadow_queries.ns_per_tick = props.limits.timestampPeriod;
        VkQueryPoolCreateInfo info = { .sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
            .queryType = VK_QUERY_TYPE_TIMESTAMP, .queryCount = SHADOW_QUERY_CAP };
        for (u32 i = 0; i < VK_FIF_MAX; ++i) {
            if (pvkCreateQueryPool(s_vk.device, &info, NULL, &shadow_queries.slots[i].pool) != VK_SUCCESS) {
                vk_shadow_query_destroy();
                shadow_queries.initialized = 1;
                fprintf(stderr, "[shadow-gpu] pool creation failed\n");
                return;
            }
            /* Coherent memory permits the CPU to clear validity before reuse.
             * Other heaps retain the nonblocking host-query path. */
            vk_buffer *buf = &shadow_queries.slots[i].readback;
            if (vk_create_host_buffer(buf, SHADOW_QUERY_CAP * 2 * sizeof(uint64_t), VK_BUFFER_USAGE_TRANSFER_DST_BIT) ||
                !buf->coherent)
                vk_destroy_idle_buffer(buf);
        }
        shadow_queries.enabled = 1;
    }
    if (shadow_queries.enabled) {
        shadow_query_slot* slot = &shadow_queries.slots[s_fif_cur];
        slot->count = slot->dropped = 0;
        slot->open = 0;
    }
}

static void vk_shadow_query_begin(void)
{
    if (!shadow_queries.enabled) return;
    shadow_query_slot* slot = &shadow_queries.slots[s_fif_cur];
    if (slot->count + 2 > SHADOW_QUERY_CAP) { slot->dropped++; return; }
    if (!slot->count) {
        slot->record_ns = ps3_ft_now();
        if (slot->readback.ptr) memset(slot->readback.ptr, 0, slot->readback.size);
        pvkCmdResetQueryPool(s_vk.cmd, slot->pool, 0, SHADOW_QUERY_CAP);
    }
    pvkCmdWriteTimestamp(s_vk.cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, slot->pool, slot->count++);
    slot->open = 1;
}

static void vk_shadow_query_end(void)
{
    shadow_query_slot* slot = &shadow_queries.slots[s_fif_cur];
    if (!slot->open) return;
    pvkCmdWriteTimestamp(s_vk.cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, slot->pool, slot->count++);
    slot->open = 0;
}

static void vk_shadow_query_resolve(void)
{
    shadow_query_slot* slot = &shadow_queries.slots[s_fif_cur];
    if (slot->count && slot->readback.ptr) {
        pvkCmdCopyQueryPoolResults(s_vk.cmd, slot->pool, 0, slot->count, slot->readback.buf, 0,
            2 * sizeof(uint64_t), VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
        VkMemoryBarrier barrier = { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT };
        pvkCmdPipelineBarrier(s_vk.cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                             0, 1, &barrier, 0, NULL, 0, NULL);
    }
}

static void vk_shadow_query_collect(void)
{
    shadow_query_slot* slot = &shadow_queries.slots[s_fif_cur];
    if (!slot->count) return;
    uint64_t ts[SHADOW_QUERY_CAP];
    u32 n = slot->count;
    slot->count = 0;
    VkResult rc = pvkGetQueryPoolResults(s_vk.device, slot->pool, 0, n,
        sizeof ts, ts, sizeof ts[0], VK_QUERY_RESULT_64_BIT);
    int copied = 0;
    if (rc == VK_NOT_READY && slot->readback.ptr) {
        const uint64_t *results = (const uint64_t *)slot->readback.ptr;
        for (u32 i = 0; i < n; ++i) {
            if (!results[2*i+1]) break;
            ts[i] = results[2*i];
            if (i+1 == n) { rc = VK_SUCCESS; copied = 1; }
        }
    }
    const uint64_t mask = UINT64_MAX >> (64 - s_vk.timestamp_bits);
    double ns = 0;
    if (rc == VK_SUCCESS)
        for (u32 i = 1; i < n; i += 2) ns += ((ts[i] - ts[i-1]) & mask) * shadow_queries.ns_per_tick;
    fprintf(stderr, "[shadow-gpu] record_ns=%llu passes=%u gpu_ms=%.3f dropped=%u result=%d copied=%d\n",
        (unsigned long long)slot->record_ns, n/2, rc == VK_SUCCESS ? ns/1e6 : -1.0, slot->dropped, (int)rc, copied);
}
