#include "dma.h"
#include "log.h"
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>

#define CACHE_LINE 64

uint64_t dma_virt_to_phys(void *virt) {
    long ps = sysconf(_SC_PAGESIZE);
    int fd = open("/proc/self/pagemap", O_RDONLY);
    if (fd < 0) return 0;
    uint64_t entry;
    off_t off = ((uint64_t)virt / ps) * sizeof(entry);
    if (lseek(fd, off, SEEK_SET) < 0 || read(fd, &entry, 8) != 8) { close(fd); return 0; }
    close(fd);
    if (!(entry & (1ULL << 63))) return 0;
    uint64_t pfn = entry & ((1ULL << 55) - 1);
    return pfn * ps + ((uint64_t)virt & (ps - 1));
}

int dma_alloc(struct dma_buffer *buf, size_t size, uint64_t dma_offset) {
    long ps = sysconf(_SC_PAGESIZE);
    size_t sz = (size + ps - 1) & ~(ps - 1);
    if (sz < (size_t)ps) sz = ps;

    buf->virt = mmap(NULL, sz, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE, -1, 0);
    if (buf->virt == MAP_FAILED) { buf->virt = NULL; return -1; }
    if (mlock(buf->virt, sz) < 0) { munmap(buf->virt, sz); buf->virt = NULL; return -1; }

    memset(buf->virt, 0, sz);
    dma_flush(buf->virt, sz);

    buf->phys = dma_virt_to_phys(buf->virt);
    if (buf->phys == 0) { munlock(buf->virt, sz); munmap(buf->virt, sz); buf->virt = NULL; return -1; }
    buf->bus = buf->phys + dma_offset;
    buf->size = sz;
    return 0;
}

void dma_free(struct dma_buffer *buf) {
    if (buf->virt) { munlock(buf->virt, buf->size); munmap(buf->virt, buf->size); }
    memset(buf, 0, sizeof(*buf));
}

void dma_flush(void *addr, size_t len) {
#if defined(__aarch64__)
    uint64_t a = (uint64_t)addr & ~(CACHE_LINE - 1);
    for (; a < (uint64_t)addr + len; a += CACHE_LINE)
        __asm__ volatile("dc cvac, %0" :: "r"(a) : "memory");
    __asm__ volatile("dsb sy" ::: "memory");
#else
    (void)addr; (void)len; __sync_synchronize();
#endif
}

void dma_invalidate(void *addr, size_t len) {
#if defined(__aarch64__)
    uint64_t a = (uint64_t)addr & ~(CACHE_LINE - 1);
    for (; a < (uint64_t)addr + len; a += CACHE_LINE)
        __asm__ volatile("dc civac, %0" :: "r"(a) : "memory");
    __asm__ volatile("dsb sy" ::: "memory");
#else
    (void)addr; (void)len; __sync_synchronize();
#endif
}
