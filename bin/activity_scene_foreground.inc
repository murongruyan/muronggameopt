static char g_cached_foreground_activity[192] = "";

static void trim_process_suffix(char* package) {
    if (!package || !package[0]) return;
    char* colon = strchr(package, ':');
    if (colon) *colon = '\0';
}

static bool should_ignore_foreground_package(const char* package) {
    if (!package || !package[0]) return true;
    return strstr(package, "inputmethod") != NULL ||
           strstr(package, ".ime") != NULL ||
           strstr(package, "keyboard") != NULL ||
           strcmp(package, "com.sohu.inputmethod.sogouoem") == 0 ||
           strcmp(package, "com.oplus.ttsaccessibilityengine") == 0 ||
           strcmp(package, "com.coloros.assistantscreen") == 0 ||
           strcmp(package, "com.coloros.oshare") == 0 ||
           strcmp(package, "com.android.systemui") == 0 ||
           strcmp(package, "com.android.launcher") == 0 ||
           strcmp(package, "com.android.intentresolver") == 0;
}

static bool read_package_name_from_pid(pid_t pid, char* out_pkg, size_t out_size) {
    if (!out_pkg || out_size == 0 || pid <= 0) return false;
    out_pkg[0] = '\0';

    char path[128];
    safe_snprintf(path, sizeof(path), "/proc/%d/cmdline", pid);
    FILE* fp = fopen(path, "r");
    if (!fp) return false;

    char buf[256] = {0};
    size_t n = fread(buf, 1, sizeof(buf) - 1, fp);
    fclose(fp);
    if (n == 0) return false;

    buf[n] = '\0';
    trim_process_suffix(buf);
    if (!token_looks_like_package(buf) || should_ignore_foreground_package(buf)) return false;

    safe_strncpy(out_pkg, buf, out_size);
    return true;
}

static pid_t read_top_app_pid_from_cpuset_file(const char* path, char* out_pkg, size_t out_size) {
    if (!path || !out_pkg || out_size == 0 || !file_exists(path)) return -1;
    FILE* fp = fopen(path, "r");
    if (!fp) return -1;

    pid_t found_pid = -1;
    char line[64];
    while (fgets(line, sizeof(line), fp)) {
        pid_t pid = (pid_t)atoi(line);
        if (pid <= 0) continue;
        if (read_package_name_from_pid(pid, out_pkg, out_size)) {
            found_pid = pid;
        }
    }
    fclose(fp);
    return found_pid;
}

static pid_t read_top_app_pid_from_cpuset(char* out_pkg, size_t out_size) {
    pid_t top_pid = read_top_app_pid_from_cpuset_file("/dev/cpuset/top-app/cgroup.procs", out_pkg, out_size);
    if (top_pid > 0) return top_pid;
    return read_top_app_pid_from_cpuset_file("/dev/cpuset/top-app/tasks", out_pkg, out_size);
}

static bool extract_package_activity_from_line(const char* line,
                                               char* out_pkg,
                                               size_t out_pkg_size,
                                               char* out_activity,
                                               size_t out_activity_size) {
    if (!line || !out_pkg || out_pkg_size == 0 || !out_activity || out_activity_size == 0) {
        return false;
    }
    out_pkg[0] = '\0';
    bool found = false;
    out_activity[0] = '\0';

    std::string token;
    auto flush_token = [&]() {
        if (token.empty()) return;
        std::string candidate = token;
        token.clear();
        while (!candidate.empty() && (candidate.back() == ',' || candidate.back() == ';' ||
               candidate.back() == '}' || candidate.back() == ']' || candidate.back() == ')' ||
               candidate.back() == ':')) {
            candidate.pop_back();
        }
        while (!candidate.empty() && (candidate.front() == '{' || candidate.front() == '[' ||
               candidate.front() == '(' || candidate.front() == ',' || candidate.front() == '=')) {
            candidate.erase(candidate.begin());
        }
        size_t slash = candidate.find('/');
        if (slash == std::string::npos || slash == 0 || slash + 1 >= candidate.size()) return;
        std::string raw_pkg = candidate.substr(0, slash);
        std::string raw_activity = candidate.substr(slash + 1);
        std::string base_pkg = raw_pkg;
        trim_process_suffix(&base_pkg[0]);
        base_pkg.resize(strlen(base_pkg.c_str()));
        if (!token_looks_like_package(base_pkg) || should_ignore_foreground_package(base_pkg.c_str())) {
            return;
        }
        if (!raw_activity.empty() && raw_activity[0] == '.') {
            raw_activity = base_pkg + raw_activity;
        }
        safe_strncpy(out_pkg, base_pkg.c_str(), out_pkg_size);
        safe_strncpy(out_activity, raw_activity.c_str(), out_activity_size);
        found = true;
    };

    for (size_t i = 0; line[i] != '\0'; i++) {
        char c = line[i];
        bool keep = isalnum((unsigned char)c) || c == '.' || c == '_' ||
                    c == '-' || c == ':' || c == '/' || c == '$';
        if (keep) {
            token.push_back(c);
        } else {
            flush_token();
        }
    }
    flush_token();
    return found;
}

static bool read_foreground_component_from_command(const char* command,
                                                   const char* keyword_a,
                                                   const char* keyword_b,
                                                   char* out_pkg,
                                                   size_t out_pkg_size,
                                                   char* out_activity,
                                                   size_t out_activity_size) {
    if (!command || !out_pkg || out_pkg_size == 0 || !out_activity || out_activity_size == 0) {
        return false;
    }
    out_pkg[0] = '\0';
    out_activity[0] = '\0';

    FILE* fp = popen(command, "r");
    if (!fp) return false;

    bool found = false;
    char line[512];
    while (fgets(line, sizeof(line), fp)) {
        std::string lower = line;
        std::transform(lower.begin(), lower.end(), lower.begin(),
                       [](unsigned char c) { return (char)tolower(c); });
        if (keyword_a && keyword_a[0] && lower.find(keyword_a) == std::string::npos &&
            (!keyword_b || !keyword_b[0] || lower.find(keyword_b) == std::string::npos)) {
            continue;
        }
        if (extract_package_activity_from_line(line, out_pkg, out_pkg_size, out_activity, out_activity_size)) {
            found = true;
        }
    }
    pclose(fp);
    return found;
}

static bool read_top_resumed_activity_package(char* out_pkg,
                                              size_t out_pkg_size,
                                              char* out_activity,
                                              size_t out_activity_size) {
    if (!out_pkg || out_pkg_size == 0 || !out_activity || out_activity_size == 0) return false;
    out_pkg[0] = '\0';
    out_activity[0] = '\0';

    if (read_foreground_component_from_command("dumpsys activity top-resumed-activity 2>/dev/null",
                                               NULL, NULL,
                                               out_pkg, out_pkg_size,
                                               out_activity, out_activity_size)) {
        return true;
    }
    if (read_foreground_component_from_command("dumpsys window windows 2>/dev/null",
                                               "mcurrentfocus", "mfocusedapp",
                                               out_pkg, out_pkg_size,
                                               out_activity, out_activity_size)) {
        return true;
    }
    return read_foreground_component_from_command("dumpsys activity activities 2>/dev/null",
                                                  "topresumedactivity", "mresumedactivity",
                                                  out_pkg, out_pkg_size,
                                                  out_activity, out_activity_size);
}

static const char* get_cached_foreground_activity_name() {
    return g_cached_foreground_activity;
}

char* get_foreground_app() {
    static char cached_app[MAX_PACKAGE_LEN] = "unknown";
    static char cached_dumpsys_app[MAX_PACKAGE_LEN] = "unknown";
    static char cached_cpuset_app[MAX_PACKAGE_LEN] = "unknown";
    static char last_mismatch_cpuset[MAX_PACKAGE_LEN] = "";
    static char last_mismatch_dumpsys[MAX_PACKAGE_LEN] = "";
    static long long last_dumpsys_ms = 0;
    static long long last_stable_foreground_ms = 0;
    static long long last_mismatch_log_ms = 0;
    static const long long dumpsys_refresh_interval_ms = 2000;
    static const long long dumpsys_cache_ttl_ms = 10000;
    static const long long stable_foreground_ttl_ms = 20000;
    static int cpuset_stable_hits = 0;

    long long now_ms = get_current_time_ms();
    char pkg_buf[MAX_PACKAGE_LEN] = {0};
    pid_t top_pid = read_top_app_pid_from_cpuset(pkg_buf, sizeof(pkg_buf));
    bool cpuset_ok = top_pid > 0 && pkg_buf[0];
    if (cpuset_ok) {
        if (strcmp(pkg_buf, cached_cpuset_app) == 0) {
            if (cpuset_stable_hits < 1000000) cpuset_stable_hits++;
        } else {
            safe_strncpy(cached_cpuset_app, pkg_buf, sizeof(cached_cpuset_app));
            cpuset_stable_hits = 1;
        }
    } else {
        cached_cpuset_app[0] = '\0';
        cpuset_stable_hits = 0;
    }

    char dumpsys_pkg[MAX_PACKAGE_LEN] = {0};
    char dumpsys_activity[192] = {0};
    bool should_refresh_dumpsys =
        last_dumpsys_ms == 0 ||
        now_ms - last_dumpsys_ms >= dumpsys_refresh_interval_ms ||
        strcmp(cached_app, "unknown") == 0;
    bool dumpsys_ok = false;

    if (should_refresh_dumpsys) {
        dumpsys_ok = read_top_resumed_activity_package(dumpsys_pkg, sizeof(dumpsys_pkg),
                                                       dumpsys_activity, sizeof(dumpsys_activity));
        if (dumpsys_ok) {
            safe_strncpy(cached_dumpsys_app, dumpsys_pkg, sizeof(cached_dumpsys_app));
            safe_strncpy(g_cached_foreground_activity, dumpsys_activity, sizeof(g_cached_foreground_activity));
            last_dumpsys_ms = now_ms;
        }
    } else if (cached_dumpsys_app[0] && strcmp(cached_dumpsys_app, "unknown") != 0) {
        safe_strncpy(dumpsys_pkg, cached_dumpsys_app, sizeof(dumpsys_pkg));
        safe_strncpy(dumpsys_activity, g_cached_foreground_activity, sizeof(dumpsys_activity));
        dumpsys_ok = true;
    }

    if (cpuset_ok && dumpsys_ok && strcmp(pkg_buf, dumpsys_pkg) != 0 &&
        ((strcmp(pkg_buf, last_mismatch_cpuset) != 0 ||
          strcmp(dumpsys_pkg, last_mismatch_dumpsys) != 0) ||
         now_ms - last_mismatch_log_ms >= 15000)) {
        
        // cpuset 读到的是旧 app（系统 cgroup 悬挂），立即失效它的稳定计数
        // 防止 cpuset_stable_hits >= 3 后与 dumpsys 产生冲突
        cpuset_stable_hits = 0;
        cached_cpuset_app[0] = '\0';
        
        last_mismatch_log_ms = now_ms;
        safe_strncpy(last_mismatch_cpuset, pkg_buf, sizeof(last_mismatch_cpuset));
        safe_strncpy(last_mismatch_dumpsys, dumpsys_pkg, sizeof(last_mismatch_dumpsys));
    }

    if (dumpsys_ok) {
        safe_strncpy(cached_app, dumpsys_pkg, sizeof(cached_app));
        last_stable_foreground_ms = now_ms;
        char* result = strdup(cached_app);
        if (result) return result;
    }

    if (cached_dumpsys_app[0] && strcmp(cached_dumpsys_app, "unknown") != 0 &&
        now_ms - last_dumpsys_ms < dumpsys_cache_ttl_ms) {
        safe_strncpy(cached_app, cached_dumpsys_app, sizeof(cached_app));
        last_stable_foreground_ms = now_ms;
        char* result = strdup(cached_dumpsys_app);
        if (result) return result;
    }

    if (cached_app[0] && strcmp(cached_app, "unknown") != 0 &&
        now_ms - last_stable_foreground_ms < stable_foreground_ttl_ms) {
        char* result = strdup(cached_app);
        if (result) return result;
    }

    if (cpuset_ok && cpuset_stable_hits >= 3 &&
        (!cached_app[0] || strcmp(cached_app, "unknown") == 0 ||
         strcmp(pkg_buf, cached_app) == 0)) {
        safe_strncpy(cached_app, pkg_buf, sizeof(cached_app));
        last_stable_foreground_ms = now_ms;
        char* result = strdup(cached_app);
        if (result) return result;
    }

    char* result = strdup(cached_app[0] ? cached_app : "unknown");
    if (!result) {
        log_message("内存分配失败：返回默认值");
        return NULL;
    }
    return result;
}
