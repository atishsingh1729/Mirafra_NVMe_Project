#ifndef NVME_DMA_H
#define NVME_DMA_H
#include <stdint.h>
#include <stddef.h>

struct dma_buffer {
    void *virt; uint64_t phys; uint64_t bus; size_t size;
};

int      dma_alloc(struct dma_buffer *buf, size_t size, uint64_t dma_offset);
void     dma_free(struct dma_buffer *buf);
void     dma_flush(void *addr, size_t len);
void     dma_invalidate(void *addr, size_t len);
uint64_t dma_virt_to_phys(void *virt);
#endif
