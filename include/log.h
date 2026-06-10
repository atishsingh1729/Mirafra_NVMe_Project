/**
 * @file log.h
 * @brief Timestamped logging subsystem for NVMe firmware project.
 *
 * Every MMIO (Memory-Mapped I/O) read/write flows through the logger
 * so we have a full trace of hardware interactions. Logs go to both
 * stdout and an optional file.
 *
 * Analogy: A flight recorder for register access — if something
 * crashes, we can replay the exact sequence of reads and writes.
 */

#ifndef NVME_LOG_H
#define NVME_LOG_H

#include <stdint.h>
#include <stdarg.h>

/** Log severity levels */
typedef enum {
    LOG_TRACE,    /**< Register-level detail (every MMIO access)  */
    LOG_DEBUG,    /**< Internal state changes                     */
    LOG_INFO,     /**< Milestone progress, key events             */
    LOG_WARN,     /**< Recoverable issues                         */
    LOG_ERROR,    /**< Failures                                   */
} log_level_t;

/**
 * @brief Initialise the logger.
 * @param level     Minimum level to display (lower = more verbose)
 * @param log_file  Path to log file (NULL for stdout only)
 * @return 0 on success, -1 on failure
 */
int  log_init(log_level_t level, const char *log_file);

/** Shut down the logger and flush any buffered output. */
void log_shutdown(void);

/**
 * @brief Log a message with timestamp and severity tag.
 * @param level   Severity level
 * @param fmt     printf-style format string
 *
 * Timestamp format: [seconds.microseconds] relative to log_init().
 * Example: [  0.001234] INFO  Controller version: NVMe 1.3.0
 */
void log_msg(log_level_t level, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

/**
 * @brief Log an MMIO (Memory-Mapped I/O) register read.
 * @param offset  Register offset within BAR0
 * @param width   Access width in bits (32 or 64)
 * @param value   Value read
 *
 * Example: [  0.000512] TRACE MMIO RD  offset=0x0000  width=32  val=0x0030003F
 */
void log_mmio_read(uint32_t offset, int width, uint64_t value);

/**
 * @brief Log an MMIO register write.
 * @param offset  Register offset within BAR0
 * @param width   Access width in bits (32 or 64)
 * @param value   Value written
 */
void log_mmio_write(uint32_t offset, int width, uint64_t value);

#endif /* NVME_LOG_H */
