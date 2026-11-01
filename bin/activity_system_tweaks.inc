#ifndef MURONG_ACTIVITY_SYSTEM_TWEAKS_INC
#define MURONG_ACTIVITY_SYSTEM_TWEAKS_INC

/*
 * activity_system_tweaks.inc —— 模式级系统微调（参考 STRP 的 SmoothUI / Battery 两个模式）
 *
 * STRP 实测（在设备上拦截 STRP-POWER/STRPFREETWEAK 的系统调用并对比 sysfs）：
 *   SmoothUI(_a)：dm-* 队列 read_ahead_kb=32、nomerges=2；基础 prop；
 *                 ctl.start=perfd/thermald/thermal-engine/mpdecision、ctl.stop=logd/statsd；
 *                 low_power=0 / low_power_sticky=0 / screen_brightness_mode=0；drop_caches。
 *   Battery(_b) ：dm-* 队列 read_ahead_kb=128、nomerges=0；基础 prop；
 *                 low_power=1 / low_power_sticky=1 / screen_brightness_mode=1；drop_caches。
 *
 * 本模块映射：
 *   powersave 模式            -> 同步 STRP Battery（更省电）
 *   balance/performance/fast -> 同步 STRP SmoothUI（更平滑）
 * 只在「当前应用生效模式」变化时应用一次；不改 CPU 频率/电压（那些仍由本模块原有配置管）。
 */

#define ST_LAST_MODE_LEN 32

static char g_st_last_mode[ST_LAST_MODE_LEN] = "";
static bool g_st_inited = false;

static bool st_write_text(const char* path, const char* text) {
    if (!path || !text || !file_exists(path)) return false;
    chmod(path, 0666);
    FILE* fp = fopen(path, "w");
    if (!fp) return false;
    bool ok = fputs(text, fp) >= 0;
    fflush(fp);
    fclose(fp);
    return ok;
}

static void st_run_cmd(const char* cmd) {
    if (!cmd || !cmd[0]) return;
    std::string out;
    execute_command_all(cmd, out);
}

static void st_setprop(const char* key, const char* value) {
    if (!key || !value) return;
    if (strncmp(key, "ro.", 3) == 0) return; /* ro.* 运行期 setprop 必然失败，跳过 */
    char cmd[320];
    safe_snprintf(cmd, sizeof(cmd), "setprop %s %s", key, value);
    st_run_cmd(cmd);
}

static void st_settings_put(const char* ns, const char* key, const char* value) {
    char cmd[320];
    safe_snprintf(cmd, sizeof(cmd), "settings put %s %s %s", ns, key, value);
    st_run_cmd(cmd);
}

static void st_drop_caches() {
    st_write_text("/proc/sys/vm/drop_caches", "3\n");
}

/* dm-* 块设备队列：read_ahead_kb / nomerges（STRP 对 dm-0..42 逐个写）。 */
static void st_apply_io(int read_ahead_kb, int nomerges) {
    char ra[32], nm[32];
    safe_snprintf(ra, sizeof(ra), "%d", read_ahead_kb);
    safe_snprintf(nm, sizeof(nm), "%d", nomerges);
    for (int i = 0; i < 64; i++) {
        char path[192];
        safe_snprintf(path, sizeof(path), "/sys/block/dm-%d/queue/read_ahead_kb", i);
        if (!file_exists(path)) continue;
        st_write_text(path, ra);
        safe_snprintf(path, sizeof(path), "/sys/block/dm-%d/queue/nomerges", i);
        st_write_text(path, nm);
    }
}

/* STRP 两个模式共用的基础 prop（去掉 ro.* 这类运行期不可写的）。 */
static void st_apply_base_props() {
    static const char* props[][2] = {
        {"Dalvik.vm.heapgrowthlimit", "96m"},
        {"Dalvik.vm.heapmaxfree", "8m"},
        {"Dalvik.vm.minfree", "2m"},
        {"Dalvik.vm.heapsize", "200m"},
        {"Dalvik.vm.heapstaetsize", "16m"},
        {"Dalvik.vm.heaptargetutilization", "0.75"},
        {"logcat.live", "disable"},
        {"mot.proximity.delay", "0"},
        {"ring.delay", "0"},
        {"profiler.force_disable_err_rpt", "1"},
        {"profiler.force_disable_ulog", "1"},
        {"persist.android.strictmode", "0"},
        {"wifi.supplicant_scan_interval", "600"},
        {"persist.sys.purgeable_assets", "1"},
    };
    for (size_t i = 0; i < sizeof(props) / sizeof(props[0]); i++) {
        st_setprop(props[i][0], props[i][1]);
    }
}

static void st_apply_smooth_ui() {
    st_apply_io(32, 2);
    st_apply_base_props();
    /* 启动性能/温控相关服务、关掉日志服务（STRP SmoothUI 的行为）。 */
    st_setprop("ctl.start", "perfd");
    st_setprop("ctl.start", "thermald");
    st_setprop("ctl.start", "thermal-engine");
    st_setprop("ctl.start", "mpdecision");
    /* 不照搬 ctl.stop=logd/statsd：本模块按前台模式频繁切换，停日志服务会波及系统/抓日志；
     * STRP 那套是全局 profile 才会这么干。保留 perfd/thermald/thermal-engine 启动。 */
    st_settings_put("global", "low_power", "0");
    st_settings_put("global", "low_power_sticky", "0");
    st_settings_put("system", "screen_brightness_mode", "0");
    st_drop_caches();
    log_config_message("system_tweaks: 已应用 SmoothUI 参数（I/O 32/2 + ctl + low_power=0）");
}

static void st_apply_battery() {
    st_apply_io(128, 0);
    st_apply_base_props();
    st_settings_put("global", "low_power", "1");
    st_settings_put("global", "low_power_sticky", "1");
    st_settings_put("system", "screen_brightness_mode", "1");
    st_drop_caches();
    log_config_message("system_tweaks: 已应用 Battery 参数（I/O 128/0 + low_power=1）");
}

static void st_apply_for_mode(const char* mode) {
    if (!mode || !mode[0]) return;
    if (strcmp(mode, "powersave") == 0) {
        st_apply_battery();
    } else {
        st_apply_smooth_ui();
    }
}

static void system_tweaks_tick(const char* current_app) {
    if (!g_st_inited) {
        g_st_inited = true;
        log_config_message("system_tweaks: 已启用（powersave=Battery，其它=SmoothUI；由 STRP 实测参数同步）");
    }
    if (!current_app || !current_app[0]) return;
    char mode[ST_LAST_MODE_LEN] = {0};
    get_mode_for_package(current_app, mode, sizeof(mode));
    if (!mode[0]) return;
    if (strcmp(mode, g_st_last_mode) == 0) return;
    safe_strncpy(g_st_last_mode, mode, sizeof(g_st_last_mode));
    st_apply_for_mode(mode);
}

#endif /* MURONG_ACTIVITY_SYSTEM_TWEAKS_INC */
