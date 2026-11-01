#ifndef MURONG_ACTIVITY_GAME_SCENARIO_INC
#define MURONG_ACTIVITY_GAME_SCENARIO_INC

/*
 * activity_game_scenario.inc —— 游戏场景参数（借鉴 cfm运行中.sh / cfm结束运行.sh）
 *
 * 参考脚本（大肥鱼 1+15 调度目录）实测参数：
 *   cfm运行中: performance governor; policy0 max 2745600/min 384000; policy6 max 2880000/min 768000;
 *              cpuset background/system-background = 0-6;
 *              walt sched_high_perf_cluster_freq_cap=1497600; sched_max_freq_partial_halt=1555200;
 *              ipc cluster0=1497600x5; cluster1=1555200x5;
 *   cfm结束运行: schedutil; max 2361600/2880000; min 384000/768000; cpuset 0-5; caps INT_MAX;
 *              ipc 2361600x5 / 2880000x5。
 *
 * 本实现只在「配置的游戏包名处于前台」时应用；退出时恢复进入前的原值（比脚本固定恢复值更安全）。
 * 应用期间置 g_game_scenario_active=true，platform_limits / 动态调频让出控制权，避免互相抢写。
 * 全部 static 符号；配置热重载 5s。
 */

#define GS_CONF_PATH CONFIG_DIR "game_scenario.conf"
#define GS_MAX_PACKAGES 16
#define GS_IPC_LEVELS 5
#define GS_MAX_POLICIES 8
#define GS_MAX_CORE_CTL 12

struct GameScenarioConfig {
    bool enabled;
    char game_packages[GS_MAX_PACKAGES][128];
    int game_package_count;
    char governor[32];
    int max_first;
    int min_first;
    int max_last;
    int min_last;
    char cpuset_game[64];
    int walt_high_perf_cap;
    int walt_partial_halt;
    int ipc_c0[GS_IPC_LEVELS];
    int ipc_c1[GS_IPC_LEVELS];
    int core_ctl_enable;
};

static GameScenarioConfig g_gs = {
    true, {{0}}, 0, "performance",
    2745600, 384000, 2880000, 768000,
    "0-6",
    1497600, 1555200,
    {1497600, 1497600, 1497600, 1497600, 1497600},
    {1555200, 1555200, 1555200, 1555200, 1555200},
    0
};

struct GameScenarioSaved {
    bool valid;
    int gov_count;
    int gov_policy[GS_MAX_POLICIES];
    char gov_value[GS_MAX_POLICIES][32];
    int freq_count;
    int freq_policy[GS_MAX_POLICIES];
    int orig_min[GS_MAX_POLICIES];
    int orig_max[GS_MAX_POLICIES];
    char orig_cpuset_bg[64];
    char orig_cpuset_sysbg[64];
    bool ipc_valid[2];
    int orig_ipc[2][GS_IPC_LEVELS];
    int hp_count;
    int orig_hp[4];
    int orig_partial_halt;
    bool partial_valid;
    int core_ctl_count;
    char core_ctl_path[GS_MAX_CORE_CTL][128];
    int core_ctl_value[GS_MAX_CORE_CTL];
};

static GameScenarioSaved g_gs_saved;
static bool g_gs_inited = false;
static bool g_gs_active = false;
static char g_gs_active_pkg[128] = "";
static long long g_gs_conf_mtime_ms = -1;
static long long g_gs_conf_size = -1;
static long long g_gs_last_check_ms = 0;
static long long g_gs_last_reapply_ms = 0;

static bool gs_read_text(const char* path, char* out, size_t out_size) {
    if (!path || !out || out_size == 0) return false;
    FILE* fp = fopen(path, "r");
    if (!fp) return false;
    size_t n = fread(out, 1, out_size - 1, fp);
    out[n] = '\0';
    fclose(fp);
    size_t len = strlen(out);
    while (len > 0 && (out[len - 1] == '\n' || out[len - 1] == '\r' || out[len - 1] == ' ')) out[--len] = '\0';
    return out[0] != '\0';
}

static bool gs_write_text(const char* path, const char* text) {
    if (!path || !text || !file_exists(path)) return false;
    chmod(path, 0666);
    FILE* fp = fopen(path, "w");
    if (!fp) return false;
    bool ok = fputs(text, fp) >= 0;
    fflush(fp);
    fclose(fp);
    return ok;
}

static int gs_read_int(const char* path, int fallback) {
    char buf[64];
    if (!gs_read_text(path, buf, sizeof(buf))) return fallback;
    return atoi(buf);
}

static void gs_read_int_array(const char* path, int* out, int max_count) {
    char buf[256];
    if (!out || max_count <= 0 || !gs_read_text(path, buf, sizeof(buf))) return;
    char* saveptr = NULL;
    char* token = strtok_r(buf, " \t", &saveptr);
    int i = 0;
    while (token && i < max_count) { out[i++] = atoi(token); token = strtok_r(NULL, " \t", &saveptr); }
}

static void gs_parse_ipc_list(const std::string& value, int* out) {
    for (int i = 0; i < GS_IPC_LEVELS; i++) out[i] = 0;
    const char* cursor = value.c_str();
    for (int i = 0; i < GS_IPC_LEVELS; i++) {
        char* end = NULL;
        long v = strtol(cursor, &end, 10);
        if (!end || end == cursor) break;
        out[i] = (int)v;
        cursor = end;
    }
}

static void gs_reset_config() {
    g_gs.enabled = true;
    g_gs.game_package_count = 0;
    safe_strncpy(g_gs.governor, "performance", sizeof(g_gs.governor));
    g_gs.max_first = 2745600;
    g_gs.min_first = 384000;
    g_gs.max_last = 2880000;
    g_gs.min_last = 768000;
    safe_strncpy(g_gs.cpuset_game, "0-6", sizeof(g_gs.cpuset_game));
    g_gs.walt_high_perf_cap = 1497600;
    g_gs.walt_partial_halt = 1555200;
    for (int i = 0; i < GS_IPC_LEVELS; i++) { g_gs.ipc_c0[i] = 1497600; g_gs.ipc_c1[i] = 1555200; }
    g_gs.core_ctl_enable = 0;
}

static void gs_add_packages(const std::string& value) {
    size_t start = 0;
    while (start <= value.size() && g_gs.game_package_count < GS_MAX_PACKAGES) {
        size_t comma = value.find(',', start);
        std::string item = value.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
        item = trim_copy(item);
        if (!item.empty()) {
            safe_strncpy(g_gs.game_packages[g_gs.game_package_count], item.c_str(),
                         sizeof(g_gs.game_packages[0]));
            g_gs.game_package_count++;
        }
        if (comma == std::string::npos) break;
        start = comma + 1;
    }
}

static bool gs_is_game_package(const char* pkg) {
    if (!pkg || !pkg[0]) return false;
    for (int i = 0; i < g_gs.game_package_count; i++) {
        if (strcmp(pkg, g_gs.game_packages[i]) == 0) return true;
    }
    return false;
}

static void gs_load_config() {
    gs_reset_config();
    FILE* fp = fopen(GS_CONF_PATH, "r");
    if (!fp) {
        log_config_message("game_scenario: 未找到 game_scenario.conf，使用内置默认值（仅 CFM 包名需要配置）");
        return;
    }
    char line[512];
    while (fgets(line, sizeof(line), fp)) {
        std::string text = trim_copy(std::string(line));
        if (text.empty() || text[0] == '#') continue;
        size_t eq = text.find('=');
        if (eq == std::string::npos) continue;
        std::string key = trim_copy(text.substr(0, eq));
        std::string value = trim_copy(text.substr(eq + 1));
        if (key == "enabled") {
            g_gs.enabled = atoi(value.c_str()) != 0;
        } else if (key == "game_packages") {
            g_gs.game_package_count = 0;
            gs_add_packages(value);
        } else if (key == "governor") {
            safe_strncpy(g_gs.governor, value.c_str(), sizeof(g_gs.governor));
        } else if (key == "game_max_first") {
            g_gs.max_first = atoi(value.c_str());
        } else if (key == "game_min_first") {
            g_gs.min_first = atoi(value.c_str());
        } else if (key == "game_max_last") {
            g_gs.max_last = atoi(value.c_str());
        } else if (key == "game_min_last") {
            g_gs.min_last = atoi(value.c_str());
        } else if (key == "cpuset_game") {
            safe_strncpy(g_gs.cpuset_game, value.c_str(), sizeof(g_gs.cpuset_game));
        } else if (key == "walt_high_perf_cap") {
            g_gs.walt_high_perf_cap = atoi(value.c_str());
        } else if (key == "walt_partial_halt") {
            g_gs.walt_partial_halt = atoi(value.c_str());
        } else if (key == "ipc_levels_cluster0") {
            gs_parse_ipc_list(value, g_gs.ipc_c0);
        } else if (key == "ipc_levels_cluster1") {
            gs_parse_ipc_list(value, g_gs.ipc_c1);
        } else if (key == "core_ctl_enable") {
            g_gs.core_ctl_enable = atoi(value.c_str());
        }
    }
    fclose(fp);
    char msg[448];
    safe_snprintf(msg, sizeof(msg),
                  "game_scenario: enabled=%d | packages=%d | governor=%s | max %d/%d min %d/%d | cpuset=%s | walt=%d/%d | core_ctl=%d",
                  g_gs.enabled ? 1 : 0, g_gs.game_package_count, g_gs.governor,
                  g_gs.max_first, g_gs.max_last, g_gs.min_first, g_gs.min_last,
                  g_gs.cpuset_game, g_gs.walt_high_perf_cap, g_gs.walt_partial_halt,
                  g_gs.core_ctl_enable);
    log_config_message(msg);
}

static const char* gs_walt_ipc_path(int cluster) {
    static char buf[2][128];
    if (cluster < 0 || cluster > 1) return NULL;
    safe_snprintf(buf[cluster], sizeof(buf[0]), "/proc/sys/walt/cluster%d/smart_freq/ipc_freq_levels", cluster);
    return buf[cluster];
}

static void gs_write_ipc_flat(int cluster, const int* values, bool restore_original) {
    const char* path = gs_walt_ipc_path(cluster);
    if (!path || !file_exists(path) || !values) return;
    char text[128];
    safe_snprintf(text, sizeof(text), "%d %d %d %d %d\n",
                  values[0], values[1], values[2], values[3], values[4]);
    gs_write_text(path, text);
    (void)restore_original;
}

static void gs_apply_walt_caps(int high_perf, int partial_halt) {
    if (file_exists("/proc/sys/walt/sched_high_perf_cluster_freq_cap")) {
        char text[128];
        safe_snprintf(text, sizeof(text), "%d %d\n", high_perf, high_perf);
        gs_write_text("/proc/sys/walt/sched_high_perf_cluster_freq_cap", text);
    }
    if (file_exists("/proc/sys/walt/sched_max_freq_partial_halt")) {
        char text[64];
        safe_snprintf(text, sizeof(text), "%d\n", partial_halt);
        gs_write_text("/proc/sys/walt/sched_max_freq_partial_halt", text);
    }
}

static void gs_save_originals() {
    memset(&g_gs_saved, 0, sizeof(g_gs_saved));
    g_gs_saved.valid = true;
    for (int i = 0; i < policy_count && i < GS_MAX_POLICIES; i++) {
        int p = cpu_policies[i].policy_num;
        char path[192], val[64];
        g_gs_saved.gov_policy[i] = p;
        safe_snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpufreq/policy%d/scaling_governor", p);
        if (gs_read_text(path, val, sizeof(val))) safe_strncpy(g_gs_saved.gov_value[i], val, sizeof(g_gs_saved.gov_value[0]));
        g_gs_saved.gov_count = i + 1;
        g_gs_saved.freq_policy[i] = p;
        safe_snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpufreq/policy%d/scaling_min_freq", p);
        g_gs_saved.orig_min[i] = gs_read_int(path, 0);
        safe_snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpufreq/policy%d/scaling_max_freq", p);
        g_gs_saved.orig_max[i] = gs_read_int(path, 0);
        g_gs_saved.freq_count = i + 1;
    }
    gs_read_text("/dev/cpuset/background/cpus", g_gs_saved.orig_cpuset_bg, sizeof(g_gs_saved.orig_cpuset_bg));
    gs_read_text("/dev/cpuset/system-background/cpus", g_gs_saved.orig_cpuset_sysbg, sizeof(g_gs_saved.orig_cpuset_sysbg));
    for (int c = 0; c < 2; c++) {
        const char* path = gs_walt_ipc_path(c);
        if (path && file_exists(path)) {
            gs_read_int_array(path, g_gs_saved.orig_ipc[c], GS_IPC_LEVELS);
            g_gs_saved.ipc_valid[c] = true;
        }
    }
    g_gs_saved.hp_count = platform_limits_read_int_tokens("/proc/sys/walt/sched_high_perf_cluster_freq_cap", g_gs_saved.orig_hp, 4);
    g_gs_saved.orig_partial_halt = gs_read_int("/proc/sys/walt/sched_max_freq_partial_halt", -1);
    g_gs_saved.partial_valid = g_gs_saved.orig_partial_halt >= 0;
    for (int cpu = 0; cpu < GS_MAX_CORE_CTL && g_gs_saved.core_ctl_count < GS_MAX_CORE_CTL; cpu++) {
        char path[160];
        safe_snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%d/core_ctl/enable", cpu);
        if (!file_exists(path)) continue;
        int value = gs_read_int(path, -1);
        if (value < 0) continue;
        safe_strncpy(g_gs_saved.core_ctl_path[g_gs_saved.core_ctl_count], path, sizeof(g_gs_saved.core_ctl_path[0]));
        g_gs_saved.core_ctl_value[g_gs_saved.core_ctl_count] = value;
        g_gs_saved.core_ctl_count++;
    }
}

static void gs_apply_policy_limits() {
    for (int i = 0; i < policy_count && i < GS_MAX_POLICIES; i++) {
        int p = cpu_policies[i].policy_num;
        char path[192];
        if (g_gs.governor[0]) {
            safe_snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpufreq/policy%d/scaling_governor", p);
            gs_write_text(path, g_gs.governor);
        }
        int target_max = (i == 0) ? g_gs.max_first : ((i == policy_count - 1) ? g_gs.max_last : 0);
        int target_min = (i == 0) ? g_gs.min_first : ((i == policy_count - 1) ? g_gs.min_last : 0);
        FreqTable table;
        bool has_table = freq_table_load(p, &table) > 0;
        std::vector<int> freq_list;
        if (has_table) freq_list.assign(table.freqs, table.freqs + table.count);
        if (target_max > 0) {
            if (has_table) target_max = snap_freq_ceiling(freq_list, target_max, target_max);
            safe_snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpufreq/policy%d/scaling_max_freq", p);
            char text[32];
            safe_snprintf(text, sizeof(text), "%d", target_max);
            gs_write_text(path, text);
        }
        if (target_min > 0) {
            if (has_table) target_min = snap_freq_floor(freq_list, target_min, target_min);
            safe_snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpufreq/policy%d/scaling_min_freq", p);
            char text[32];
            safe_snprintf(text, sizeof(text), "%d", target_min);
            gs_write_text(path, text);
        }
    }
}

static void gs_apply_game() {
    if (g_gs.game_package_count <= 0) {
        log_config_message("game_scenario: game_packages 为空，跳过（请在 config/game_scenario.conf 配置包名）");
        return;
    }
    gs_save_originals();
    gs_apply_policy_limits();
    if (g_gs.cpuset_game[0]) {
        gs_write_text("/dev/cpuset/background/cpus", g_gs.cpuset_game);
        gs_write_text("/dev/cpuset/system-background/cpus", g_gs.cpuset_game);
    }
    gs_apply_walt_caps(g_gs.walt_high_perf_cap, g_gs.walt_partial_halt);
    gs_write_ipc_flat(0, g_gs.ipc_c0, false);
    gs_write_ipc_flat(1, g_gs.ipc_c1, false);
    for (int i = 0; i < g_gs_saved.core_ctl_count; i++) {
        char text[16];
        safe_snprintf(text, sizeof(text), "%d", g_gs.core_ctl_enable);
        gs_write_text(g_gs_saved.core_ctl_path[i], text);
    }
    g_gs_active = true;
    g_game_scenario_active = true;
    g_gs_last_reapply_ms = get_current_time_ms();
    char msg[320];
    safe_snprintf(msg, sizeof(msg),
                  "game_scenario: 已应用游戏参数 %s | governor=%s | max %d/%d | min %d/%d | cpuset=%s | walt %d/%d | core_ctl=%d",
                  g_gs_active_pkg, g_gs.governor, g_gs.max_first, g_gs.max_last,
                  g_gs.min_first, g_gs.min_last, g_gs.cpuset_game,
                  g_gs.walt_high_perf_cap, g_gs.walt_partial_halt, g_gs.core_ctl_enable);
    log_config_message(msg);
}

static void gs_restore() {
    if (!g_gs_active) {
        g_game_scenario_active = false;
        return;
    }
    if (g_gs_saved.valid) {
        for (int i = 0; i < g_gs_saved.gov_count; i++) {
            char path[192];
            safe_snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpufreq/policy%d/scaling_governor", g_gs_saved.gov_policy[i]);
            if (g_gs_saved.gov_value[i][0]) gs_write_text(path, g_gs_saved.gov_value[i]);
        }
        for (int i = 0; i < g_gs_saved.freq_count; i++) {
            char path[192], text[32];
            if (g_gs_saved.orig_min[i] > 0) {
                safe_snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpufreq/policy%d/scaling_min_freq", g_gs_saved.freq_policy[i]);
                safe_snprintf(text, sizeof(text), "%d", g_gs_saved.orig_min[i]);
                gs_write_text(path, text);
            }
            if (g_gs_saved.orig_max[i] > 0) {
                safe_snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpufreq/policy%d/scaling_max_freq", g_gs_saved.freq_policy[i]);
                safe_snprintf(text, sizeof(text), "%d", g_gs_saved.orig_max[i]);
                gs_write_text(path, text);
            }
        }
        if (g_gs_saved.orig_cpuset_bg[0]) gs_write_text("/dev/cpuset/background/cpus", g_gs_saved.orig_cpuset_bg);
        if (g_gs_saved.orig_cpuset_sysbg[0]) gs_write_text("/dev/cpuset/system-background/cpus", g_gs_saved.orig_cpuset_sysbg);
        for (int c = 0; c < 2; c++) {
            if (g_gs_saved.ipc_valid[c]) gs_write_ipc_flat(c, g_gs_saved.orig_ipc[c], true);
        }
        if (g_gs_saved.hp_count > 0 && file_exists("/proc/sys/walt/sched_high_perf_cluster_freq_cap")) {
            char text[128];
            size_t used = 0;
            text[0] = '\0';
            for (int i = 0; i < g_gs_saved.hp_count && used < sizeof(text) - 2; i++) {
                int w = safe_snprintf(text + used, sizeof(text) - used, i ? " %d" : "%d", g_gs_saved.orig_hp[i]);
                if (w <= 0) break;
                used += (size_t)w;
            }
            safe_strcat(text, sizeof(text), "\n");
            gs_write_text("/proc/sys/walt/sched_high_perf_cluster_freq_cap", text);
        }
        if (g_gs_saved.partial_valid) {
            char text[32];
            safe_snprintf(text, sizeof(text), "%d", g_gs_saved.orig_partial_halt);
            gs_write_text("/proc/sys/walt/sched_max_freq_partial_halt", text);
        }
        for (int i = 0; i < g_gs_saved.core_ctl_count; i++) {
            char text[16];
            safe_snprintf(text, sizeof(text), "%d", g_gs_saved.core_ctl_value[i]);
            gs_write_text(g_gs_saved.core_ctl_path[i], text);
        }
    }
    g_gs_active = false;
    g_game_scenario_active = false;
    g_gs_last_reapply_ms = 0;
    g_gs_active_pkg[0] = '\0';
    log_config_message("game_scenario: 已恢复进入游戏前的参数");
}

static void gs_maybe_reload(long long now_ms) {
    if (now_ms - g_gs_last_check_ms < 5000) return;
    g_gs_last_check_ms = now_ms;
    struct stat st;
    if (stat(GS_CONF_PATH, &st) != 0) return;
    long long mtime_ms = (long long)st.st_mtime * 1000LL;
    if (mtime_ms == g_gs_conf_mtime_ms && (long long)st.st_size == g_gs_conf_size) return;
    g_gs_conf_mtime_ms = mtime_ms;
    g_gs_conf_size = (long long)st.st_size;
    bool was_active = g_gs_active;
    char active_pkg[128];
    safe_strncpy(active_pkg, g_gs_active_pkg, sizeof(active_pkg));
    if (was_active) gs_restore();
    gs_load_config();
    if (was_active) {
        if (gs_is_game_package(active_pkg)) {
            safe_strncpy(g_gs_active_pkg, active_pkg, sizeof(g_gs_active_pkg));
            gs_apply_game();
        }
    }
    log_config_message("game_scenario: 配置已热重载");
}

static void gs_atexit_restore() { gs_restore(); }

static void game_scenario_init() {
    if (g_gs_inited) return;
    g_gs_inited = true;
    gs_load_config();
    struct stat st;
    if (stat(GS_CONF_PATH, &st) == 0) {
        g_gs_conf_mtime_ms = (long long)st.st_mtime * 1000LL;
        g_gs_conf_size = (long long)st.st_size;
    }
    atexit(gs_atexit_restore);
}

static void game_scenario_tick(long long now_ms, const char* current_app) {
    game_scenario_init();
    if (now_ms <= 0) now_ms = get_current_time_ms();
    gs_maybe_reload(now_ms);
    if (!g_gs.enabled) {
        if (g_gs_active) gs_restore();
        return;
    }
    bool want = current_app && gs_is_game_package(current_app);
    if (want && !g_gs_active) {
        safe_strncpy(g_gs_active_pkg, current_app, sizeof(g_gs_active_pkg));
        gs_apply_game();
    } else if (!want && g_gs_active) {
        gs_restore();
    }
    /* 模式配置/前台切换可能把 governor/频率写回，场景激活期间每 2s 重新贴一次。 */
    if (g_gs_active && now_ms - g_gs_last_reapply_ms >= 2000) {
        g_gs_last_reapply_ms = now_ms;
        gs_apply_policy_limits();
    }
}

#endif /* MURONG_ACTIVITY_GAME_SCENARIO_INC */
