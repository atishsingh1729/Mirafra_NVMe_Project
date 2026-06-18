#ifndef NVME_LOG_H
#define NVME_LOG_H
#include <stdint.h>

typedef enum { LOG_TRACE, LOG_DEBUG, LOG_INFO, LOG_WARN, LOG_ERROR } log_level_t;

int  log_init(log_level_t level, const char *log_file);
void log_shutdown(void);
void log_msg(log_level_t level, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));
void log_mmio_read(uint32_t offset, int width, uint64_t value);
void log_mmio_write(uint32_t offset, int width, uint64_t value);

#endif
