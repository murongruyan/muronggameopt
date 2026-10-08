static std::vector<std::string> g_scheduler_skip_process_packages;

static bool is_valid_scheduler_skip_package(const std::string& package) {
    if (package.size() < 5 || package.size() >= MAX_PACKAGE_LEN ||
        package.find('.') == std::string::npos ||
        !isalpha((unsigned char)package[0])) {
        return false;
    }
    for (size_t i = 0; i < package.size(); i++) {
        const char c = package[i];
        if (!(isalnum((unsigned char)c) || c == '.' || c == '_' || c == '-')) {
            return false;
        }
    }
    return package.front() != '.' && package.back() != '.';
}

static bool load_scheduler_skip_processes_config() {
    FILE* fp = fopen(SCHEDULER_SKIP_PROCESSES_FILE, "r");
    if (!fp) {
        g_scheduler_skip_process_packages.clear();
        log_message("未找到进程调度排除名单，按空名单处理");
        return false;
    }

    std::vector<std::string> loaded;
    char line[MAX_LINE_LEN];
    while (fgets(line, sizeof(line), fp)) {
        line[strcspn(line, "\r\n")] = '\0';
        std::string package = trim_copy(line);
        if (package.size() >= 3 &&
            (unsigned char)package[0] == 0xEF &&
            (unsigned char)package[1] == 0xBB &&
            (unsigned char)package[2] == 0xBF) {
            package.erase(0, 3);
            package = trim_copy(package);
        }
        if (package.empty() || package[0] == '#') continue;
        const size_t inline_comment = package.find('#');
        if (inline_comment != std::string::npos) {
            package = trim_copy(package.substr(0, inline_comment));
        }
        package = scheduler_to_lower_copy(package.c_str());
        if (!is_valid_scheduler_skip_package(package)) {
            char message[320];
            safe_snprintf(
                message,
                sizeof(message),
                "忽略无效的进程调度排除项: %s",
                package.c_str());
            log_message(message);
            continue;
        }
        loaded.push_back(package);
    }
    fclose(fp);

    std::sort(loaded.begin(), loaded.end());
    loaded.erase(std::unique(loaded.begin(), loaded.end()), loaded.end());
    g_scheduler_skip_process_packages.swap(loaded);

    char message[192];
    safe_snprintf(
        message,
        sizeof(message),
        "已加载进程调度排除名单，共 %zu 个包名",
        g_scheduler_skip_process_packages.size());
    log_message(message);
    return true;
}

static bool process_name_matches_scheduler_skip_package(
    const char* process_name,
    const std::string& package
) {
    if (!process_name || !process_name[0] || package.empty()) return false;
    const size_t package_size = package.size();
    if (strncmp(process_name, package.c_str(), package_size) != 0) return false;
    return process_name[package_size] == '\0' || process_name[package_size] == ':';
}

static bool find_running_scheduler_skip_process(
    char* out_package,
    size_t out_package_size,
    pid_t* out_pid
) {
    if (out_package && out_package_size > 0) out_package[0] = '\0';
    if (out_pid) *out_pid = -1;
    if (g_scheduler_skip_process_packages.empty()) return false;

    DIR* proc_dir = opendir("/proc");
    if (!proc_dir) return false;

    bool found = false;
    struct dirent* entry = NULL;
    while (!found && (entry = readdir(proc_dir)) != NULL) {
        if (!entry->d_name[0] || !isdigit((unsigned char)entry->d_name[0])) continue;
        char* end = NULL;
        long parsed_pid = strtol(entry->d_name, &end, 10);
        if (!end || *end != '\0' || parsed_pid <= 0) continue;

        char cmdline_path[96];
        safe_snprintf(
            cmdline_path,
            sizeof(cmdline_path),
            "/proc/%ld/cmdline",
            parsed_pid);
        FILE* cmdline_file = fopen(cmdline_path, "rb");
        if (!cmdline_file) continue;

        char process_name[MAX_APP_NAME_LEN] = {0};
        size_t bytes_read = fread(
            process_name,
            1,
            sizeof(process_name) - 1,
            cmdline_file);
        fclose(cmdline_file);
        if (bytes_read == 0) continue;
        process_name[bytes_read] = '\0';

        std::string lowered_process = scheduler_to_lower_copy(process_name);
        for (size_t i = 0; i < g_scheduler_skip_process_packages.size(); i++) {
            const std::string& package = g_scheduler_skip_process_packages[i];
            if (!process_name_matches_scheduler_skip_package(
                    lowered_process.c_str(),
                    package)) {
                continue;
            }
            if (out_package && out_package_size > 0) {
                safe_strncpy(out_package, package.c_str(), out_package_size);
            }
            if (out_pid) *out_pid = (pid_t)parsed_pid;
            found = true;
            break;
        }
    }
    closedir(proc_dir);
    return found;
}

static bool refresh_scheduler_skip_process_state() {
    char matched_package[MAX_PACKAGE_LEN] = {0};
    pid_t matched_pid = -1;
    const bool active = find_running_scheduler_skip_process(
        matched_package,
        sizeof(matched_package),
        &matched_pid);
    const int previous = g_scheduler_skip_process_active.exchange(active ? 1 : 0);
    if (previous == (active ? 1 : 0)) return false;

    char message[384];
    if (active) {
        safe_snprintf(
            message,
            sizeof(message),
            "检测到调度排除进程，暂停全部调度写入并释放线程策略 | package:%s | pid:%d",
            matched_package,
            (int)matched_pid);
    } else {
        safe_snprintf(
            message,
            sizeof(message),
            "调度排除进程已退出，恢复慕容调度");
    }
    log_message(message);
    return true;
}

static bool is_scheduler_skip_process_active() {
    return g_scheduler_skip_process_active.load() != 0;
}

static void handle_scheduler_skip_process_transition() {
    g_dynamic_tuning_active = 0;
    reset_dynamic_tuning_state();
    UnifiedScheduler::reset_cluster_freq_overrides();
    UnifiedScheduler::refresh_bpf_targets_now();
    if (!is_scheduler_skip_process_active()) {
        apply_settings();
    }
}
