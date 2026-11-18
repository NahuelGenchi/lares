#pragma once
#include <gio/gio.h>

/* The PC's own numbers, read from /proc and /sys. Nothing is read while no
 * one is looking: sections ask for samples with hardware_want(). */

typedef struct {
    /* Fractions run from 0 to 1; a negative fraction or a NAN means unknown. */
    double cpu, cpu_temp;
    double gpu, gpu_temp, gpu_watts, gpu_fan_rpm;
    guint64 vram_used, vram_total;
    guint64 mem_used, mem_total;
    guint64 disk_used, disk_total;
    double disk_temp;
} HwSample;

typedef struct {
    char *cpu_name;
    int threads;
    char *gpu_name;      /* NULL when the GPU reports no load (no AMD sysfs) */
    char *disk_name;
} HwInfo;

typedef struct Hardware Hardware;
typedef void (*HwNotify)(gpointer user);

Hardware *hardware_new(void);
const HwInfo *hardware_info(Hardware *hw);
const HwSample *hardware_sample(Hardware *hw);
void hardware_watch(Hardware *hw, HwNotify fn, gpointer user);
void hardware_want(Hardware *hw, gboolean on);
/* Also read the power, fan, video memory and drive, which some sections
 * don't show. */
void hardware_want_details(Hardware *hw, gboolean on);

char *format_bytes(guint64 bytes);
