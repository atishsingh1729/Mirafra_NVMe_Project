#ifndef NVME_TRANSPORT_H
#define NVME_TRANSPORT_H

#include "nvme_queue.h"
#include "pci.h"
#include "dma.h"

struct transport_req {
    uint8_t  opcode;
    uint32_t nsid;
    uint64_t lba;
    uint32_t num_blocks;
    void    *buf;
    size_t   buf_len;
    uint32_t cdw10, cdw11;
    int      is_io;         /* 1 for Read/Write, 0 for admin cmds */

    /* Filled by transport */
    struct dma_buffer dma;
    uint16_t status;
    uint32_t result;
    int      completed;
};

typedef int  (*hook_pre_submit_fn)(struct transport_req *req, void *ctx);
typedef void (*hook_post_complete_fn)(struct transport_req *req, void *ctx);
typedef int  (*hook_reset_fn)(void *ctx);

struct transport {
    struct pci_device *pci;
    struct nvme_qpair  admin;
    struct nvme_qpair  io;
    uint32_t dstrd, to_ms, block_size;
    int io_ready;

    hook_pre_submit_fn     pre_submit;
    hook_post_complete_fn  post_complete;
    hook_reset_fn          reset;
    void *hook_ctx;
};

int  transport_init(struct transport *t, struct pci_device *pci);
int  transport_create_io_queue(struct transport *t, uint16_t depth);
int  transport_submit(struct transport *t, struct transport_req *req);
int  transport_reset(struct transport *t);
void transport_shutdown(struct transport *t);
void transport_set_hooks(struct transport *t, hook_pre_submit_fn pre,
                         hook_post_complete_fn post, hook_reset_fn rst, void *ctx);

#endif
