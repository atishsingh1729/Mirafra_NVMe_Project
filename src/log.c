#include "log.h"
#include <stdio.h>
#include <time.h>
#include <stdarg.h>

static FILE *g_fp;
static log_level_t g_level;
static struct timespec g_t0;
static const char *g_names[] = {"TRACE","DEBUG","INFO ","WARN ","ERROR"};

int log_init(log_level_t level, const char *path) {
    g_level = level;
    clock_gettime(CLOCK_MONOTONIC, &g_t0);
    if (path) g_fp = fopen(path, "w");
    return 0;
}

void log_shutdown(void) {
    if (g_fp) { fflush(g_fp); fclose(g_fp); g_fp = NULL; }
}

static double elapsed(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (now.tv_sec - g_t0.tv_sec) + (now.tv_nsec - g_t0.tv_nsec) / 1e9;
}

void log_msg(log_level_t level, const char *fmt, ...) {
    if (level < g_level) return;
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    double t = elapsed();
    fprintf(stdout, "[%10.6f] %s  %s\n", t, g_names[level], buf);
    if (g_fp) { fprintf(g_fp, "[%10.6f] %s  %s\n", t, g_names[level], buf); fflush(g_fp); }
}

void log_mmio_read(uint32_t off, int w, uint64_t val) {
    if (g_level > LOG_TRACE) return;
    double t = elapsed();
    if (w <= 32)
        fprintf(stdout, "[%10.6f] TRACE  MMIO RD  0x%04X = 0x%08X\n", t, off, (uint32_t)val);
    else
        fprintf(stdout, "[%10.6f] TRACE  MMIO RD  0x%04X = 0x%016llX\n", t, off, (unsigned long long)val);
    if (g_fp) {
        if (w <= 32)
            fprintf(g_fp, "[%10.6f] TRACE  MMIO RD  0x%04X = 0x%08X\n", t, off, (uint32_t)val);
        else
            fprintf(g_fp, "[%10.6f] TRACE  MMIO RD  0x%04X = 0x%016llX\n", t, off, (unsigned long long)val);
        fflush(g_fp);
    }
}

void log_mmio_write(uint32_t off, int w, uint64_t val) {
    if (g_level > LOG_TRACE) return;
    double t = elapsed();
    if (w <= 32)
        fprintf(stdout, "[%10.6f] TRACE  MMIO WR  0x%04X = 0x%08X\n", t, off, (uint32_t)val);
    else
        fprintf(stdout, "[%10.6f] TRACE  MMIO WR  0x%04X = 0x%016llX\n", t, off, (unsigned long long)val);
    if (g_fp) {
        if (w <= 32)
            fprintf(g_fp, "[%10.6f] TRACE  MMIO WR  0x%04X = 0x%08X\n", t, off, (uint32_t)val);
        else
            fprintf(g_fp, "[%10.6f] TRACE  MMIO WR  0x%04X = 0x%016llX\n", t, off, (unsigned long long)val);
        fflush(g_fp);
    }
}
