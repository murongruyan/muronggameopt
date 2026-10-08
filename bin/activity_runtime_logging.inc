static std::recursive_mutex g_log_file_mutex;
static std::mutex g_affinity_log_throttle_mutex;

static bool safe_localtime_copy(time_t now, struct tm* out_tm) {
    if (!out_tm) return false;
#if defined(_WIN32)
    return localtime_s(out_tm, &now) == 0;
#else
    return localtime_r(&now, out_tm) != NULL;
#endif
}

static void append_timestamped_log_line(const char* path, const char* message) {
    if (!path || !message) return;
    std::lock_guard<std::recursive_mutex> lock(g_log_file_mutex);
    FILE* log = fopen(path, "a");
    if (!log) return;
    time_t now = time(NULL);
    struct tm tm_info = {};
    char timestamp[32];
    if (safe_localtime_copy(now, &tm_info)) {
        strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", &tm_info);
    } else {
        safe_strncpy(timestamp, "1970-01-01 00:00:00", sizeof(timestamp));
    }
    fprintf(log, "%s - %s\n", timestamp, message);
    fclose(log);
}

static void ensure_log_file_exists(const char* path) {
    FILE* fp = fopen(path, "a");
    if (fp) {
        fclose(fp);
        chmod(path, 0644);
    }
}

static void write_trace_marker_line(const char* line) {
    if (!line || !line[0]) return;
    static const char* trace_paths[] = {
        "/sys/kernel/tracing/trace_marker",
        "/sys/kernel/debug/tracing/trace_marker",
        NULL
    };
    for (int i = 0; trace_paths[i]; i++) {
        int fd = open(trace_paths[i], O_WRONLY | O_CLOEXEC);
        if (fd < 0) continue;
        write(fd, line, strlen(line));
        write(fd, "\n", 1);
        close(fd);
        return;
    }
}

static void trace_instantf(const char* category, const char* format, ...) {
    if (!category || !format) return;
    char payload[384];
    va_list args;
    va_start(args, format);
    vsnprintf(payload, sizeof(payload), format, args);
    va_end(args);
    char line[448];
    safe_snprintf(line, sizeof(line), "I|%d|%s|%s", getpid(), category, payload);
    write_trace_marker_line(line);
}

static const char* get_backup_prefix_for_log(const char* path) {
    if (strcmp(path, LOG_FILE) == 0) return "log";
    if (strcmp(path, DEBUG_LOG_FILE) == 0) return "debug_log";
    if (strcmp(path, FAS_LOG_FILE) == 0) return "fas_log";
    if (strcmp(path, AFFINITY_LOG_FILE) == 0) return "affinity_log";
    if (strcmp(path, STATS_LOG_FILE) == 0) return "stats_log";
    return "misc_log";
}

/*
 * 轮转一个日志文件。
 *
 * 【重要】文件不存在、或大小为 0 时【直接跳过】，不产生备份。
 * 早期版本无条件轮转全部 5 个日志，于是每次 daemon 重启都会为那些从未写过的
 * 辅助日志生成一堆 0 字节 .bak，把 log_backups 的配额全占满（实测 12 个里 12 个是空 backup），
 * 真正有用的主日志备份反而被挤掉。
 */
static void backup_log_file(const char* path, bool announce) {
    if (!path) return;
    std::lock_guard<std::recursive_mutex> lock(g_log_file_mutex);

    struct stat st;
    if (stat(path, &st) != 0 || st.st_size <= 0) {
        return;   /* 不存在或空文件：没有可备份的内容 */
    }

    mkdir(LOG_BACKUP_DIR, 0755);
    time_t now = time(NULL);
    struct tm tm_info = {};
    if (!safe_localtime_copy(now, &tm_info)) {
        memset(&tm_info, 0, sizeof(tm_info));
        tm_info.tm_year = 70;
        tm_info.tm_mday = 1;
    }
    char backup_path[256];
    safe_snprintf(backup_path, sizeof(backup_path), "%s/%s_%04d%02d%02d_%02d%02d%02d.bak",
        LOG_BACKUP_DIR, get_backup_prefix_for_log(path),
        tm_info.tm_year + 1900, tm_info.tm_mon + 1, tm_info.tm_mday,
        tm_info.tm_hour, tm_info.tm_min, tm_info.tm_sec);

    if (rename(path, backup_path) == 0) {
        ensure_log_file_exists(path);
        if (announce) {
            char msg[256];
            safe_snprintf(msg, sizeof(msg), "旧日志已备份为: %s", backup_path);
            append_timestamped_log_line(LOG_FILE, msg);
        }
    } else if (errno != ENOENT && announce) {
        char msg[256];
        safe_snprintf(msg, sizeof(msg), "日志备份失败: %s", strerror(errno));
        append_timestamped_log_line(LOG_FILE, msg);
    }
}


/*
 * 启动时的日志轮转。
 *
 * 只处理【已经有内容】的日志：backup_log_file() 内部会跳过不存在/空文件，
 * 这里也不再无条件把 5 个文件重建出来——那会在 config/ 里留一堆 0 字节空壳，
 * 软件日志页会显示成 "0/5"，用户以为坏了。
 *
 * 主日志在启动后马上会写「启动调度服务」，所以只有它是必须存在的。
 */
void backup_and_clear_log() {
    mkdir(LOG_BACKUP_DIR, 0755);
    backup_log_file(LOG_FILE, false);
    backup_log_file(DEBUG_LOG_FILE, false);
    backup_log_file(FAS_LOG_FILE, false);
    backup_log_file(AFFINITY_LOG_FILE, false);
    backup_log_file(STATS_LOG_FILE, false);
    ensure_log_file_exists(LOG_FILE);
}


/*
 * 备份配额：按【日志类型】分别计数，而不是所有备份共用一个数字。
 *
 * 旧实现是「log_backups 里总共只留 12 个」，于是辅助日志的备份会把主日志备份挤掉。
 * 现在主日志留 8 份（它是用户最关心的），每类辅助日志各留 2 份。
 *
 * 注意：删除旧备份【不再】往主日志写「删除旧日志备份: xxx」——
 * 一次启动能刷 20 多行纯噪音，主日志要留给状态变化。
 */
static int backup_quota_for_prefix(const char* prefix) {
    if (!prefix) return 2;
    if (strcmp(prefix, "log") == 0) return 8;
    return 2;
}

void cleanup_old_logs() {
    std::lock_guard<std::recursive_mutex> lock(g_log_file_mutex);
    DIR* dir = opendir(LOG_BACKUP_DIR);
    if (!dir) return;

    struct dirent** namelist;
    int n = scandir(LOG_BACKUP_DIR, &namelist, NULL, alphasort);
    if (n < 0) {
        closedir(dir);
        return;
    }

    /* 文件名形如 <prefix>_YYYYMMDD_HHMMSS.bak，只取最后一个 '_' 之前的部分作前缀 */
    std::unordered_map<std::string, std::vector<std::string>> by_prefix;
    for (int i = 0; i < n; i++) {
        const char* name = namelist[i]->d_name;
        if (name[0] == '.') { free(namelist[i]); continue; }
        std::string s(name);
        size_t dot = s.rfind('.');
        std::string stem = (dot == std::string::npos) ? s : s.substr(0, dot);
        size_t us = stem.rfind('_');
        std::string prefix = (us == std::string::npos) ? stem : stem.substr(0, us);
        by_prefix[prefix].push_back(s);
        free(namelist[i]);
    }
    free(namelist);
    closedir(dir);

    for (auto& kv : by_prefix) {
        std::vector<std::string>& files = kv.second;
        int quota = backup_quota_for_prefix(kv.first.c_str());
        if ((int)files.size() <= quota) continue;
        std::sort(files.begin(), files.end());   /* 名字带时间戳，字典序即时间序 */
        int remove_count = (int)files.size() - quota;
        for (int i = 0; i < remove_count; i++) {
            char filepath[256];
            safe_snprintf(filepath, sizeof(filepath), "%s/%s", LOG_BACKUP_DIR, files[i].c_str());
            unlink(filepath);
        }
    }
}

static size_t get_max_log_file_size_for(const char* path) {
    if (!path) return MAX_LOG_FILE_SIZE;
    if (strcmp(path, DEBUG_LOG_FILE) == 0 ||
        strcmp(path, FAS_LOG_FILE) == 0 ||
        strcmp(path, AFFINITY_LOG_FILE) == 0 ||
        strcmp(path, STATS_LOG_FILE) == 0) {
        return MAX_AUX_LOG_FILE_SIZE;
    }
    return MAX_LOG_FILE_SIZE;
}


void clear_log_if_needed_for(const char* path) {
    std::lock_guard<std::recursive_mutex> lock(g_log_file_mutex);
    struct stat st;
    size_t max_size = get_max_log_file_size_for(path);
    if (stat(path, &st) == 0 && st.st_size > (off_t)max_size) {
        backup_log_file(path, true);
    }
}

void log_message(const char* message) {
    clear_log_if_needed_for(LOG_FILE);
    append_timestamped_log_line(LOG_FILE, message);
}

/*
 * 同一类事件限流写主日志。用于"会反复发生但只需要留痕"的消息，
 * 例如前台 PID 反复刷新、前台识别连续失败——它们不该把主日志刷满。
 */
void log_message_throttled(const char* key, long long interval_ms, const char* message) {
    if (!key || !key[0] || !message) return;

    struct LogThrottleState {
        long long last_ms;
        unsigned long long suppressed;
    };
    static std::mutex throttle_mutex;
    static std::unordered_map<std::string, LogThrottleState> states;

    long long now_ms = get_current_time_ms();
    std::lock_guard<std::mutex> lock(throttle_mutex);
    auto& state = states[key];
    if (state.last_ms != 0 && now_ms - state.last_ms < interval_ms) {
        state.suppressed++;
        return;
    }

    unsigned long long suppressed = state.suppressed;
    state.last_ms = now_ms;
    state.suppressed = 0;

    if (suppressed > 0) {
        char summary[256];
        safe_snprintf(summary, sizeof(summary),
                      "%s | 同类消息已抑制 %llu 条", message, suppressed);
        log_message(summary);
        return;
    }
    log_message(message);
}

/*
 * 辅助日志开关：读 config/log.conf（每 5 秒检查一次改动）。
 *
 * 以前它寄生在动态调频参数里（g_dyn_params.debug_log / 每簇 debug_log），
 * 结果是"关动态调频"会顺手把 FAS、亲和性、统计日志一起关掉，反之亦然。
 * 现在开关独立成文件；旧位置仅作为迁移期兼容，读到 true 仍然生效。
 *
 * 文件格式（任选其一）：
 *   enabled=1
 *   1
 */
static bool runtime_log_config_enabled() {
    static bool loaded = false;
    static bool enabled = false;
    static long long last_check_ms = 0;

    long long now_ms = get_current_time_ms();
    if (loaded && now_ms - last_check_ms < 5000) {
        return enabled;
    }
    loaded = true;
    last_check_ms = now_ms;

    /* 这里用 fopen 直接读，避免依赖后置声明的辅助函数 */
    char text[64] = {0};
    FILE* fp = fopen(LOG_CONFIG_FILE, "r");
    if (!fp) {
        enabled = false;
        return enabled;
    }
    if (!fgets(text, sizeof(text), fp)) {
        text[0] = '\0';
    }
    fclose(fp);

    std::string value = trim_copy(text);
    const char* cursor = value.c_str();
    if (strncmp(cursor, "enabled", 7) == 0) {
        const char* eq = strchr(cursor, '=');
        if (eq) {
            value = trim_copy(std::string(eq + 1));
        }
    }
    std::string lowered = scheduler_to_lower_copy(value.c_str());
    enabled = (lowered == "1" || lowered == "true" || lowered == "on" || lowered == "yes");
    return enabled;
}

static bool is_aux_log_enabled() {
    /* 只认 config/log.conf；旧的 dynamic_tuning.debug_log 位置已废弃，不再读取。 */
    return runtime_log_config_enabled();
}

/*
 * 辅助日志「按需创建」：只在真正写入之后才确保文件存在并放开权限。
 *
 * 早期版本在每次 daemon 启动时无条件创建全部 5 个日志文件，即使一个字节都没写过，
 * 于是 config/ 里长期躺着 3~4 个 0 字节空壳，软件的日志页会显示成 "日志文件: 0/5"。
 * 现在只有写过的辅助日志才会出现在磁盘上——日志页显示的 "N/5" 因此是真实含义。
 */
static void aux_log_created(const char* path) {
    if (!path) return;
    struct stat st;
    if (stat(path, &st) != 0) return;
    if ((st.st_mode & 0777) != 0644) {
        chmod(path, 0644);
    }
}

void log_debug_message(const char* message) {
    if (!is_aux_log_enabled()) {
        return;
    }
    static std::mutex throttle_mutex;
    static long long last_log_ms = 0;
    long long now_ms = get_current_time_ms();
    std::lock_guard<std::mutex> lock(throttle_mutex);
    if (now_ms - last_log_ms < 1000) {
        return;
    }
    last_log_ms = now_ms;

    clear_log_if_needed_for(DEBUG_LOG_FILE);
    append_timestamped_log_line(DEBUG_LOG_FILE, message);
}

void log_fas_message(const char* message) {
    if (!is_aux_log_enabled()) {
        return;
    }
    clear_log_if_needed_for(FAS_LOG_FILE);
    append_timestamped_log_line(FAS_LOG_FILE, message);
    aux_log_created(FAS_LOG_FILE);
}

/*
 * 分级日志：主日志只留「状态类」信息，调频细节分流到各自文件。
 *
 *   log_message()        主日志 log.txt        —— 启动/模式切换/前台变化/档位加载/致命错误
 *   log_config_message() 调试日志 debug_log.txt —— 配置加载细节（动态调频动作表、日用负载表等）
 *   log_fas_message()    FAS 日志 fas_log.txt  —— control_trace / hitch（帧追踪）
 *   log_affinity_message() 亲和性日志          —— 线程绑核
 *   log_stats_message()  统计日志 stats_log.txt —— daily_write / band_override /
 *                                                  band_yield / 写入被钳制 / 写入未生效
 *
 * 这三个函数不做节流，调用方自己负责限流（例如 log_message_throttled）。
 */
void log_stats_message(const char* message) {
    if (!is_aux_log_enabled()) {
        return;
    }
    clear_log_if_needed_for(STATS_LOG_FILE);
    append_timestamped_log_line(STATS_LOG_FILE, message);
    aux_log_created(STATS_LOG_FILE);
}

void log_config_message(const char* message) {
    if (!is_aux_log_enabled()) {
        return;
    }
    clear_log_if_needed_for(DEBUG_LOG_FILE);
    append_timestamped_log_line(DEBUG_LOG_FILE, message);
    aux_log_created(DEBUG_LOG_FILE);
}

/*
 * 与 log_message_throttled() 同语义，但写到调试日志。
 * 用于「会反复发生、只需要留痕」的配置类告警，例如 pipeline cpu 配置越界——
 * 它每次调度都可能命中，不限流会把调试日志刷爆。
 */
void log_config_message_throttled(const char* key, long long interval_ms, const char* message) {
    if (!key || !key[0] || !message) return;
    static std::mutex throttle_mutex;
    static std::unordered_map<std::string, long long> last_ms_by_key;

    long long now_ms = get_current_time_ms();
    std::lock_guard<std::mutex> lock(throttle_mutex);
    long long& last_ms = last_ms_by_key[key];
    if (last_ms != 0 && now_ms - last_ms < interval_ms) {
        return;
    }
    last_ms = now_ms;
    log_config_message(message);
}

static void log_affinity_message(const char* message) {
    if (!is_aux_log_enabled()) {
        return;
    }
    auto extract_affinity_log_prefix = [](const char* text) -> std::string {
        if (!text || !text[0]) return "";
        if (strstr(text, "setaffinity_failed:")) return "绑核失败";
        if (strstr(text, "cpuset_recover:")) return "cpuset恢复";
        if (strncmp(text, "tid:", 4) == 0 &&
            (strstr(text, "explicit_mask:") || strstr(text, "mask:cluster-"))) {
            return "线程绑核应用";
        }
        const char* sep = strstr(text, " | ");
        if (!sep) return std::string(text);
        return std::string(text, (size_t)(sep - text));
    };
    auto get_affinity_log_min_interval_ms = [](const std::string& prefix) -> long long {
        if (prefix == "线程绑核应用") return 15000;
        if (prefix == "绑核失败") return 5000;
        if (prefix == "cpuset恢复") return 10000;
        /* 撤回失败会在每轮调度里重试，必须限流，否则一局能刷几千行。 */
        if (prefix == "恢复线程调度失败") return 10000;
        if (prefix == "恢复线程优先级失败") return 10000;
        if (prefix == "恢复线程绑核失败") return 10000;
        if (prefix == "锁定守护重打") return 15000;
        if (prefix == "线程采样") return 30000;
        if (prefix == "静态规则绑核") return 15000;
        if (prefix == "线程冻结") return 30000;
        if (prefix == "persistent规则应用") return 15000;
        return 0;
    };
    struct AffinityLogThrottleState {
        long long last_emit_ms;
        long long suppressed_since_ms;
        int suppressed_count;
    };
    static std::unordered_map<std::string, AffinityLogThrottleState> throttle_states;
    long long now_ms = get_current_time_ms();
    std::lock_guard<std::mutex> lock(g_affinity_log_throttle_mutex);
    std::string prefix = extract_affinity_log_prefix(message);
    long long min_interval_ms = get_affinity_log_min_interval_ms(prefix);
    if (min_interval_ms > 0 && !prefix.empty()) {
        AffinityLogThrottleState& state = throttle_states[prefix];
        if (state.last_emit_ms > 0 && now_ms - state.last_emit_ms < min_interval_ms) {
            if (state.suppressed_count == 0) {
                state.suppressed_since_ms = now_ms;
            }
            state.suppressed_count++;
            return;
        }
        clear_log_if_needed_for(AFFINITY_LOG_FILE);
        if (state.suppressed_count > 0) {
            char summary[256];
            long long suppressed_window_ms =
                state.suppressed_since_ms > 0 ? (now_ms - state.suppressed_since_ms) : 0;
            safe_snprintf(summary, sizeof(summary),
                          "日志限流汇总 | type:%s | suppressed:%d | window_ms:%lld",
                          prefix.c_str(),
                          state.suppressed_count,
                          suppressed_window_ms);
            append_timestamped_log_line(AFFINITY_LOG_FILE, summary);
            state.suppressed_count = 0;
            state.suppressed_since_ms = 0;
        }
        state.last_emit_ms = now_ms;
        append_timestamped_log_line(AFFINITY_LOG_FILE, message);
        aux_log_created(AFFINITY_LOG_FILE);
        return;
    }
    clear_log_if_needed_for(AFFINITY_LOG_FILE);
    append_timestamped_log_line(AFFINITY_LOG_FILE, message);
    aux_log_created(AFFINITY_LOG_FILE);
}

static void log_affinity_failure_once(const char* message) {
    static std::mutex throttle_mutex;
    static long long last_log_ms = 0;
    long long now_ms = get_current_time_ms();
    std::lock_guard<std::mutex> lock(throttle_mutex);
    if (now_ms - last_log_ms < 1000) {
        return;
    }
    last_log_ms = now_ms;
    log_affinity_message(message);
}

static void emit_runtime_stats_snapshot(const char* current_app, long long now_ms) {
    static long long last_emit_ms = 0;
    static long long prev_loop_ticks = 0;
    static long long prev_dynamic_tuning_ticks = 0;
    static long long prev_freq_update_count = 0;
    static long long prev_affinity_apply_count = 0;
    static long long prev_affinity_fail_count = 0;
    static long long prev_guard_reapply_count = 0;
    static long long prev_boost_request_count = 0;
    static long long prev_apply_settings_count = 0;
    static long long prev_foreground_switch_count = 0;
    static long long prev_thread_proc_scan_count = 0;
    static long long prev_thread_event_fork_count = 0;
    static long long prev_thread_event_exit_count = 0;
    static long long prev_thread_event_bind_count = 0;
    static long long prev_thread_event_unbind_count = 0;
    static long long prev_guard_yield_count = 0;
    static long long prev_affinity_backoff_count = 0;

    if (now_ms - last_emit_ms < 5000) {
        return;
    }

    long long loop_ticks = g_runtime_stats.loop_ticks.load();
    long long dynamic_tuning_ticks = g_runtime_stats.dynamic_tuning_ticks.load();
    long long freq_update_count = g_runtime_stats.freq_update_count.load();
    long long affinity_apply_count = g_runtime_stats.affinity_apply_count.load();
    long long affinity_fail_count = g_runtime_stats.affinity_fail_count.load();
    long long guard_reapply_count = g_runtime_stats.guard_reapply_count.load();
    long long boost_request_count = g_runtime_stats.boost_request_count.load();
    long long apply_settings_count = g_runtime_stats.apply_settings_count.load();
    long long foreground_switch_count = g_runtime_stats.foreground_switch_count.load();
    long long thread_proc_scan_count = g_runtime_stats.thread_proc_scan_count.load();
    long long thread_event_fork_count = g_runtime_stats.thread_event_fork_count.load();
    long long thread_event_exit_count = g_runtime_stats.thread_event_exit_count.load();
    long long thread_event_bind_count = g_runtime_stats.thread_event_bind_count.load();
    long long thread_event_unbind_count = g_runtime_stats.thread_event_unbind_count.load();
    long long guard_yield_count = g_runtime_stats.guard_yield_count.load();
    long long affinity_backoff_count = g_runtime_stats.affinity_backoff_count.load();

    char msg[768];
    safe_snprintf(
        msg,
        sizeof(msg),
        "runtime_stats | app:%s | mode:%s | loops:%lld(+%lld) | dyn:%lld(+%lld) | freq:%lld(+%lld) | aff_ok:%lld(+%lld) | aff_fail:%lld(+%lld) | aff_backoff:%lld(+%lld) | guard:%lld(+%lld) | guard_yield:%lld(+%lld) | boost:%lld(+%lld) | apply:%lld(+%lld) | fg_switch:%lld(+%lld) | proc_scan:%lld(+%lld) | ev_fork:%lld(+%lld) | ev_exit:%lld(+%lld) | ev_bind:%lld(+%lld) | ev_unbind:%lld(+%lld)",
        current_app && current_app[0] ? current_app : "unknown",
        g_current_mode,
        loop_ticks, loop_ticks - prev_loop_ticks,
        dynamic_tuning_ticks, dynamic_tuning_ticks - prev_dynamic_tuning_ticks,
        freq_update_count, freq_update_count - prev_freq_update_count,
        affinity_apply_count, affinity_apply_count - prev_affinity_apply_count,
        affinity_fail_count, affinity_fail_count - prev_affinity_fail_count,
        affinity_backoff_count, affinity_backoff_count - prev_affinity_backoff_count,
        guard_reapply_count, guard_reapply_count - prev_guard_reapply_count,
        guard_yield_count, guard_yield_count - prev_guard_yield_count,
        boost_request_count, boost_request_count - prev_boost_request_count,
        apply_settings_count, apply_settings_count - prev_apply_settings_count,
        foreground_switch_count, foreground_switch_count - prev_foreground_switch_count,
        thread_proc_scan_count, thread_proc_scan_count - prev_thread_proc_scan_count,
        thread_event_fork_count, thread_event_fork_count - prev_thread_event_fork_count,
        thread_event_exit_count, thread_event_exit_count - prev_thread_event_exit_count,
        thread_event_bind_count, thread_event_bind_count - prev_thread_event_bind_count,
        thread_event_unbind_count, thread_event_unbind_count - prev_thread_event_unbind_count);
    log_stats_message(msg);
    trace_instantf("murong_stats",
                   "app=%s mode=%s loops=%lld dyn=%lld freq=%lld aff=%lld guard=%lld boost=%lld apply=%lld fg=%lld",
                   current_app && current_app[0] ? current_app : "unknown",
                   g_current_mode,
                   loop_ticks,
                   dynamic_tuning_ticks,
                   freq_update_count,
                   affinity_apply_count,
                   guard_reapply_count,
                   boost_request_count,
                   apply_settings_count,
                   foreground_switch_count);

    prev_loop_ticks = loop_ticks;
    prev_dynamic_tuning_ticks = dynamic_tuning_ticks;
    prev_freq_update_count = freq_update_count;
    prev_affinity_apply_count = affinity_apply_count;
    prev_affinity_fail_count = affinity_fail_count;
    prev_guard_reapply_count = guard_reapply_count;
    prev_guard_yield_count = guard_yield_count;
    prev_affinity_backoff_count = affinity_backoff_count;
    prev_boost_request_count = boost_request_count;
    prev_apply_settings_count = apply_settings_count;
    prev_foreground_switch_count = foreground_switch_count;
    prev_thread_proc_scan_count = thread_proc_scan_count;
    prev_thread_event_fork_count = thread_event_fork_count;
    prev_thread_event_exit_count = thread_event_exit_count;
    prev_thread_event_bind_count = thread_event_bind_count;
    prev_thread_event_unbind_count = thread_event_unbind_count;
    last_emit_ms = now_ms;
}

static void log_scheduler_probe_once(const char* message) {
    static std::mutex throttle_mutex;
    static long long last_log_ms = 0;
    long long now_ms = get_current_time_ms();
    std::lock_guard<std::mutex> lock(throttle_mutex);
    if (now_ms - last_log_ms < 5000) {
        return;
    }
    last_log_ms = now_ms;
    log_affinity_message(message);
}

static void log_affinity_success_once(pid_t tid, const char* mask_desc) {
    if (tid <= 0 || !mask_desc || !mask_desc[0]) return;
    char msg[192];
    safe_snprintf(msg, sizeof(msg), "tid:%d | affinity_applied:%s", tid, mask_desc);
    log_scheduler_probe_once(msg);
}

