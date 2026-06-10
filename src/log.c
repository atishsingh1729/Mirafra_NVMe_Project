/**
 * @file log.c
 * @brief Timestamped logging implementation.
 */

#include "log.h"
#include <stdio.h>
#include <time.h>
#include <string.h>

static FILE        *g_log_fp;
static log_level_t  g_min_level;
static struct timespec g_start_time;

static const char *level_names[] = {
    "TRACE", "DEBUG", "INFO ", "WARN ", "ERROR"
};

int log_init(log_level_t level, const char *log_file)
{
    g_min_level = level;
    clock_gettime(CLOCK_MONOTONIC, &g_start_time);

    if (log_file) {
        g_log_fp = fopen(log_file, "w");
        if (!g_log_fp) {
            perror("log_init: fopen");
            return -1;
        }
    }
    return 0;
}

void log_shutdown(void)
{
    if (g_log_fp) {
        fflush(g_log_fp);
        fclose(g_log_fp);
        g_log_fp = NULL;
    }
}

static double elapsed_sec(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (now.tv_sec - g_start_time.tv_sec) +
           (now.tv_nsec - g_start_time.tv_nsec) / 1e9;
}

void log_msg(log_level_t level, const char *fmt, ...)
{
    if (level < g_min_level) return;

    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    double t = elapsed_sec();
    fprintf(stdout, "[%10.6f] %s  %s\n", t, level_names[level], buf);

    if (g_log_fp) {
        fprintf(g_log_fp, "[%10.6f] %s  %s\n", t, level_names[level], buf);
        fflush(g_log_fp);
    }
}

void log_mmio_read(uint32_t offset, int width, uint64_t value)
{
    if (g_min_level > LOG_TRACE) return;

    double t = elapsed_sec();
    if (width <= 32) {
        fprintf(stdout, "[%10.6f] TRACE  MMIO RD  off=0x%04X  w=%d  val=0x%08X\n",
                t, offset, width, (uint32_t)value);
    } else {
        fprintf(stdout, "[%10.6f] TRACE  MMIO RD  off=0x%04X  w=%d  val=0x%016llX\n",
                t, offset, width, (unsigned long long)value);
    }

    if (g_log_fp) {
        if (width <= 32)
            fprintf(g_log_fp, "[%10.6f] TRACE  MMIO RD  off=0x%04X  w=%d  val=0x%08X\n",
                    t, offset, width, (uint32_t)value);
        else
            fprintf(g_log_fp, "[%10.6f] TRACE  MMIO RD  off=0x%04X  w=%d  val=0x%016llX\n",
                    t, offset, width, (unsigned long long)value);
        fflush(g_log_fp);
    }
}

void log_mmio_write(uint32_t offset, int width, uint64_t value)
{
    if (g_min_level > LOG_TRACE) return;

    double t = elapsed_sec();
    if (width <= 32) {
        fprintf(stdout, "[%10.6f] TRACE  MMIO WR  off=0x%04X  w=%d  val=0x%08X\n",
                t, offset, width, (uint32_t)value);
    } else {
        fprintf(stdout, "[%10.6f] TRACE  MMIO WR  off=0x%04X  w=%d  val=0x%016llX\n",
                t, offset, width, (unsigned long long)value);
    }

    if (g_log_fp) {
        if (width <= 32)
            fprintf(g_log_fp, "[%10.6f] TRACE  MMIO WR  off=0x%04X  w=%d  val=0x%08X\n",
                    t, offset, width, (uint32_t)value);
        else
            fprintf(g_log_fp, "[%10.6f] TRACE  MMIO WR  off=0x%04X  w=%d  val=0x%016llX\n",
                    t, offset, width, (unsigned long long)value);
        fflush(g_log_fp);
    }
}
