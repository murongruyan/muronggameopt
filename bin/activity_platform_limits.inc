#ifndef MURONG_ACTIVITY_PLATFORM_LIMITS_INC
#define MURONG_ACTIVITY_PLATFORM_LIMITS_INC

/*
 * activity_platform_limits.inc —— WALT / msm_performance 平台限制管理（6.13+）
 *
 * 背景：用户反馈 IPC 频率档位突然飙高、日常功耗偏高、msm_performance 的
 * cpu_max_freq 被平台反复改导致帧率不稳。节点语义已由 GitHub 一加开源 6.12 内核
 * 源码确认（OnePlusOSS/android_kernel_oneplus_sm8850 @ fc30e541），详见
 * _module_scan/kernel_notes.md §6/§7：ipc_freq_levels 是「IPC 档位→频率上限(kHz)」，
 * 必须非递减；high_perf_cluster_freq_cap 本版本因 plenty_giant_tasks 从未赋值而空转；
 * partial_halt 仅 CPU 全暂停窗口生效；msm cpu_max_freq 是该驱动自己的 FREQ_QOS_MAX.
 *
 * 设计原则（保守，不写坏系统）：
 *   1) 只做「读当前值 → 超过配置上限则写回上限、否则不动」，主动放开仅限
 *      msm cpu_max_freq 的 raise-only 且默认关闭；
 *   2) 节点不存在/不可写直接跳过；写入失败限流日志，不刷屏；
 *   3) 写入去重（值不变不写）+ 3s 节流，避免与平台/其它服务抢写；
 *   4) 全部为 static 符号，避免与其它 .inc 冲突。
 *
 * 接线方式（Lead 负责）：
 *   - 在 activity_diaodu.cpp 的 include 区（建议 activity_official_state.inc 之后、
 *     activity_main_loop.inc 之前）加：#include "activity_platform_limits.inc"
 *   - 初始化：platform_limits_init();（也可省略，tick 首调会自动 init）
 *   - 主循环：platform_limits_tick(get_current_time_ms(), is_game);
 *
 * 依赖（均在 include 点之前定义）：CONFIG_DIR、safe_snprintf、file_exists、
 * read_text_file、trim_copy、log_config_message(_throttled)、get_current_time_ms。
 */

#include <sys/stat.h>

#define PL_MAX_CLUSTERS 4
#define PL_IPC_LEVELS 5
#define PL_MAX_CPU_PAIRS 16

#define PL_CONF_PATH CONFIG_DIR "platform_limits.conf"
#define PL_IPC_LEVELS_PATH "/proc/sys/walt/cluster%d/smart_freq/ipc_freq_levels"
#define PL_HIGH_PERF_CAP_PATH "/proc/sys/walt/sched_high_perf_cluster_freq_cap"
#define PL_PARTIAL_HALT_PATH "/proc/sys/walt/sched_max_freq_partial_halt"
#define PL_MSM_CPU_MAX_FREQ_PATH "/sys/kernel/msm_performance/parameters/cpu_max_freq"

#define PL_INT_UNLIMITED 2147483647
#define PL_TICK_INTERVAL_MS 3000LL
#define PL_MSM_TICK_INTERVAL_MS 5000LL
#define PL_FAIL_LOG_INTERVAL_MS 30000
#define PL_CHANGE_LOG_INTERVAL_MS 60000

struct PlatformLimitsConfig {
    bool enabled;
    /*
     * 每簇 IPC 频率上限（kHz）：
     *   >0 = 固定上限；0 = 自动取当前数组第 3 档（index 2）；<0 = 不限制该簇
     */
    int ipc_limit_khz[PL_MAX_CLUSTERS];
    int daily_high_perf_cap;
    int daily_partial_halt;
    int game_high_perf_cap;
    int game_partial_halt;
    int msm_enable;                      /* 0=只记录基线；1=游戏中 raise-only（旧选项） */
    char msm_cpu_max_freq[256];          /* "cpu:khz cpu:khz ..." */
    /* 6.15：msm cpu_max_freq 固定上限（借鉴大肥鱼方案：日常固定 cap + chmod 0444 防平台改写）。 */
    char daily_msm_cpu_max_freq[256];    /* 空 = 不管理 */
    char game_msm_cpu_max_freq[256];     /* 空 = 游戏中沿用日常值 */
    int msm_readonly;                    /* 1=写完后 chmod 0444，阻止平台/性能服务改写 */
    int game_ipc_restore;                /* 1=游戏时恢复 IPC 原始档位，避免被日常 IPC 上限压住 */
    /* 6.16：按模式固定 msm cpu_max_freq / IPC 档位（system_tweaks 模式切换时调用）。
     * 空字符串 = 该模式不改；IPC 值规则："0"=自动取第 3 档，"-1"=恢复原始值，否则为 5 个 kHz 值。 */
    char mode_msm_powersave[256];
    char mode_msm_balance[256];
    char mode_msm_performance[256];
    char mode_msm_fast[256];
    char mode_ipc_powersave_c0[64];
    char mode_ipc_powersave_c1[64];
    char mode_ipc_balance_c0[64];
    char mode_ipc_balance_c1[64];
    char mode_ipc_performance_c0[64];
    char mode_ipc_performance_c1[64];
    char mode_ipc_fast_c0[64];
    char mode_ipc_fast_c1[64];
};

static PlatformLimitsConfig g_platform_limits = {
    true,
    {0, 0, 0, 0},
    PL_INT_UNLIMITED,
    PL_INT_UNLIMITED,
    PL_INT_UNLIMITED,
    PL_INT_UNLIMITED,
    0,
    "",
    "",
    "",
    1,
    1,
    "", "", "", "",
    "", "", "", "", "", "", "", ""
};
static bool g_platform_limits_inited = false;
static long long g_platform_limits_last_tick_ms = 0;
static long long g_platform_limits_last_msm_ms = 0;
static int g_platform_limits_last_game = -1;
static char g_platform_limits_msm_baseline[512] = "";
/* msm cpu_max_freq 接管状态：首次写入前保存原文本/权限，禁用或退出时恢复。 */
static char g_platform_limits_msm_original[512] = "";
static mode_t g_platform_limits_msm_original_mode = 0644;
static bool g_platform_limits_msm_original_valid = false;
static bool g_platform_limits_msm_applied = false;
/* 前置声明：热重载恢复逻辑在文件靠前位置调用，定义在后面的 msm 接管函数之后。 */
static void platform_limits_apply_msm_caps(bool is_game);
static void platform_limits_restore_all();
static void platform_limits_apply_mode_tweaks(const char* mode);
static void platform_limits_init();
/* 6.16 模式表状态（定义放文件靠前，恢复/重载逻辑要用）。 */
static char g_platform_limits_active_mode[32] = "";
static bool g_platform_limits_mode_override = false;
/* 配置热重载：每 5s 检查 mtime/size，改为「改配置立即生效」，无需重启守护进程。 */
static long long g_platform_limits_conf_last_mtime_ms = -1;
static long long g_platform_limits_conf_last_size = -1;
static long long g_platform_limits_conf_last_check_ms = 0;
/* IPC 原始值（首次写入前保存），enabled=0/删配置时恢复。 */
static int g_platform_limits_ipc_original[PL_MAX_CLUSTERS][PL_IPC_LEVELS];
static bool g_platform_limits_ipc_original_valid[PL_MAX_CLUSTERS] = {false, false, false, false};

static void platform_limits_reset_config() {
    g_platform_limits.enabled = true;
    for (int i = 0; i < PL_MAX_CLUSTERS; i++) {
        g_platform_limits.ipc_limit_khz[i] = 0;
    }
    g_platform_limits.daily_high_perf_cap = PL_INT_UNLIMITED;
    g_platform_limits.daily_partial_halt = PL_INT_UNLIMITED;
    g_platform_limits.game_high_perf_cap = PL_INT_UNLIMITED;
    g_platform_limits.game_partial_halt = PL_INT_UNLIMITED;
    g_platform_limits.msm_enable = 0;
    g_platform_limits.msm_cpu_max_freq[0] = '\0';
    g_platform_limits.daily_msm_cpu_max_freq[0] = '\0';
    g_platform_limits.game_msm_cpu_max_freq[0] = '\0';
    g_platform_limits.msm_readonly = 1;
    g_platform_limits.game_ipc_restore = 1;
    safe_strncpy(g_platform_limits.mode_msm_powersave,
                 "0:2227200 1:2227200 2:2227200 3:2227200 4:2227200 5:2227200 6:2438400 7:2438400",
                 sizeof(g_platform_limits.mode_msm_powersave));
    safe_strncpy(g_platform_limits.mode_msm_balance,
                 "0:2745600 1:2745600 2:2745600 3:2745600 4:2745600 5:2745600 6:2880000 7:2880000",
                 sizeof(g_platform_limits.mode_msm_balance));
    safe_strncpy(g_platform_limits.mode_msm_performance,
                 "0:2899200 1:2899200 2:2899200 3:2899200 4:2899200 5:2899200 6:3398400 7:3398400",
                 sizeof(g_platform_limits.mode_msm_performance));
    safe_strncpy(g_platform_limits.mode_msm_fast,
                 "0:3628800 1:3628800 2:3628800 3:3628800 4:3628800 5:3628800 6:4608000 7:4608000",
                 sizeof(g_platform_limits.mode_msm_fast));
    safe_strncpy(g_platform_limits.mode_ipc_powersave_c0,
                 "1363200 1363200 1363200 1363200 1363200",
                 sizeof(g_platform_limits.mode_ipc_powersave_c0));
    safe_strncpy(g_platform_limits.mode_ipc_powersave_c1,
                 "1497600 1497600 1497600 1497600 1497600",
                 sizeof(g_platform_limits.mode_ipc_powersave_c1));
    safe_strncpy(g_platform_limits.mode_ipc_balance_c0, "0", sizeof(g_platform_limits.mode_ipc_balance_c0));
    safe_strncpy(g_platform_limits.mode_ipc_balance_c1, "0", sizeof(g_platform_limits.mode_ipc_balance_c1));
    safe_strncpy(g_platform_limits.mode_ipc_performance_c0, "-1", sizeof(g_platform_limits.mode_ipc_performance_c0));
    safe_strncpy(g_platform_limits.mode_ipc_performance_c1, "-1", sizeof(g_platform_limits.mode_ipc_performance_c1));
    safe_strncpy(g_platform_limits.mode_ipc_fast_c0, "-1", sizeof(g_platform_limits.mode_ipc_fast_c0));
    safe_strncpy(g_platform_limits.mode_ipc_fast_c1, "-1", sizeof(g_platform_limits.mode_ipc_fast_c1));
}

static int platform_limits_parse_int(const std::string& value, int fallback) {
    if (value.empty()) return fallback;
    const char* text = value.c_str();
    char* end = NULL;
    long parsed = strtol(text, &end, 10);
    if (!end || end == text) return fallback;
    if (parsed > 2147483647L) return PL_INT_UNLIMITED;
    if (parsed < -2147483648L) return -2147483647;
    return (int)parsed;
}

static void platform_limits_apply_kv(const std::string& key, const std::string& value) {
    if (key == "enabled") {
        g_platform_limits.enabled = platform_limits_parse_int(value, 1) != 0;
        return;
    }
    for (int i = 0; i < PL_MAX_CLUSTERS; i++) {
        char cluster_key[32];
        safe_snprintf(cluster_key, sizeof(cluster_key), "ipc_limit_cluster%d", i);
        if (key == cluster_key) {
            g_platform_limits.ipc_limit_khz[i] = platform_limits_parse_int(value, 0);
            return;
        }
    }
    if (key == "daily_sched_high_perf_cluster_freq_cap") {
        g_platform_limits.daily_high_perf_cap = platform_limits_parse_int(value, PL_INT_UNLIMITED);
        return;
    }
    if (key == "daily_sched_max_freq_partial_halt") {
        g_platform_limits.daily_partial_halt = platform_limits_parse_int(value, PL_INT_UNLIMITED);
        return;
    }
    if (key == "game_sched_high_perf_cluster_freq_cap") {
        g_platform_limits.game_high_perf_cap = platform_limits_parse_int(value, PL_INT_UNLIMITED);
        return;
    }
    if (key == "game_sched_max_freq_partial_halt") {
        g_platform_limits.game_partial_halt = platform_limits_parse_int(value, PL_INT_UNLIMITED);
        return;
    }
    if (key == "msm_cpu_max_freq_enable") {
        g_platform_limits.msm_enable = platform_limits_parse_int(value, 0) != 0 ? 1 : 0;
        return;
    }
    if (key == "msm_cpu_max_freq") {
        safe_strncpy(g_platform_limits.msm_cpu_max_freq, value.c_str(),
                     sizeof(g_platform_limits.msm_cpu_max_freq));
        return;
    }
    if (key == "daily_msm_cpu_max_freq") {
        safe_strncpy(g_platform_limits.daily_msm_cpu_max_freq, value.c_str(),
                     sizeof(g_platform_limits.daily_msm_cpu_max_freq));
        return;
    }
    if (key == "game_msm_cpu_max_freq") {
        safe_strncpy(g_platform_limits.game_msm_cpu_max_freq, value.c_str(),
                     sizeof(g_platform_limits.game_msm_cpu_max_freq));
        return;
    }
    if (key == "msm_readonly") {
        g_platform_limits.msm_readonly = platform_limits_parse_int(value, 1) != 0 ? 1 : 0;
        return;
    }
    if (key == "game_ipc_restore") {
        g_platform_limits.game_ipc_restore = platform_limits_parse_int(value, 1) != 0 ? 1 : 0;
        return;
    }
    if (key == "mode_msm_powersave") { safe_strncpy(g_platform_limits.mode_msm_powersave, value.c_str(), sizeof(g_platform_limits.mode_msm_powersave)); return; }
    if (key == "mode_msm_balance") { safe_strncpy(g_platform_limits.mode_msm_balance, value.c_str(), sizeof(g_platform_limits.mode_msm_balance)); return; }
    if (key == "mode_msm_performance") { safe_strncpy(g_platform_limits.mode_msm_performance, value.c_str(), sizeof(g_platform_limits.mode_msm_performance)); return; }
    if (key == "mode_msm_fast") { safe_strncpy(g_platform_limits.mode_msm_fast, value.c_str(), sizeof(g_platform_limits.mode_msm_fast)); return; }
    if (key == "mode_ipc_powersave_c0") { safe_strncpy(g_platform_limits.mode_ipc_powersave_c0, value.c_str(), sizeof(g_platform_limits.mode_ipc_powersave_c0)); return; }
    if (key == "mode_ipc_powersave_c1") { safe_strncpy(g_platform_limits.mode_ipc_powersave_c1, value.c_str(), sizeof(g_platform_limits.mode_ipc_powersave_c1)); return; }
    if (key == "mode_ipc_balance_c0") { safe_strncpy(g_platform_limits.mode_ipc_balance_c0, value.c_str(), sizeof(g_platform_limits.mode_ipc_balance_c0)); return; }
    if (key == "mode_ipc_balance_c1") { safe_strncpy(g_platform_limits.mode_ipc_balance_c1, value.c_str(), sizeof(g_platform_limits.mode_ipc_balance_c1)); return; }
    if (key == "mode_ipc_performance_c0") { safe_strncpy(g_platform_limits.mode_ipc_performance_c0, value.c_str(), sizeof(g_platform_limits.mode_ipc_performance_c0)); return; }
    if (key == "mode_ipc_performance_c1") { safe_strncpy(g_platform_limits.mode_ipc_performance_c1, value.c_str(), sizeof(g_platform_limits.mode_ipc_performance_c1)); return; }
    if (key == "mode_ipc_fast_c0") { safe_strncpy(g_platform_limits.mode_ipc_fast_c0, value.c_str(), sizeof(g_platform_limits.mode_ipc_fast_c0)); return; }
    if (key == "mode_ipc_fast_c1") { safe_strncpy(g_platform_limits.mode_ipc_fast_c1, value.c_str(), sizeof(g_platform_limits.mode_ipc_fast_c1)); return; }
}

static void platform_limits_load_config() {
    platform_limits_reset_config();
    FILE* fp = fopen(PL_CONF_PATH, "r");
    if (!fp) {
        log_config_message("platform_limits: 未找到 config/platform_limits.conf，使用内置默认值");
        return;
    }
    char line[512];
    int parsed = 0;
    while (fgets(line, sizeof(line), fp)) {
        std::string text = trim_copy(std::string(line));
        if (text.empty() || text[0] == '#') continue;
        size_t eq = text.find('=');
        if (eq == std::string::npos) continue;
        std::string key = trim_copy(text.substr(0, eq));
        std::string value = trim_copy(text.substr(eq + 1));
        if (key.empty()) continue;
        platform_limits_apply_kv(key, value);
        parsed++;
    }
    fclose(fp);
    char msg[160];
    safe_snprintf(msg, sizeof(msg), "platform_limits: 已读取配置 %s（%d 项）", PL_CONF_PATH, parsed);
    log_config_message(msg);
}

static bool platform_limits_write_text(const char* path, const char* text) {
    if (!path || !path[0] || !text) return false;
    FILE* fp = fopen(path, "w");
    if (!fp) return false;
    int written = fprintf(fp, "%s", text);
    int flushed = fflush(fp);
    int closed = fclose(fp);
    return written > 0 && flushed == 0 && closed == 0;
}

/* 读取 5 个 IPC 档位（kHz）。返回是否成功。 */
static bool platform_limits_read_ipc_levels(int cluster, int values[PL_IPC_LEVELS]) {
    if (cluster < 0 || cluster >= PL_MAX_CLUSTERS || !values) return false;
    char path[192];
    safe_snprintf(path, sizeof(path), PL_IPC_LEVELS_PATH, cluster);
    if (!file_exists(path)) return false;
    char text[256];
    if (!read_text_file(path, text, sizeof(text))) return false;
    int v0 = 0, v1 = 0, v2 = 0, v3 = 0, v4 = 0;
    if (sscanf(text, "%d %d %d %d %d", &v0, &v1, &v2, &v3, &v4) != PL_IPC_LEVELS) return false;
    values[0] = v0;
    values[1] = v1;
    values[2] = v2;
    values[3] = v3;
    values[4] = v4;
    return true;
}

/*
 * IPC 档位限制：每个元素不超过上限，有变化才写回。
 * 语义（源码已确认）：ipc_freq_levels[i] = IPC 档位 i 的频率上限(kHz)，内核校验数组
 * 非递减；这里只做上限钳制（min），保持非递减，不做频点吸附（真机需确认写值被接受）。
 */
static void platform_limits_apply_ipc_limits() {
    for (int cluster = 0; cluster < PL_MAX_CLUSTERS; cluster++) {
        int limit = g_platform_limits.ipc_limit_khz[cluster];
        if (limit < 0) continue;                       /* <0 = 该簇不限制 */
        char path[192];
        safe_snprintf(path, sizeof(path), PL_IPC_LEVELS_PATH, cluster);
        if (!file_exists(path)) continue;              /* 节点不存在：跳过 */
        int values[PL_IPC_LEVELS];
        if (!platform_limits_read_ipc_levels(cluster, values)) {
            char key[64];
            safe_snprintf(key, sizeof(key), "platform_limits_ipc_read_c%d", cluster);
            char msg[160];
            safe_snprintf(msg, sizeof(msg),
                          "platform_limits: 读取 cluster%d ipc_freq_levels 失败/格式未知，跳过", cluster);
            log_config_message_throttled(key, PL_FAIL_LOG_INTERVAL_MS, msg);
            continue;
        }
        if (limit == 0) {
            /* 自动：取当前数组第 3 档（index 2）作为保守上限，并固定下来 */
            limit = values[PL_IPC_LEVELS / 2];
            g_platform_limits.ipc_limit_khz[cluster] = limit;
            char msg[192];
            safe_snprintf(msg, sizeof(msg),
                          "platform_limits: cluster%d IPC 上限自动取第 3 档 = %d kHz", cluster, limit);
            log_config_message(msg);
        }
        if (limit <= 0) continue;
        if (!g_platform_limits_ipc_original_valid[cluster]) {
            for (int i = 0; i < PL_IPC_LEVELS; i++) {
                g_platform_limits_ipc_original[cluster][i] = values[i];
            }
            g_platform_limits_ipc_original_valid[cluster] = true;
        }
        bool changed = false;
        int clamped[PL_IPC_LEVELS];
        for (int i = 0; i < PL_IPC_LEVELS; i++) {
            clamped[i] = values[i] > limit ? limit : values[i];
            if (clamped[i] != values[i]) changed = true;
        }
        if (!changed) continue;
        char text[128];
        safe_snprintf(text, sizeof(text), "%d %d %d %d %d\n",
                      clamped[0], clamped[1], clamped[2], clamped[3], clamped[4]);
        char key[64];
        safe_snprintf(key, sizeof(key), "platform_limits_ipc_write_c%d", cluster);
        if (platform_limits_write_text(path, text)) {
            char msg[224];
            safe_snprintf(msg, sizeof(msg),
                          "platform_limits: cluster%d ipc_freq_levels 已钳制上限 %d kHz（%d %d %d %d %d）",
                          cluster, limit, clamped[0], clamped[1], clamped[2], clamped[3], clamped[4]);
            log_config_message_throttled(key, PL_CHANGE_LOG_INTERVAL_MS, msg);
        } else {
            char msg[192];
            safe_snprintf(msg, sizeof(msg),
                          "platform_limits: cluster%d ipc_freq_levels 写入失败（节点只读/权限？）", cluster);
            log_config_message_throttled(key, PL_FAIL_LOG_INTERVAL_MS, msg);
        }
    }
}

/* 恢复 IPC 原始值（enabled=0 / 删除配置时调用，只恢复仍由我们钳制过的簇）。 */
static void platform_limits_restore_ipc_originals() {
    for (int cluster = 0; cluster < PL_MAX_CLUSTERS; cluster++) {
        if (!g_platform_limits_ipc_original_valid[cluster]) continue;
        char path[192];
        safe_snprintf(path, sizeof(path), PL_IPC_LEVELS_PATH, cluster);
        int values[PL_IPC_LEVELS];
        if (platform_limits_read_ipc_levels(cluster, values)) {
            bool same = true;
            for (int i = 0; i < PL_IPC_LEVELS; i++) {
                if (values[i] != g_platform_limits_ipc_original[cluster][i]) { same = false; break; }
            }
            if (!same) {
                char text[128];
                safe_snprintf(text, sizeof(text), "%d %d %d %d %d\n",
                              g_platform_limits_ipc_original[cluster][0], g_platform_limits_ipc_original[cluster][1],
                              g_platform_limits_ipc_original[cluster][2], g_platform_limits_ipc_original[cluster][3],
                              g_platform_limits_ipc_original[cluster][4]);
                platform_limits_write_text(path, text);
            }
        }
        g_platform_limits_ipc_original_valid[cluster] = false;
    }
}

/* 配置热重载：每 5s 检查一次配置文件的 mtime/size。 */
static void platform_limits_maybe_reload_config(long long now_ms) {
    if (now_ms - g_platform_limits_conf_last_check_ms < 5000) return;
    g_platform_limits_conf_last_check_ms = now_ms;
    struct stat st;
    if (stat(PL_CONF_PATH, &st) != 0) {
        if (g_platform_limits.enabled) {
            g_platform_limits.enabled = false;
            platform_limits_restore_all();
            log_config_message("platform_limits: 配置文件已删除，停止限制并恢复 IPC/msm 原值");
        }
        return;
    }
    long long mtime_ms = (long long)st.st_mtime * 1000LL;
    if (mtime_ms == g_platform_limits_conf_last_mtime_ms &&
        (long long)st.st_size == g_platform_limits_conf_last_size) {
        return;
    }
    g_platform_limits_conf_last_mtime_ms = mtime_ms;
    g_platform_limits_conf_last_size = (long long)st.st_size;
    platform_limits_load_config();
    if (!g_platform_limits.enabled) {
        platform_limits_restore_all();
        log_config_message("platform_limits: 已禁用，IPC/msm 限制已恢复初始值");
        return;
    }
    for (int cluster = 0; cluster < PL_MAX_CLUSTERS; cluster++) {
        if (g_platform_limits.ipc_limit_khz[cluster] != 0) continue;
        int values[PL_IPC_LEVELS];
        if (platform_limits_read_ipc_levels(cluster, values) && values[PL_IPC_LEVELS / 2] > 0) {
            g_platform_limits.ipc_limit_khz[cluster] = values[PL_IPC_LEVELS / 2];
        }
    }
    g_platform_limits_last_game = -1; /* 让场景 caps 重新应用 */
    if (g_platform_limits_mode_override && g_platform_limits_active_mode[0]) {
        char mode_copy[32];
        safe_strncpy(mode_copy, g_platform_limits_active_mode, sizeof(mode_copy));
        g_platform_limits_active_mode[0] = '\0';
        platform_limits_apply_mode_tweaks(mode_copy);
    }
    log_config_message("platform_limits: 配置已热重载");
}

/* 读取一个节点里的前 max_count 个整数；返回实际个数（-1=读不到）。 */
static int platform_limits_read_int_tokens(const char* path, int* out, int max_count) {
    if (!path || !out || max_count <= 0) return -1;
    char text[256];
    if (!read_text_file(path, text, sizeof(text))) return -1;
    int count = 0;
    const char* cursor = text;
    while (*cursor && count < max_count) {
        while (*cursor && !isdigit((unsigned char)*cursor) && *cursor != '-') cursor++;
        if (!*cursor) break;
        char* end = NULL;
        long value = strtol(cursor, &end, 10);
        if (!end || end == cursor) break;
        out[count++] = (int)value;
        cursor = end;
    }
    return count;
}

/*
 * /proc/sys/walt/sched_high_perf_cluster_freq_cap：真实格式为 2 个整数
 * （本机 2147483647 2147483647）。按实际元素个数写入同一个上限值；
 * 值未变化时不写，避免无谓触发。
 */
static void platform_limits_apply_high_perf_cap(int cap) {
    if (!file_exists(PL_HIGH_PERF_CAP_PATH)) return;
    int current[8];
    int count = platform_limits_read_int_tokens(PL_HIGH_PERF_CAP_PATH, current, 8);
    if (count <= 0) {
        log_config_message_throttled("platform_limits_hp_read", PL_FAIL_LOG_INTERVAL_MS,
                                     "platform_limits: 读取 sched_high_perf_cluster_freq_cap 失败，跳过");
        return;
    }
    bool changed = false;
    for (int i = 0; i < count; i++) {
        if (current[i] != cap) {
            changed = true;
            break;
        }
    }
    if (!changed) return;
    char text[160];
    size_t used = 0;
    text[0] = '\0';
    for (int i = 0; i < count; i++) {
        int written = safe_snprintf(text + used, sizeof(text) - used, i == 0 ? "%d" : " %d", cap);
        if (written <= 0 || (size_t)written >= sizeof(text) - used) break;
        used += (size_t)written;
    }
    safe_snprintf(text + used, sizeof(text) - used, "\n");
    if (platform_limits_write_text(PL_HIGH_PERF_CAP_PATH, text)) {
        char msg[224];
        safe_snprintf(msg, sizeof(msg),
                      "platform_limits: sched_high_perf_cluster_freq_cap 写为 %d（%d 项）", cap, count);
        log_config_message_throttled("platform_limits_hp_write", PL_CHANGE_LOG_INTERVAL_MS, msg);
    } else {
        log_config_message_throttled("platform_limits_hp_fail", PL_FAIL_LOG_INTERVAL_MS,
                                     "platform_limits: 写入 sched_high_perf_cluster_freq_cap 失败");
    }
}

/* /proc/sys/walt/sched_max_freq_partial_halt：真实格式为 1 个整数。 */
static void platform_limits_apply_partial_halt(int cap) {
    if (!file_exists(PL_PARTIAL_HALT_PATH)) return;
    int current[4];
    int count = platform_limits_read_int_tokens(PL_PARTIAL_HALT_PATH, current, 4);
    if (count <= 0) {
        log_config_message_throttled("platform_limits_halt_read", PL_FAIL_LOG_INTERVAL_MS,
                                     "platform_limits: 读取 sched_max_freq_partial_halt 失败，跳过");
        return;
    }
    if (current[0] == cap) return;
    char text[64];
    safe_snprintf(text, sizeof(text), "%d\n", cap);
    if (platform_limits_write_text(PL_PARTIAL_HALT_PATH, text)) {
        char msg[192];
        safe_snprintf(msg, sizeof(msg),
                      "platform_limits: sched_max_freq_partial_halt %d -> %d", current[0], cap);
        log_config_message_throttled("platform_limits_halt_write", PL_CHANGE_LOG_INTERVAL_MS, msg);
    } else {
        log_config_message_throttled("platform_limits_halt_fail", PL_FAIL_LOG_INTERVAL_MS,
                                     "platform_limits: 写入 sched_max_freq_partial_halt 失败");
    }
}

static void platform_limits_apply_msm_caps(bool is_game);
static void platform_limits_restore_all();

/* 游戏状态切换时应用日常/游戏两套 cap（只在切换时写，避免每 tick 抢写）。 */
static void platform_limits_apply_caps(bool is_game) {
    int high_perf_cap = is_game ? g_platform_limits.game_high_perf_cap
                                : g_platform_limits.daily_high_perf_cap;
    int partial_halt = is_game ? g_platform_limits.game_partial_halt
                               : g_platform_limits.daily_partial_halt;
    platform_limits_apply_high_perf_cap(high_perf_cap);
    platform_limits_apply_partial_halt(partial_halt);
    platform_limits_apply_msm_caps(is_game);
    /* 游戏时恢复 IPC 原始档位，避免日常 IPC 上限把游戏频率压住（借鉴大肥鱼方案）。 */
    if (is_game && g_platform_limits.game_ipc_restore) {
        platform_limits_restore_ipc_originals();
    }
}

/* 解析 "cpu:khz cpu:khz ..." 文本到数组；返回 pair 数。 */
static int platform_limits_parse_cpu_pairs(const char* text, int* cpus, int* freqs, int max_pairs) {
    if (!text || !cpus || !freqs || max_pairs <= 0) return 0;
    int count = 0;
    const char* cursor = text;
    while (*cursor && count < max_pairs) {
        int cpu = -1;
        int freq = -1;
        if (sscanf(cursor, " %d:%d", &cpu, &freq) != 2) break;
        if (cpu < 0 || freq <= 0) break;
        cpus[count] = cpu;
        freqs[count] = freq;
        count++;
        const char* space = strchr(cursor, ' ');
        if (!space) break;
        cursor = space + 1;
    }
    return count;
}

/* 记录 msm_performance cpu_max_freq 基线（只读，不写）。 */
static void platform_limits_record_msm_baseline() {
    if (!file_exists(PL_MSM_CPU_MAX_FREQ_PATH)) {
        log_config_message("platform_limits: 无 /sys/kernel/msm_performance，跳过 msm 限制");
        return;
    }
    char text[512];
    if (!read_text_file(PL_MSM_CPU_MAX_FREQ_PATH, text, sizeof(text))) {
        log_config_message_throttled("platform_limits_msm_read", PL_FAIL_LOG_INTERVAL_MS,
                                     "platform_limits: 读取 msm cpu_max_freq 基线失败");
        return;
    }
    std::string trimmed = trim_copy(std::string(text));
    safe_strncpy(g_platform_limits_msm_baseline, trimmed.c_str(),
                 sizeof(g_platform_limits_msm_baseline));
    char msg[640];
    safe_snprintf(msg, sizeof(msg), "platform_limits: msm cpu_max_freq 基线 = %s",
                  g_platform_limits_msm_baseline);
    log_config_message(msg);
}

/*
 * msm cpu_max_freq 稳定化（默认关闭）：
 * 源码已确认该节点是本驱动自己的 FREQ_QOS_MAX 请求，最终频率上限 = 所有 QoS 请求的
 * 最小值 → 向上写只能放松本驱动约束（不能突破 thermal/其它请求），向下写会真实压低。
 * 因此只在游戏中、且配置 enable=1 时做 raise-only：把低于配置值的 CPU 上限抬到配置值，
 * 绝不压低；节点可能被平台重置，5s 节流重试。
 */
static void platform_limits_msm_raise_only() {
    if (!g_platform_limits.msm_enable || !g_platform_limits.msm_cpu_max_freq[0]) return;
    if (!file_exists(PL_MSM_CPU_MAX_FREQ_PATH)) return;
    char current_text[512];
    if (!read_text_file(PL_MSM_CPU_MAX_FREQ_PATH, current_text, sizeof(current_text))) {
        log_config_message_throttled("platform_limits_msm_read_tick", PL_FAIL_LOG_INTERVAL_MS,
                                     "platform_limits: 读取 msm cpu_max_freq 失败，跳过");
        return;
    }
    int current_cpu[PL_MAX_CPU_PAIRS];
    int current_freq[PL_MAX_CPU_PAIRS];
    int current_count = platform_limits_parse_cpu_pairs(current_text, current_cpu, current_freq,
                                                         PL_MAX_CPU_PAIRS);
    if (current_count <= 0) return;
    int cfg_cpu[PL_MAX_CPU_PAIRS];
    int cfg_freq[PL_MAX_CPU_PAIRS];
    int cfg_count = platform_limits_parse_cpu_pairs(g_platform_limits.msm_cpu_max_freq,
                                                     cfg_cpu, cfg_freq, PL_MAX_CPU_PAIRS);
    if (cfg_count <= 0) return;

    int out_cpu[PL_MAX_CPU_PAIRS];
    int out_freq[PL_MAX_CPU_PAIRS];
    int out_count = 0;
    bool changed = false;
    for (int i = 0; i < current_count && out_count < PL_MAX_CPU_PAIRS; i++) {
        out_cpu[out_count] = current_cpu[i];
        out_freq[out_count] = current_freq[i];
        for (int j = 0; j < cfg_count; j++) {
            if (cfg_cpu[j] == current_cpu[i] && cfg_freq[j] > current_freq[i]) {
                out_freq[out_count] = cfg_freq[j];
                changed = true;
                break;
            }
        }
        out_count++;
    }
    if (!changed) return;

    char text[512];
    size_t used = 0;
    text[0] = '\0';
    for (int i = 0; i < out_count; i++) {
        int written = safe_snprintf(text + used, sizeof(text) - used, i == 0 ? "%d:%d" : " %d:%d",
                                    out_cpu[i], out_freq[i]);
        if (written <= 0 || (size_t)written >= sizeof(text) - used) break;
        used += (size_t)written;
    }
    safe_snprintf(text + used, sizeof(text) - used, "\n");
    if (platform_limits_write_text(PL_MSM_CPU_MAX_FREQ_PATH, text)) {
        char msg[420];
        safe_snprintf(msg, sizeof(msg),
                      "platform_limits: msm cpu_max_freq raise-only 写回 = %s", text);
        log_config_message_throttled("platform_limits_msm_write", PL_CHANGE_LOG_INTERVAL_MS, msg);
    } else {
        log_config_message_throttled("platform_limits_msm_fail", PL_FAIL_LOG_INTERVAL_MS,
                                     "platform_limits: 写入 msm cpu_max_freq 失败");
    }
}

/* 设置 msm cpu_max_freq 节点权限（0444 用于阻止平台/性能服务改写，0444 前需 0666 才可写）。 */
static void platform_limits_msm_chmod(mode_t mode) {
    if (chmod(PL_MSM_CPU_MAX_FREQ_PATH, mode) != 0) {
        log_config_message_throttled("platform_limits_msm_chmod", PL_FAIL_LOG_INTERVAL_MS,
                                     "platform_limits: chmod msm cpu_max_freq 失败");
    }
}

/* 首次接管前保存原文本与权限。 */
static void platform_limits_msm_capture_original() {
    if (g_platform_limits_msm_original_valid) return;
    struct stat st;
    if (stat(PL_MSM_CPU_MAX_FREQ_PATH, &st) == 0) {
        g_platform_limits_msm_original_mode = st.st_mode & 0777;
    }
    char text[512];
    if (read_text_file(PL_MSM_CPU_MAX_FREQ_PATH, text, sizeof(text))) {
        std::string trimmed = trim_copy(std::string(text));
        safe_strncpy(g_platform_limits_msm_original, trimmed.c_str(),
                     sizeof(g_platform_limits_msm_original));
        g_platform_limits_msm_original_valid = true;
    }
}

/* 写 msm cpu_max_freq（自动记录原值、按配置决定是否 chmod 0444）。 */
static bool platform_limits_msm_write_raw(const char* text_in) {
    if (!text_in || !text_in[0]) return false;
    if (!file_exists(PL_MSM_CPU_MAX_FREQ_PATH)) return false;
    platform_limits_msm_capture_original();
    platform_limits_msm_chmod(0666);
    char text[512];
    safe_snprintf(text, sizeof(text), "%s\n", text_in);
    bool ok = platform_limits_write_text(PL_MSM_CPU_MAX_FREQ_PATH, text);
    if (ok && g_platform_limits.msm_readonly) {
        platform_limits_msm_chmod(0444);
    }
    if (ok) g_platform_limits_msm_applied = true;
    return ok;
}

/* 恢复 msm cpu_max_freq 原值与原权限。 */
static void platform_limits_restore_msm() {
    if (!g_platform_limits_msm_applied) return;
    platform_limits_msm_chmod(0666);
    if (g_platform_limits_msm_original_valid && g_platform_limits_msm_original[0]) {
        char text[512];
        safe_snprintf(text, sizeof(text), "%s\n", g_platform_limits_msm_original);
        platform_limits_write_text(PL_MSM_CPU_MAX_FREQ_PATH, text);
    }
    platform_limits_msm_chmod(g_platform_limits_msm_original_mode);
    g_platform_limits_msm_applied = false;
    g_platform_limits_msm_original_valid = false;
    log_config_message("platform_limits: 已恢复 msm cpu_max_freq 原值与权限");
}

/* 按当前场景应用 msm cpu_max_freq 固定上限（空=不管理/沿用日常）。 */
static void platform_limits_apply_msm_caps(bool is_game) {
    const char* target = is_game ? g_platform_limits.game_msm_cpu_max_freq
                                 : g_platform_limits.daily_msm_cpu_max_freq;
    if (is_game && (!target || !target[0])) {
        target = g_platform_limits.daily_msm_cpu_max_freq;
    }
    if (!target || !target[0]) {
        platform_limits_restore_msm();
        return;
    }
    char current_text[512];
    if (read_text_file(PL_MSM_CPU_MAX_FREQ_PATH, current_text, sizeof(current_text))) {
        std::string current = trim_copy(std::string(current_text));
        std::string wanted = trim_copy(std::string(target));
        if (current == wanted) {
            if (g_platform_limits.msm_readonly) platform_limits_msm_chmod(0444);
            g_platform_limits_msm_applied = true;
            return;
        }
    }
    if (platform_limits_msm_write_raw(target)) {
        char msg[420];
        safe_snprintf(msg, sizeof(msg), "platform_limits: msm cpu_max_freq 固定为 %s（%s）",
                      target, is_game ? "游戏" : "日常");
        log_config_message_throttled("platform_limits_msm_cap", PL_CHANGE_LOG_INTERVAL_MS, msg);
    } else {
        log_config_message_throttled("platform_limits_msm_cap_fail", PL_FAIL_LOG_INTERVAL_MS,
                                     "platform_limits: 写入 msm cpu_max_freq 失败");
    }
}

/* 禁用/退出：恢复 IPC 原始档位、msm 原值，并把两个 WALT cap 恢复默认不限制。 */
static void platform_limits_restore_all() {
    platform_limits_restore_ipc_originals();
    platform_limits_restore_msm();
    platform_limits_apply_high_perf_cap(PL_INT_UNLIMITED);
    platform_limits_apply_partial_halt(PL_INT_UNLIMITED);
    g_platform_limits_mode_override = false;
    g_platform_limits_active_mode[0] = '\0';
}

static void platform_limits_atexit_restore() {
    platform_limits_restore_all();
}

/* ===== 6.16 模式表：msm cpu_max_freq / IPC 档位按模式固定 ===== */

static const char* platform_limits_mode_msm(const char* mode) {
    if (!mode) return "";
    if (strcmp(mode, "powersave") == 0) return g_platform_limits.mode_msm_powersave;
    if (strcmp(mode, "balance") == 0) return g_platform_limits.mode_msm_balance;
    if (strcmp(mode, "performance") == 0) return g_platform_limits.mode_msm_performance;
    if (strcmp(mode, "fast") == 0) return g_platform_limits.mode_msm_fast;
    return g_platform_limits.mode_msm_balance;
}

static const char* platform_limits_mode_ipc(const char* mode, int cluster) {
    if (!mode) return "";
    if (strcmp(mode, "powersave") == 0) {
        return cluster == 0 ? g_platform_limits.mode_ipc_powersave_c0 : g_platform_limits.mode_ipc_powersave_c1;
    }
    if (strcmp(mode, "balance") == 0) {
        return cluster == 0 ? g_platform_limits.mode_ipc_balance_c0 : g_platform_limits.mode_ipc_balance_c1;
    }
    if (strcmp(mode, "performance") == 0) {
        return cluster == 0 ? g_platform_limits.mode_ipc_performance_c0 : g_platform_limits.mode_ipc_performance_c1;
    }
    if (strcmp(mode, "fast") == 0) {
        return cluster == 0 ? g_platform_limits.mode_ipc_fast_c0 : g_platform_limits.mode_ipc_fast_c1;
    }
    return cluster == 0 ? g_platform_limits.mode_ipc_balance_c0 : g_platform_limits.mode_ipc_balance_c1;
}

static void platform_limits_write_ipc_array(int cluster, const int values[PL_IPC_LEVELS]) {
    char path[192];
    safe_snprintf(path, sizeof(path), PL_IPC_LEVELS_PATH, cluster);
    if (!file_exists(path)) return;
    char text[128];
    safe_snprintf(text, sizeof(text), "%d %d %d %d %d\n",
                  values[0], values[1], values[2], values[3], values[4]);
    platform_limits_write_text(path, text);
}

static void platform_limits_restore_ipc_cluster(int cluster) {
    if (cluster < 0 || cluster >= PL_MAX_CLUSTERS) return;
    if (!g_platform_limits_ipc_original_valid[cluster]) return;
    char path[192];
    safe_snprintf(path, sizeof(path), PL_IPC_LEVELS_PATH, cluster);
    int current[PL_IPC_LEVELS];
    if (platform_limits_read_ipc_levels(cluster, current)) {
        bool same = true;
        for (int i = 0; i < PL_IPC_LEVELS; i++) {
            if (current[i] != g_platform_limits_ipc_original[cluster][i]) { same = false; break; }
        }
        if (!same) platform_limits_write_ipc_array(cluster, g_platform_limits_ipc_original[cluster]);
    }
    g_platform_limits_ipc_original_valid[cluster] = false;
}

/*
 * 解析模式 IPC 配置：
 *   空 -> -2（不改）；"-1" -> -1（恢复原始值）；"0" -> 0（自动取第 3 档）；
 *   其它 -> 1（5 个 kHz 值，允许只写 1 个值表示全档同值）。
 */
static int platform_limits_parse_ipc_spec(const char* spec, int out[PL_IPC_LEVELS]) {
    if (!spec || !spec[0]) return -2;
    char* ptr = (char*)spec;
    char* end = NULL;
    long first = strtol(ptr, &end, 10);
    if (!end || end == ptr) return 0;
    ptr = end;
    while (*ptr == ' ' || *ptr == '\t' || *ptr == '\r' || *ptr == '\n') ptr++;
    if (*ptr == '\0') {
        if (first == -1) return -1;
        if (first == 0) return 0;
    }
    int count = 0;
    ptr = (char*)spec;
    while (*ptr && count < PL_IPC_LEVELS) {
        end = NULL;
        long v = strtol(ptr, &end, 10);
        if (!end || end == ptr) break;
        out[count++] = (int)v;
        ptr = end;
    }
    if (count <= 0) return 0;
    if (count == 1) {
        for (int i = 1; i < PL_IPC_LEVELS; i++) out[i] = out[0];
    } else if (count < PL_IPC_LEVELS) {
        for (int i = count; i < PL_IPC_LEVELS; i++) out[i] = out[count - 1];
    }
    return 1;
}

static void platform_limits_apply_mode_ipc(int cluster, const char* spec) {
    int values[PL_IPC_LEVELS];
    int kind = platform_limits_parse_ipc_spec(spec, values);
    if (kind == -2) return;
    if (kind == -1) {
        platform_limits_restore_ipc_cluster(cluster);
        return;
    }
    char path[192];
    safe_snprintf(path, sizeof(path), PL_IPC_LEVELS_PATH, cluster);
    if (!file_exists(path)) return;
    int current[PL_IPC_LEVELS];
    if (!platform_limits_read_ipc_levels(cluster, current)) return;
    if (!g_platform_limits_ipc_original_valid[cluster]) {
        for (int i = 0; i < PL_IPC_LEVELS; i++) g_platform_limits_ipc_original[cluster][i] = current[i];
        g_platform_limits_ipc_original_valid[cluster] = true;
    }
    int target[PL_IPC_LEVELS];
    if (kind == 0) {
        int limit = current[PL_IPC_LEVELS / 2];
        for (int i = 0; i < PL_IPC_LEVELS; i++) target[i] = current[i] > limit ? limit : current[i];
    } else {
        for (int i = 0; i < PL_IPC_LEVELS; i++) target[i] = values[i];
    }
    bool changed = false;
    for (int i = 0; i < PL_IPC_LEVELS; i++) {
        if (target[i] != current[i]) { changed = true; break; }
    }
    if (!changed) return;
    platform_limits_write_ipc_array(cluster, target);
    char msg[224];
    safe_snprintf(msg, sizeof(msg), "platform_limits: cluster%d IPC 档位按模式写为 %d %d %d %d %d",
                  cluster, target[0], target[1], target[2], target[3], target[4]);
    log_config_message_throttled("platform_limits_mode_ipc", PL_CHANGE_LOG_INTERVAL_MS, msg);
}

/* 按模式应用 msm / IPC（system_tweaks 检测到模式变化时调用）。 */
static void platform_limits_apply_mode_tweaks(const char* mode) {
    if (!g_platform_limits_inited) platform_limits_init();
    if (!g_platform_limits.enabled) return;
    if (!mode || !mode[0]) return;
    if (g_platform_limits_mode_override && strcmp(mode, g_platform_limits_active_mode) == 0) return;
    safe_strncpy(g_platform_limits_active_mode, mode, sizeof(g_platform_limits_active_mode));
    g_platform_limits_mode_override = true;

    const char* msm = platform_limits_mode_msm(mode);
    if (msm && msm[0]) {
        platform_limits_msm_write_raw(msm);
    } else {
        platform_limits_restore_msm();
    }
    platform_limits_restore_ipc_originals();
    platform_limits_apply_mode_ipc(0, platform_limits_mode_ipc(mode, 0));
    platform_limits_apply_mode_ipc(1, platform_limits_mode_ipc(mode, 1));

    char msg[420];
    safe_snprintf(msg, sizeof(msg),
                  "platform_limits: 模式 %s 已应用 | msm=%s | ipc_c0=%s | ipc_c1=%s",
                  mode, msm && msm[0] ? msm : "(不改)",
                  platform_limits_mode_ipc(mode, 0), platform_limits_mode_ipc(mode, 1));
    log_config_message(msg);
}

static void platform_limits_init() {
    if (g_platform_limits_inited) return;
    g_platform_limits_inited = true;
    platform_limits_load_config();
    {
        struct stat st;
        if (stat(PL_CONF_PATH, &st) == 0) {
            g_platform_limits_conf_last_mtime_ms = (long long)st.st_mtime * 1000LL;
            g_platform_limits_conf_last_size = (long long)st.st_size;
        }
    }

    /* 解析自动 IPC 上限：读取当前数组第 3 档并固定，方便日志展示与后续不漂移 */
    for (int cluster = 0; cluster < PL_MAX_CLUSTERS; cluster++) {
        if (g_platform_limits.ipc_limit_khz[cluster] != 0) continue;
        int values[PL_IPC_LEVELS];
        if (platform_limits_read_ipc_levels(cluster, values)) {
            g_platform_limits.ipc_limit_khz[cluster] = values[PL_IPC_LEVELS / 2];
        }
    }

    platform_limits_record_msm_baseline();
    platform_limits_apply_msm_caps(false);
    atexit(platform_limits_atexit_restore);

    char msg[448];
    safe_snprintf(msg, sizeof(msg),
                  "platform_limits: enabled=%d | ipc[c0=%d c1=%d c2=%d c3=%d] kHz | daily_hp=%d daily_halt=%d | game_hp=%d game_halt=%d | msm_enable=%d",
                  g_platform_limits.enabled ? 1 : 0,
                  g_platform_limits.ipc_limit_khz[0], g_platform_limits.ipc_limit_khz[1],
                  g_platform_limits.ipc_limit_khz[2], g_platform_limits.ipc_limit_khz[3],
                  g_platform_limits.daily_high_perf_cap, g_platform_limits.daily_partial_halt,
                  g_platform_limits.game_high_perf_cap, g_platform_limits.game_partial_halt,
                  g_platform_limits.msm_enable);
    if (g_platform_limits.daily_msm_cpu_max_freq[0]) {
        char msm_msg[420];
        safe_snprintf(msm_msg, sizeof(msm_msg),
                      "platform_limits: msm 日常上限 = %s | msm_readonly=%d | game_ipc_restore=%d",
                      g_platform_limits.daily_msm_cpu_max_freq,
                      g_platform_limits.msm_readonly, g_platform_limits.game_ipc_restore);
        log_config_message(msm_msg);
    }
    log_config_message(msg);
}

/*
 * 主循环调用（建议每 tick 都调，内部 3s 节流）：
 *   is_game = 当前前台是否为游戏（可直接用 is_game_package_name(g_current_foreground_app)）。
 */
static void platform_limits_tick(long long now_ms, bool is_game) {
    if (!g_platform_limits_inited) platform_limits_init();
    if (now_ms <= 0) now_ms = get_current_time_ms();
    platform_limits_maybe_reload_config(now_ms);
    if (!g_platform_limits.enabled) return;
    /* 模式表接管 msm/IPC 时，不再跑日常/游戏那套固定值。 */
    if (g_platform_limits_mode_override) return;

    int game = is_game ? 1 : 0;
    if (game != g_platform_limits_last_game) {
        g_platform_limits_last_game = game;
        platform_limits_apply_caps(is_game);
    }

    if (now_ms - g_platform_limits_last_tick_ms < PL_TICK_INTERVAL_MS) return;
    g_platform_limits_last_tick_ms = now_ms;

    /* 游戏中且开启 game_ipc_restore 时，不再把 IPC 档位钳到日常上限（游戏允许更高档位）。 */
    if (!(is_game && g_platform_limits.game_ipc_restore)) {
        platform_limits_apply_ipc_limits();
    }

    if (g_platform_limits.msm_enable && is_game &&
        now_ms - g_platform_limits_last_msm_ms >= PL_MSM_TICK_INTERVAL_MS) {
        g_platform_limits_last_msm_ms = now_ms;
        platform_limits_msm_raise_only();
    }
}

#endif /* MURONG_ACTIVITY_PLATFORM_LIMITS_INC */
