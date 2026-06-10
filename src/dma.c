/**
 * @file dma.c
 * @brief DMA buffer allocation, virt-to-phys, and cache maintenance.
 */

#include "dma.h"
#include "log.h"
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>

#define CACHE_LINE_SIZE 64  /* ARM Cortex-A76 (BCM2712) cache line */

uint64_t dma_virt_to_phys(void *virt)
{
    long page_size = sysconf(_SC_PAGESIZE);
    int fd = open("/proc/self/pagemap", O_RDONLY);
    if (fd < 0) return 0;

    uint64_t vaddr = (uint64_t)virt;
    uint64_t entry;
    off_t offset = (vaddr / page_size) * sizeof(entry);

    if (lseek(fd, offset, SEEK_SET) < 0 ||
        read(fd, &entry, sizeof(entry)) != sizeof(entry)) {
        close(fd);
        return 0;
    }
    close(fd);

    if (!(entry & (1ULL << 63))) return 0; /* page not present */

    uint64_t pfn = entry & ((1ULL << 55) - 1);
    return (pfn * page_size) | (vaddr & (page_size - 1));
}

int dma_alloc(struct dma_buffer *buf, size_t size, uint64_t dma_offset)
{
    long page_size = sysconf(_SC_PAGESIZE);
    size_t alloc_size = (size + page_size - 1) & ~(page_size - 1);
    if (alloc_size < (size_t)page_size) alloc_size = page_size;

    buf->virt = mmap(NULL, alloc_size, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE, -1, 0);
    if (buf->virt == MAP_FAILED) {
        buf->virt = NULL;
        log_msg(LOG_ERROR, "dma_alloc: mmap failed for %zu bytes", alloc_size);
        return -1;
    }

    if (mlock(buf->virt, alloc_size) < 0) {
        log_msg(LOG_ERROR, "dma_alloc: mlock failed (run as root?)");
        munmap(buf->virt, alloc_size);
        buf->virt = NULL;
        return -1;
    }

    /* Zero and flush to RAM — cache lines must be CLEAN before DMA */
    memset(buf->virt, 0, alloc_size);
    dma_flush(buf->virt, alloc_size);

    buf->phys = dma_virt_to_phys(buf->virt);
    if (buf->phys == 0) {
        log_msg(LOG_ERROR, "dma_alloc: virt_to_phys failed");
        munlock(buf->virt, alloc_size);
        munmap(buf->virt, alloc_size);
        buf->virt = NULL;
        return -1;
    }

    buf->bus  = buf->phys + dma_offset;
    buf->size = alloc_size;

    log_msg(LOG_DEBUG, "DMA buffer: virt=%p  phys=0x%llX  bus=0x%llX  size=%zu",
            buf->virt, (unsigned long long)buf->phys,
            (unsigned long long)buf->bus, buf->size);
    return 0;
}

void dma_free(struct dma_buffer *buf)
{
    if (buf->virt) {
        munlock(buf->virt, buf->size);
        munmap(buf->virt, buf->size);
        memset(buf, 0, sizeof(*buf));
    }
}

void dma_flush(void *addr, size_t len)
{
#if defined(__aarch64__)
    uint64_t line = (uint64_t)addr & ~(CACHE_LINE_SIZE - 1);
    uint64_t end  = (uint64_t)addr + len;
    for (; line < end; line += CACHE_LINE_SIZE)
        __asm__ volatile("dc cvac, %0" :: "r"(line) : "memory");
    __asm__ volatile("dsb sy" ::: "memory");
#else
    (void)addr; (void)len;
    __sync_synchronize();
#endif
}

void dma_invalidate(void *addr, size_t len)
{
#if defined(__aarch64__)
    uint64_t line = (uint64_t)addr & ~(CACHE_LINE_SIZE - 1);
    uint64_t end  = (uint64_t)addr + len;
    for (; line < end; line += CACHE_LINE_SIZE)
        __asm__ volatile("dc civac, %0" :: "r"(line) : "memory");
    __asm__ volatile("dsb sy" ::: "memory");
#else
    (void)addr; (void)len;
    __sync_synchronize();
#endif
}
