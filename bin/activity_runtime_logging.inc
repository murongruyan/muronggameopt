static void append_timestamped_log_line(const char* path, const char* message) {
    if (!path || !message) return;
    FILE* log = fopen(path, "a");
    if (!log) return;
    time_t now = time(NULL);
    struct tm* tm_info = localtime(&now);
    char timestamp[32];
    strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", tm_info);
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
    if (strcmp(path, TRACE_LOG_FILE) == 0) return "trace_log";
    if (strcmp(path, AFFINITY_LOG_FILE) == 0) return "affinity_log";
    if (strcmp(path, STATS_LOG_FILE) == 0) return "stats_log";
    return "misc_log";
}

static void backup_log_file(const char* path, bool announce) {
    if (!path) return;
    mkdir(LOG_BACKUP_DIR, 0755);
    time_t now = time(NULL);
    struct tm* tm_info = localtime(&now);
    char backup_path[256];
    safe_snprintf(backup_path, sizeof(backup_path), "%s/%s_%04d%02d%02d_%02d%02d%02d.bak",
        LOG_BACKUP_DIR, get_backup_prefix_for_log(path),
        tm_info->tm_year + 1900, tm_info->tm_mon + 1, tm_info->tm_mday,
        tm_info->tm_hour, tm_info->tm_min, tm_info->tm_sec);

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


void backup_and_clear_log() {
    mkdir(LOG_BACKUP_DIR, 0755);
    backup_log_file(LOG_FILE, false);
    backup_log_file(DEBUG_LOG_FILE, false);
    backup_log_file(TRACE_LOG_FILE, false);
    backup_log_file(AFFINITY_LOG_FILE, false);
    backup_log_file(STATS_LOG_FILE, false);
    ensure_log_file_exists(LOG_FILE);
    ensure_log_file_exists(DEBUG_LOG_FILE);
    ensure_log_file_exists(TRACE_LOG_FILE);
    ensure_log_file_exists(AFFINITY_LOG_FILE);
    ensure_log_file_exists(STATS_LOG_FILE);
}


void cleanup_old_logs() {
    DIR* dir = opendir(LOG_BACKUP_DIR);
    if (!dir) return;

    struct dirent** namelist;
    int n = scandir(LOG_BACKUP_DIR, &namelist, NULL, alphasort);

    if (n < 0) {
        closedir(dir);
        return;
    }

    if (n > 12) {
        for (int i = 0; i < n - 12; i++) {
            char filepath[256];
            safe_snprintf(filepath, sizeof(filepath), "%s/%s", LOG_BACKUP_DIR, namelist[i]->d_name);
            if (unlink(filepath) == 0) {
                char log_msg[256];
                safe_snprintf(log_msg, sizeof(log_msg), "删除旧日志备份: %s", namelist[i]->d_name);
                append_timestamped_log_line(LOG_FILE, log_msg);
            }

            free(namelist[i]);
        }

        for (int i = n - 12; i < n; i++) {
            free(namelist[i]);
        }
    }
    else {
        
        for (int i = 0; i < n; i++) {
            free(namelist[i]);
        }
    }

    free(namelist);
    closedir(dir);
}

static size_t get_max_log_file_size_for(const char* path) {
    if (!path) return MAX_LOG_FILE_SIZE;
    if (strcmp(path, DEBUG_LOG_FILE) == 0 ||
        strcmp(path, TRACE_LOG_FILE) == 0 ||
        strcmp(path, AFFINITY_LOG_FILE) == 0 ||
        strcmp(path, STATS_LOG_FILE) == 0) {
        return MAX_AUX_LOG_FILE_SIZE;
    }
    return MAX_LOG_FILE_SIZE;
}


void clear_log_if_needed_for(const char* path) {
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

static bool is_aux_log_enabled() {
    if (g_dyn_params.debug_log) {
        return true;
    }
    for (int i = 0; i < cluster_count && i < MAX_CLUSTERS; i++) {
        if (g_cluster_dyn_params[i].debug_log) {
            return true;
        }
    }
    return false;
}

void log_debug_message(const char* message) {
    if (!is_aux_log_enabled()) {
        return;
    }
    static long long last_log_ms = 0;
    long long now_ms = get_current_time_ms();
    if (now_ms - last_log_ms < 1000) {
        return;
    }
    last_log_ms = now_ms;

    clear_log_if_needed_for(DEBUG_LOG_FILE);
    append_timestamped_log_line(DEBUG_LOG_FILE, message);
}

static void log_affinity_message(const char* message) {
    if (!is_aux_log_enabled()) {
        return;
    }
    clear_log_if_needed_for(AFFINITY_LOG_FILE);
    append_timestamped_log_line(AFFINITY_LOG_FILE, message);
}

static void log_stats_message(const char* message) {
    if (!is_aux_log_enabled()) {
        return;
    }
    clear_log_if_needed_for(STATS_LOG_FILE);
    append_timestamped_log_line(STATS_LOG_FILE, message);
}

static void log_affinity_failure_once(const char* message) {
    static long long last_log_ms = 0;
    long long now_ms = get_current_time_ms();
    if (now_ms - last_log_ms < 1000) {
        return;
    }
    last_log_ms = now_ms;
    log_affinity_message(message);
}

static void emit_runtime_stats_snapshot(const char* current_app, long long now_ms) {
    static long long last_emit_ms = 0;
    static long long prev_loop_ticks = 0;
    static long long prev_policy_eval_ticks = 0;
    static long long prev_freq_update_count = 0;
    static long long prev_affinity_apply_count = 0;
    static long long prev_affinity_fail_count = 0;
    static long long prev_guard_reapply_count = 0;
    static long long prev_boost_request_count = 0;
    static long long prev_apply_settings_count = 0;
    static long long prev_foreground_switch_count = 0;

    if (now_ms - last_emit_ms < 5000) {
        return;
    }

    long long loop_ticks = g_runtime_stats.loop_ticks.load();
    long long policy_eval_ticks = g_runtime_stats.policy_eval_ticks.load();
    long long freq_update_count = g_runtime_stats.freq_update_count.load();
    long long affinity_apply_count = g_runtime_stats.affinity_apply_count.load();
    long long affinity_fail_count = g_runtime_stats.affinity_fail_count.load();
    long long guard_reapply_count = g_runtime_stats.guard_reapply_count.load();
    long long boost_request_count = g_runtime_stats.boost_request_count.load();
    long long apply_settings_count = g_runtime_stats.apply_settings_count.load();
    long long foreground_switch_count = g_runtime_stats.foreground_switch_count.load();

    char msg[512];
    safe_snprintf(
        msg,
        sizeof(msg),
        "runtime_stats | app:%s | mode:%s | loops:%lld(+%lld) | policy:%lld(+%lld) | freq:%lld(+%lld) | aff_ok:%lld(+%lld) | aff_fail:%lld(+%lld) | guard:%lld(+%lld) | boost:%lld(+%lld) | apply:%lld(+%lld) | fg_switch:%lld(+%lld)",
        current_app && current_app[0] ? current_app : "unknown",
        g_current_mode,
        loop_ticks, loop_ticks - prev_loop_ticks,
        policy_eval_ticks, policy_eval_ticks - prev_policy_eval_ticks,
        freq_update_count, freq_update_count - prev_freq_update_count,
        affinity_apply_count, affinity_apply_count - prev_affinity_apply_count,
        affinity_fail_count, affinity_fail_count - prev_affinity_fail_count,
        guard_reapply_count, guard_reapply_count - prev_guard_reapply_count,
        boost_request_count, boost_request_count - prev_boost_request_count,
        apply_settings_count, apply_settings_count - prev_apply_settings_count,
        foreground_switch_count, foreground_switch_count - prev_foreground_switch_count);
    log_stats_message(msg);
    trace_instantf("murong_stats",
                   "app=%s mode=%s loops=%lld policy=%lld freq=%lld aff=%lld guard=%lld boost=%lld apply=%lld fg=%lld",
                   current_app && current_app[0] ? current_app : "unknown",
                   g_current_mode,
                   loop_ticks,
                   policy_eval_ticks,
                   freq_update_count,
                   affinity_apply_count,
                   guard_reapply_count,
                   boost_request_count,
                   apply_settings_count,
                   foreground_switch_count);

    prev_loop_ticks = loop_ticks;
    prev_policy_eval_ticks = policy_eval_ticks;
    prev_freq_update_count = freq_update_count;
    prev_affinity_apply_count = affinity_apply_count;
    prev_affinity_fail_count = affinity_fail_count;
    prev_guard_reapply_count = guard_reapply_count;
    prev_boost_request_count = boost_request_count;
    prev_apply_settings_count = apply_settings_count;
    prev_foreground_switch_count = foreground_switch_count;
    last_emit_ms = now_ms;
}

static void log_scheduler_probe_once(const char* message) {
    static long long last_log_ms = 0;
    long long now_ms = get_current_time_ms();
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
