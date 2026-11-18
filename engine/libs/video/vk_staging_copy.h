#ifndef PS3RECOMP_VK_STAGING_COPY_H
#define PS3RECOMP_VK_STAGING_COPY_H

#include <stddef.h>
#include <string.h>

/* Source and destination must be disjoint, as for memcpy. */
static inline void vk_staging_copy(void* destination, const void* source, size_t bytes)
{
#ifdef __SWITCH__
    unsigned char* dst = (unsigned char*)destination;
    const unsigned char* src = (const unsigned char*)source;
    /* Fixed blocks let the A57 compiler use vector loads/stores; unlike an
     * overlapping head/tail copy, every access stays within the byte range. */
    while (bytes >= 64) {
        unsigned char block[64];
        memcpy(block, src, sizeof block);
        memcpy(dst, block, sizeof block);
        src += sizeof block;
        dst += sizeof block;
        bytes -= sizeof block;
    }
    if (bytes) memcpy(dst, src, bytes);
#else
    if (bytes) memcpy(destination, source, bytes);
#endif
}

#endif
