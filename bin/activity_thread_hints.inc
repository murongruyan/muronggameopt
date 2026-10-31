/*
 * 6.12/6.13：线程级「厂商接口」与可选「全局 RT 模式」——完整版 / 纯线程版共用。
 *
 * 为什么放在这里：
 *   FPSGO boost_affinity、WALT sched_per_task_boost、pipeline 提示，本质上都是
 *   **线程级放核/迁移提示**，不是频率闭环；全局 RT 模式同理，只改线程的调度策略。
 *   所以纯线程版也应该有这两项。之前它们写在动态调频执行器里，纯线程版不编译
 *   那条链路就一起丢了，属于实现位置问题，不是定位问题。
 *
 * 6.13 语义修正：RT 属于线程分类，不再属于动态调频。
 *   - rt 块的新位置：线程配置文件（bin/cpu/<cpuModel>.threads.json，加载器同时兼容
 *     <拓扑>.threads.json 与旧版 <拓扑>.json），统一由 get_thread_signature_config_path()
 *     取路径；这样纯线程版（不带 dyn.json）也能读到 RT 配置。
 *   - 仅当线程配置里没有顶层 rt 块时，才回退读取 bin/cpu/dyn.json 的 rt，并打印一次
 *     弃用提示（向后兼容旧配置，旧位置后续版本移除）。
 *   - vendor_hints 仍来自 dyn.json（厂商接口开关不属于 RT 语义）。
 */
struct ThreadVendorHintsConfig {
    bool enabled;
    bool fpsgo;
    bool walt;
    bool pipeline;
};

struct ThreadRtConfig {
    bool enabled;
    std::string policy;   /* rr | fifo */
    int priority;         /* 1..99 */
    bool write_sysctls;
};

static ThreadVendorHintsConfig g_thread_vendor_hints = {false, true, true, false};
static ThreadRtConfig g_thread_rt = {false, "rr", 1, true};

static long long g_thread_hints_last_cfg_ms = 0;
static long long g_thread_hints_last_tick_ms = 0;
static long long g_thread_hints_vendor_pid = 0;
static long long g_thread_hints_rt_pid = 0;
static bool g_thread_hints_rt_sysctl_done = false;
/* 旧 dyn.json.rt 回退只提示一次，避免 5s 轮询刷日志。 */
static bool g_thread_rt_dyn_fallback_logged = false;

static ThreadRtConfig thread_hints_default_rt_config() {
    ThreadRtConfig config;
    config.enabled = false;
    config.policy = "rr";
    config.priority = 1;
    config.write_sysctls = true;
    return config;
}

static bool thread_hints_write_text(const char* path, const char* text) {
    if (!path || !text) return false;
    FILE* fp = fopen(path, "w");
    if (!fp) return false;
    int ok = fprintf(fp, "%s", text);
    int close_ok = fclose(fp);
    return ok > 0 && close_ok == 0;
}

static char* thread_hints_read_file(const char* path, long max_bytes) {
    if (!path || !path[0]) return NULL;
    FILE* fp = fopen(path, "rb");
    if (!fp) return NULL;
    fseek(fp, 0, SEEK_END);
    long len = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (len <= 0 || len > max_bytes) {
        fclose(fp);
        return NULL;
    }
    char* data = (char*)malloc((size_t)len + 1);
    if (!data) {
        fclose(fp);
        return NULL;
    }
    size_t read_size = fread(data, 1, (size_t)len, fp);
    data[read_size] = '\0';
    fclose(fp);
    return data;
}

/* 解析顶层 rt 对象；线程配置与旧 dyn.json.rt 共用同一套字段语义。 */
static void thread_hints_apply_rt_config(cJSON* rt, ThreadRtConfig* out) {
    if (!rt || !cJSON_IsObject(rt) || !out) return;
    cJSON* enabled = cJSON_GetObjectItem(rt, "enabled");
    if (enabled && (cJSON_IsBool(enabled) || cJSON_IsNumber(enabled))) {
        out->enabled = enabled->valueint != 0;
    }
    cJSON* policy = cJSON_GetObjectItem(rt, "policy");
    if (policy && cJSON_IsString(policy) && policy->valuestring) {
        std::string s = scheduler_to_lower_copy(policy->valuestring);
        if (s == "rr" || s == "fifo") out->policy = s;
    }
    cJSON* priority = cJSON_GetObjectItem(rt, "priority");
    if (priority && cJSON_IsNumber(priority)) {
        int p = priority->valueint;
        if (p >= 1 && p <= 99) out->priority = p;
    }
    cJSON* write_sysctls = cJSON_GetObjectItem(rt, "write_sysctls");
    if (write_sysctls && (cJSON_IsBool(write_sysctls) || cJSON_IsNumber(write_sysctls))) {
        out->write_sysctls = write_sysctls->valueint != 0;
    }
}

/*
 * 从线程配置文件读取顶层 rt 块。
 * 返回 true 表示线程配置显式声明了 rt（即使字段非法也按默认值处理）；返回 false
 * 表示线程配置没有 rt（旧拓扑模板 / DSL 规则 / 文件不存在），由调用方回退旧 dyn.json.rt。
 */
static bool thread_hints_load_rt_from_thread_config(ThreadRtConfig* out) {
    const char* path = get_thread_signature_config_path();
    if (!path || !path[0]) return false;
    char* data = thread_hints_read_file(path, 1024 * 1024);
    if (!data) return false;
    cJSON* root = cJSON_Parse(data);
    free(data);
    if (!root) return false;
    cJSON* rt = cJSON_GetObjectItem(root, "rt");
    bool found = (rt && cJSON_IsObject(rt));
    if (found) {
        thread_hints_apply_rt_config(rt, out);
    }
    cJSON_Delete(root);
    return found;
}

static void thread_hints_load_config(bool force) {
    long long now = get_current_time_ms();
    if (!force && now - g_thread_hints_last_cfg_ms < 5000) return;
    g_thread_hints_last_cfg_ms = now;

    /* RT 属于线程分类：优先读线程配置文件的顶层 rt；旧 dyn.json.rt 只作回退。 */
    ThreadRtConfig rt_config = thread_hints_default_rt_config();
    bool rt_from_thread_config = thread_hints_load_rt_from_thread_config(&rt_config);

    FILE* fp = fopen(CPU_CONFIG_DIR "dyn.json", "rb");
    if (!fp) {
        g_thread_rt = rt_config;
        return;
    }
    fseek(fp, 0, SEEK_END);
    long len = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (len <= 0 || len > 65536) {
        fclose(fp);
        g_thread_rt = rt_config;
        return;
    }
    char* data = (char*)malloc((size_t)len + 1);
    if (!data) {
        fclose(fp);
        g_thread_rt = rt_config;
        return;
    }
    size_t read_size = fread(data, 1, (size_t)len, fp);
    data[read_size] = '\0';
    fclose(fp);

    cJSON* root = cJSON_Parse(data);
    free(data);
    if (!root) {
        g_thread_rt = rt_config;
        return;
    }

    cJSON* vendor = cJSON_GetObjectItem(root, "vendor_hints");
    if (vendor && cJSON_IsObject(vendor)) {
        cJSON* enabled = cJSON_GetObjectItem(vendor, "enabled");
        if (enabled && (cJSON_IsBool(enabled) || cJSON_IsNumber(enabled))) {
            g_thread_vendor_hints.enabled = enabled->valueint != 0;
        }
        cJSON* fpsgo = cJSON_GetObjectItem(vendor, "fpsgo");
        if (fpsgo && (cJSON_IsBool(fpsgo) || cJSON_IsNumber(fpsgo))) {
            g_thread_vendor_hints.fpsgo = fpsgo->valueint != 0;
        }
        cJSON* walt = cJSON_GetObjectItem(vendor, "walt");
        if (walt && (cJSON_IsBool(walt) || cJSON_IsNumber(walt))) {
            g_thread_vendor_hints.walt = walt->valueint != 0;
        }
        cJSON* pipeline = cJSON_GetObjectItem(vendor, "pipeline");
        if (pipeline && (cJSON_IsBool(pipeline) || cJSON_IsNumber(pipeline))) {
            g_thread_vendor_hints.pipeline = pipeline->valueint != 0;
        }
    }

    if (!rt_from_thread_config) {
        cJSON* legacy_rt = cJSON_GetObjectItem(root, "rt");
        if (legacy_rt && cJSON_IsObject(legacy_rt)) {
            thread_hints_apply_rt_config(legacy_rt, &rt_config);
            if (!g_thread_rt_dyn_fallback_logged) {
                g_thread_rt_dyn_fallback_logged = true;
                log_config_message("RT 配置位置已变更：请在 bin/cpu/<cpuModel>.threads.json 的顶层 rt 块配置；"
                                   "当前仍从 bin/cpu/dyn.json 的 rt 回退读取（旧位置已弃用，后续版本移除）");
            }
        }
    }

    cJSON_Delete(root);
    g_thread_rt = rt_config;
}

static void thread_hints_rt_restore() {
    if (g_thread_hints_rt_pid > 0) {
        struct sched_param sp = {};
        if (sched_setscheduler((pid_t)g_thread_hints_rt_pid, SCHED_OTHER, &sp) == 0) {
            char msg[160];
            safe_snprintf(msg, sizeof(msg), "RT模式恢复 | pid:%d -> SCHED_OTHER",
                          (int)g_thread_hints_rt_pid);
            log_affinity_message(msg);
        }
        g_thread_hints_rt_pid = 0;
    }
}

/*
 * 每 ~1s 调一次：先刷新配置（RT 读线程配置文件的 rt 块，vendor_hints 读 dyn.json），
 * 再对前台游戏主线程应用厂商接口提示 / 可选 RT 模式。纯线程版也走这条路径。
 */
static void thread_hints_tick() {
    long long now = get_current_time_ms();
    thread_hints_load_config(false);
    if (now - g_thread_hints_last_tick_ms < 1000) return;
    g_thread_hints_last_tick_ms = now;

    pid_t fg_pid = UnifiedScheduler::get_foreground_pid_snapshot();
    const bool game_foreground =
        fg_pid > 0 && g_current_foreground_app[0] &&
        strcmp(g_current_foreground_app, "unknown") != 0 &&
        is_game_package_name(g_current_foreground_app);

    /* 全局 RT 模式（默认关） */
    if (g_thread_rt.enabled && game_foreground) {
        if (g_thread_hints_rt_pid != fg_pid) {
            thread_hints_rt_restore();
            if (g_thread_rt.write_sysctls && !g_thread_hints_rt_sysctl_done) {
                g_thread_hints_rt_sysctl_done = true;
                thread_hints_write_text("/proc/sys/kernel/sched_rr_timeslice_ms", "3");
                thread_hints_write_text("/proc/sys/kernel/sched_rt_period_us", "30000");
                thread_hints_write_text("/proc/sys/kernel/sched_rt_runtime_us", "29500");
            }
            int policy = (g_thread_rt.policy == "fifo") ? SCHED_FIFO : SCHED_RR;
            struct sched_param sp = {};
            sp.sched_priority = g_thread_rt.priority;
            if (sched_setscheduler(fg_pid, policy, &sp) == 0) {
                g_thread_hints_rt_pid = fg_pid;
                char msg[192];
                safe_snprintf(msg, sizeof(msg), "RT模式已应用 | app:%s | pid:%d | %s prio:%d",
                              g_current_foreground_app, (int)fg_pid,
                              g_thread_rt.policy.c_str(), g_thread_rt.priority);
                log_affinity_message(msg);
            }
        }
    } else {
        thread_hints_rt_restore();
    }

    /* 厂商接口：线程级放核/迁移提示（默认关） */
    if (g_thread_vendor_hints.enabled && game_foreground &&
        g_thread_hints_vendor_pid != fg_pid) {
        char value[96];
        if (g_thread_vendor_hints.fpsgo &&
            file_exists("/sys/kernel/fpsgo/fbt/fbt_attr_by_pid")) {
            safe_snprintf(value, sizeof(value), "boost_affinity s %d 2", (int)fg_pid);
            thread_hints_write_text("/sys/kernel/fpsgo/fbt/fbt_attr_by_pid", value);
        }
        if (g_thread_vendor_hints.walt &&
            file_exists("/proc/sys/walt/sched_per_task_boost")) {
            safe_snprintf(value, sizeof(value), "%d 2", (int)fg_pid);
            thread_hints_write_text("/proc/sys/walt/sched_per_task_boost", value);
        }
        if (g_thread_vendor_hints.pipeline &&
            file_exists("/proc/sys/walt/sched_pipeline_special")) {
            thread_hints_write_text("/proc/sys/walt/sched_pipeline_special", "0");
        }
        g_thread_hints_vendor_pid = fg_pid;
        char msg[256];
        safe_snprintf(msg, sizeof(msg),
                      "厂商接口提示 | app:%s | pid:%d | fpsgo:%d walt:%d pipeline:%d",
                      g_current_foreground_app, (int)fg_pid,
                      g_thread_vendor_hints.fpsgo ? 1 : 0,
                      g_thread_vendor_hints.walt ? 1 : 0,
                      g_thread_vendor_hints.pipeline ? 1 : 0);
        log_affinity_message(msg);
    }
}
