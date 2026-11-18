#include "hardware.h"
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/statvfs.h>
#include <unistd.h>

#define SAMPLE_SECONDS 1

typedef struct { HwNotify fn; gpointer user; } Watcher;

struct Hardware {
    HwInfo info;
    HwSample now;
    char *cpu_temp_path, *gpu_dir, *gpu_hwmon, *disk_temp_path;
    guint64 last_busy, last_total;   /* only the sampling thread touches these */
    GArray *watchers;
    int wanted;
    gint details;                    /* sections showing power, fan, video memory and drive temperature */
    gint fresh_load;                 /* the next load reading starts over */
    gboolean sampling;               /* a sample is being read */
    guint timer;
};

/* Small sysfs and procfs reads go through one fixed buffer: no allocation
 * per sample. */
static gboolean read_small(const char *path, char *buf, size_t size)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return FALSE;
    ssize_t n = read(fd, buf, size - 1);
    close(fd);
    if (n <= 0) return FALSE;
    buf[n] = 0;
    return TRUE;
}

static gboolean read_number(const char *dir, const char *file, double *out)
{
    char path[512], buf[64];
    if (dir) snprintf(path, sizeof path, "%s/%s", dir, file);
    else snprintf(path, sizeof path, "%s", file);
    if (!read_small(path, buf, sizeof buf)) return FALSE;
    *out = strtod(buf, NULL);
    return TRUE;
}

/* The hwmon folder whose name is one of `names`, or NULL. */
static char *find_hwmon(const char *const *names)
{
    GDir *d = g_dir_open("/sys/class/hwmon", 0, NULL);
    if (!d) return NULL;
    const char *e;
    char *found = NULL;
    while (!found && (e = g_dir_read_name(d))) {
        char path[512], name[64];
        snprintf(path, sizeof path, "/sys/class/hwmon/%s/name", e);
        if (!read_small(path, name, sizeof name)) continue;
        g_strchomp(name);
        for (int i = 0; names[i] && !found; i++)
            if (g_str_equal(name, names[i])) found = g_strdup_printf("/sys/class/hwmon/%s", e);
    }
    g_dir_close(d);
    return found;
}

/* "Intel(R) Core(TM) i5-8400 CPU @ 2.80GHz" from /proc/cpuinfo, without the marks and speed. */
static char *cpu_name(void)
{
    char *text = NULL;
    if (!g_file_get_contents("/proc/cpuinfo", &text, NULL, NULL)) return g_strdup("Processor");
    char *name = NULL;
    char *p = strstr(text, "model name");
    if (p && (p = strchr(p, ':'))) {
        char *end = strchr(p, '\n');
        name = g_strndup(p + 1, end ? (gsize)(end - p - 1) : strlen(p + 1));
    }
    g_free(text);
    if (!name) return g_strdup("Processor");
    GString *s = g_string_new(g_strstrip(name));
    g_string_replace(s, "(R)", "", 0);
    g_string_replace(s, "(TM)", "", 0);
    g_string_replace(s, " CPU", "", 0);
    char *at = strstr(s->str, " @");
    if (at) g_string_truncate(s, at - s->str);
    g_free(name);
    return g_string_free(s, FALSE);
}

/* A GPU's marketing name from the PCI id list, read only up to its entry:
 * "Tonga PRO [Radeon R9 285/380]" becomes "AMD Radeon R9 285/380". */
static char *gpu_name(const char *dev)
{
    double vendor, device;
    if (!read_number(dev, "vendor", &vendor) || !read_number(dev, "device", &device)) return NULL;
    char vid[8], did[8], *name = NULL;
    snprintf(vid, sizeof vid, "%04x", (unsigned)vendor);
    snprintf(did, sizeof did, "%04x", (unsigned)device);
    const char *brand = (unsigned)vendor == 0x1002 ? "AMD" : (unsigned)vendor == 0x10de ? "NVIDIA"
                      : (unsigned)vendor == 0x8086 ? "Intel" : NULL;
    FILE *f = fopen("/usr/share/hwdata/pci.ids", "r");
    if (!f) f = fopen("/usr/share/misc/pci.ids", "r");
    if (f) {
        char line[256];
        gboolean in_vendor = FALSE;
        while (fgets(line, sizeof line, f)) {
            if (line[0] != '\t' && line[0] != '#') {
                if (in_vendor) break;
                in_vendor = g_str_has_prefix(line, vid);
            } else if (in_vendor && line[0] == '\t' && line[1] != '\t' && g_str_has_prefix(line + 1, did)) {
                char *text = g_strstrip(line + 1 + 4);
                char *open = strchr(text, '['), *close = open ? strchr(open, ']') : NULL;
                if (open && close) text = g_strndup(open + 1, close - open - 1);
                else text = g_strdup(text);
                name = brand ? g_strdup_printf("%s %s", brand, text) : g_strdup(text);
                g_free(text);
                break;
            }
        }
        fclose(f);
    }
    return name ? name : g_strdup(brand ? brand : "Graphics");
}

/* An AMD card exposes its load in sysfs; other cards are left out. */
static void find_gpu(Hardware *hw)
{
    for (int i = 0; i < 8; i++) {
        char dev[64], probe[128];
        snprintf(dev, sizeof dev, "/sys/class/drm/card%d/device", i);
        snprintf(probe, sizeof probe, "%s/gpu_busy_percent", dev);
        if (!g_file_test(probe, G_FILE_TEST_EXISTS)) continue;
        hw->gpu_dir = g_strdup(dev);
        hw->info.gpu_name = gpu_name(dev);
        char hwdir[128];
        snprintf(hwdir, sizeof hwdir, "%s/hwmon", dev);
        GDir *d = g_dir_open(hwdir, 0, NULL);
        const char *e = d ? g_dir_read_name(d) : NULL;
        if (e) hw->gpu_hwmon = g_strdup_printf("%s/%s", hwdir, e);
        if (d) g_dir_close(d);
        return;
    }
}

static void sample_cpu(Hardware *hw, HwSample *out)
{
    char buf[256];
    out->cpu = -1;
    if (!read_small("/proc/stat", buf, sizeof buf)) return;
    guint64 v[10] = { 0 };
    sscanf(buf, "cpu %lu %lu %lu %lu %lu %lu %lu %lu", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7]);
    guint64 idle = v[3] + v[4], total = 0;
    for (int i = 0; i < 8; i++) total += v[i];
    guint64 busy = total - idle;
    if (hw->last_total && total > hw->last_total)
        out->cpu = (double)(busy - hw->last_busy) / (double)(total - hw->last_total);
    hw->last_busy = busy;
    hw->last_total = total;
}

static void sample_memory(HwSample *out)
{
    char buf[512];
    if (!read_small("/proc/meminfo", buf, sizeof buf)) return;
    guint64 total = 0, avail = 0;
    char *p = strstr(buf, "MemTotal:"), *q = strstr(buf, "MemAvailable:");
    if (p) total = strtoull(p + 9, NULL, 10) * 1024;
    if (q) avail = strtoull(q + 13, NULL, 10) * 1024;
    out->mem_total = total;
    out->mem_used = total > avail ? total - avail : 0;
}

/* Reading runs in a worker thread: some files block. On AMD cards,
 * power1_average makes the driver ask the card's power controller and
 * takes about half a second, which would freeze the window each time.
 * That one and the other details are read only while the Hardware section
 * shows them. */
typedef struct { Hardware *hw; HwSample s; } Job;

static void sample_thread(GTask *task, gpointer src, gpointer data, GCancellable *cancel)
{
    Job *job = data;
    Hardware *hw = job->hw;
    HwSample *s = &job->s;
    double v;
    if (g_atomic_int_compare_and_exchange(&hw->fresh_load, 1, 0)) hw->last_total = 0;

    sample_cpu(hw, s);
    s->cpu_temp = hw->cpu_temp_path && read_number(NULL, hw->cpu_temp_path, &v) ? v / 1000 : NAN;
    sample_memory(s);
    if (hw->gpu_dir) s->gpu = read_number(hw->gpu_dir, "gpu_busy_percent", &v) ? v / 100 : -1;
    if (hw->gpu_hwmon) s->gpu_temp = read_number(hw->gpu_hwmon, "temp1_input", &v) ? v / 1000 : NAN;
    struct statvfs fs;
    if (statvfs("/", &fs) == 0) {
        s->disk_total = (guint64)fs.f_blocks * fs.f_frsize;
        s->disk_used = s->disk_total - (guint64)fs.f_bavail * fs.f_frsize;
    }

    if (g_atomic_int_get(&hw->details) > 0) {
        if (hw->gpu_dir) {
            s->vram_used = read_number(hw->gpu_dir, "mem_info_vram_used", &v) ? (guint64)v : 0;
            s->vram_total = read_number(hw->gpu_dir, "mem_info_vram_total", &v) ? (guint64)v : 0;
        }
        if (hw->gpu_hwmon) {
            s->gpu_fan_rpm = read_number(hw->gpu_hwmon, "fan1_input", &v) ? v : NAN;
            s->gpu_watts = read_number(hw->gpu_hwmon, "power1_average", &v) ? v / 1e6 : NAN;
        }
        /* An NVMe drive answers its temperature with a command of its own,
         * a few milliseconds that also keep it awake. */
        s->disk_temp = hw->disk_temp_path && read_number(NULL, hw->disk_temp_path, &v) ? v / 1000 : NAN;
    }
    g_task_return_boolean(task, TRUE);
}

static void sample_done(GObject *src, GAsyncResult *res, gpointer data)
{
    Hardware *hw = data;
    hw->sampling = FALSE;
    if (!g_task_propagate_boolean(G_TASK(res), NULL) || hw->wanted <= 0) return;
    hw->now = ((Job *)g_task_get_task_data(G_TASK(res)))->s;
    for (guint i = 0; i < hw->watchers->len; i++) {
        Watcher *w = &g_array_index(hw->watchers, Watcher, i);
        w->fn(w->user);
    }
}

/* A sample still being read when the next is due simply makes that one wait. */
static gboolean sample(gpointer data)
{
    Hardware *hw = data;
    if (hw->sampling) return G_SOURCE_CONTINUE;
    hw->sampling = TRUE;
    GTask *task = g_task_new(NULL, NULL, sample_done, hw);
    Job *job = g_new(Job, 1);
    job->hw = hw;
    job->s = hw->now;     /* starts from the last sample, so unread details stay */
    g_task_set_task_data(task, job, g_free);
    g_task_run_in_thread(task, sample_thread);
    g_object_unref(task);
    return G_SOURCE_CONTINUE;
}

void hardware_want(Hardware *hw, gboolean on)
{
    hw->wanted += on ? 1 : -1;
    if (hw->wanted > 0 && !hw->timer) {
        sample(hw);
        /* Whole-second timers wake together with the rest of the desktop's,
         * so the CPU stays asleep longer. */
        hw->timer = g_timeout_add_seconds(SAMPLE_SECONDS, sample, hw);
    } else if (hw->wanted <= 0 && hw->timer) {
        g_source_remove(hw->timer);
        hw->timer = 0;
        hw->wanted = 0;
        g_atomic_int_set(&hw->fresh_load, 1);
    }
}

void hardware_want_details(Hardware *hw, gboolean on)
{
    g_atomic_int_add(&hw->details, on ? 1 : -1);
    if (on) sample(hw);   /* the details now, not a second later */
}

void hardware_watch(Hardware *hw, HwNotify fn, gpointer user)
{
    Watcher w = { fn, user };
    g_array_append_val(hw->watchers, w);
}

const HwInfo *hardware_info(Hardware *hw) { return &hw->info; }
const HwSample *hardware_sample(Hardware *hw) { return &hw->now; }

char *format_bytes(guint64 bytes)
{
    double gb = bytes / 1073741824.0;
    if (gb >= 1000) return g_strdup_printf("%.1f TB", gb / 1024);
    if (gb >= 10) return g_strdup_printf("%.0f GB", gb);
    return g_strdup_printf("%.1f GB", gb);
}

Hardware *hardware_new(void)
{
    Hardware *hw = g_new0(Hardware, 1);
    hw->watchers = g_array_new(FALSE, FALSE, sizeof(Watcher));
    hw->info.cpu_name = cpu_name();
    hw->info.threads = (int)g_get_num_processors();

    static const char *const cpu_sensors[] = { "coretemp", "k10temp", "zenpower", "cpu_thermal", NULL };
    char *cpu = find_hwmon(cpu_sensors);
    if (cpu) hw->cpu_temp_path = g_strdup_printf("%s/temp1_input", cpu);
    g_free(cpu);

    static const char *const disk_sensors[] = { "nvme", "drivetemp", NULL };
    char *disk = find_hwmon(disk_sensors);
    if (disk) {
        hw->disk_temp_path = g_strdup_printf("%s/temp1_input", disk);
        char path[512], name[32] = "";
        snprintf(path, sizeof path, "%s/name", disk);
        read_small(path, name, sizeof name);
        hw->info.disk_name = g_strdup(g_str_has_prefix(name, "nvme") ? "NVMe drive" : "Drive");
    }
    g_free(disk);

    find_gpu(hw);
    hw->now.cpu = hw->now.gpu = -1;
    hw->now.cpu_temp = hw->now.gpu_temp = hw->now.disk_temp = NAN;
    hw->now.gpu_watts = hw->now.gpu_fan_rpm = NAN;
    return hw;
}
