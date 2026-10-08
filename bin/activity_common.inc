#ifndef ACTIVITY_COMMON_INC
#define ACTIVITY_COMMON_INC

#include <sys/time.h>
#include <vector>
#include <string>
#include <cstring>
#include <ctype.h>
#include <stdio.h>

/*
 * 动态调频的开关（每簇一份）。具体策略：
 *   日用（无帧信号）→ dyn.json 的 daily 分段表（见 activity_dyn_config.inc）
 *   游戏（有帧信号）→ FAS 的帧债动作表
 */
typedef struct {
    bool enabled;
} DynamicTuningParams;

struct AllCpuStats {
    uint64_t idle[32];
    uint64_t total[32];
    int valid;
};

#include <shared_mutex>
static std::shared_mutex g_mutex;

static int read_all_cpu_stats(AllCpuStats* out) {
    if (!out) return 0;
    out->valid = 0;
    memset(out->idle, 0, sizeof(out->idle));
    memset(out->total, 0, sizeof(out->total));

    FILE* fp = fopen("/proc/stat", "r");
    if (!fp) return 0;

    char line[256];
    while (fgets(line, sizeof(line), fp)) {
        if (strncmp(line, "cpu", 3) == 0 && isdigit((unsigned char)line[3])) {
            int cpu_id = atoi(line + 3);
            if (cpu_id >= 0 && cpu_id < 32) {
                char* saveptr;
                char* token = strtok_r(line + 3, " \t\n", &saveptr);
                token = strtok_r(NULL, " \t\n", &saveptr);
                int field_idx = 0;
                unsigned long long user = 0, nice = 0, system = 0, idle = 0, iowait = 0, irq = 0, softirq = 0, steal = 0;

                while (token != NULL) {
                    unsigned long long val = strtoull(token, NULL, 10);
                    switch (field_idx) {
                        case 0: user = val; break;
                        case 1: nice = val; break;
                        case 2: system = val; break;
                        case 3: idle = val; break;
                        case 4: iowait = val; break;
                        case 5: irq = val; break;
                        case 6: softirq = val; break;
                        case 7: steal = val; break;
                    }
                    field_idx++;
                    if (field_idx >= 8) break;
                    token = strtok_r(NULL, " \t\n", &saveptr);
                }

                if (field_idx >= 4) {
                    out->idle[cpu_id] = idle + iowait;
                    out->total[cpu_id] = user + nice + system + idle + iowait + irq + softirq + steal;
                }
            }
        }
    }
    fclose(fp);
    out->valid = 1;
    return 1;
}

long long get_current_time_ms() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

#endif // ACTIVITY_COMMON_INC
