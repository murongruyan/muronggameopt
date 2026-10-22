static char g_cached_foreground_activity[192] = "";

#include <dlfcn.h>
#include <thread>
#include <mutex>
#include <atomic>
#include <poll.h>
#include <sys/wait.h>

static void trim_process_suffix(char* package) {
    if (!package || !package[0]) return;
    char* colon = strchr(package, ':');
    if (colon) *colon = '\0';
}

static bool token_looks_like_process_name(const std::string& token) {
    if (token.size() < 5 || token.find('.') == std::string::npos) return false;
    if (!isalpha((unsigned char)token[0])) return false;
    if (token.front() == '.' || token.back() == '.') return false;
    int dot_count = 0;
    for (size_t i = 0; i < token.size(); i++) {
        char c = token[i];
        if (!(isalnum((unsigned char)c) || c == '.' || c == '_' || c == '-' || c == ':')) {
            return false;
        }
        if (c == '.') dot_count++;
    }
    return dot_count >= 2;
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

static bool read_process_name_from_pid(pid_t pid, char* out_process, size_t out_size) {
    if (!out_process || out_size == 0 || pid <= 0) return false;
    out_process[0] = '\0';

    char path[128];
    safe_snprintf(path, sizeof(path), "/proc/%d/cmdline", pid);
    FILE* fp = fopen(path, "r");
    if (!fp) return false;

    char buf[256] = {0};
    size_t n = fread(buf, 1, sizeof(buf) - 1, fp);
    fclose(fp);
    if (n == 0) return false;

    buf[n] = '\0';
    std::string raw_process = buf;
    if (!token_looks_like_process_name(raw_process)) return false;
    std::string base_package = raw_process;
    trim_process_suffix(&base_package[0]);
    base_package.resize(strlen(base_package.c_str()));
    if (!token_looks_like_package(base_package) || should_ignore_foreground_package(base_package.c_str())) {
        return false;
    }

    safe_strncpy(out_process, raw_process.c_str(), out_size);
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
                                               size_t out_activity_size,
                                               bool apply_ignore_filter = true) {
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
        if (!token_looks_like_package(base_pkg) ||
            (apply_ignore_filter && should_ignore_foreground_package(base_pkg.c_str()))) {
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
                                                   size_t out_activity_size,
                                                   bool* out_ignored_found = NULL) {
    if (!command || !out_pkg || out_pkg_size == 0 || !out_activity || out_activity_size == 0) {
        return false;
    }
    out_pkg[0] = '\0';
    out_activity[0] = '\0';
    if (out_ignored_found) *out_ignored_found = false;

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
            continue;
        }
        /*
         * 解析出了组件但被忽略名单挡下（桌面、SystemUI、输入法等）：说明前台
         * 确实存在，只是不该被调度。这个信息必须往外传，否则调用方会误判成
         * "dumpsys 失败"，然后退到 cpuset 猜测，随机抓到别的进程。
         */
        if (out_ignored_found) {
            char ignored_pkg[MAX_PACKAGE_LEN] = {0};
            char ignored_activity[192] = {0};
            if (extract_package_activity_from_line(line, ignored_pkg, sizeof(ignored_pkg),
                                                   ignored_activity, sizeof(ignored_activity),
                                                   false)) {
                *out_ignored_found = true;
            }
        }
    }
    pclose(fp);
    return found;
}

/*
 * 6.9：Binder 前台探测（对应 AsoulOpt 的 AIBinder_dump 方案）。
 * 从 AMS dump 文本里找 " TOP " 行，解析出 "pid:package/activity" 的 package。
 * libbinder_ndk 不存在（Android <9 或裁剪系统）时返回 false，调用方回退 dumpsys。
 */
static bool foreground_parse_top_line(const char* data, size_t len,
                                      char* out_pkg, size_t out_size) {
    if (!data || !out_pkg || out_size == 0) return false;
    static const char kToken[] = " TOP ";
    const char* p = data;
    const char* end = data + len;
    while (p < end) {
        const char* hit = std::search(p, end, kToken, kToken + 5);
        if (hit == end) return false;
        const char* q = hit + 5;
        const char* limit = end;
        if ((size_t)(limit - q) > 192) limit = q + 192;
        const char* colon = (const char*)memchr(q, ':', (size_t)(limit - q));
        if (colon) {
            const char* digits_begin = colon;
            while (digits_begin > q && isdigit((unsigned char)digits_begin[-1])) {
                digits_begin--;
            }
            if (digits_begin < colon) {
                const char* pkg_begin = colon + 1;
                const char* pkg_end = pkg_begin;
                while (pkg_end < limit &&
                       (isalnum((unsigned char)*pkg_end) || *pkg_end == '.' ||
                        *pkg_end == '_' || *pkg_end == '-')) {
                    pkg_end++;
                }
                size_t n = (size_t)(pkg_end - pkg_begin);
                if (n > 0 && n < out_size) {
                    std::string candidate(pkg_begin, n);
                    if (token_looks_like_process_name(candidate)) {
                        memcpy(out_pkg, pkg_begin, n);
                        out_pkg[n] = '\0';
                        return true;
                    }
                }
            }
        }
        p = hit + 1;
    }
    return false;
}

static bool binder_probe_impl(char* out_pkg, size_t out_size) {
    if (!out_pkg || out_size == 0) return false;
    typedef struct AIBinder AIBinder;
    typedef AIBinder* (*get_service_fn)(const char*);
    typedef void (*dump_fn)(AIBinder*, int);
    typedef void (*dec_strong_fn)(AIBinder*);

    static get_service_fn get_service = nullptr;
    static dump_fn do_dump = nullptr;
    static dec_strong_fn dec_strong = nullptr;
    static bool tried = false;
    if (!tried) {
        tried = true;
        void* handle = dlopen("libbinder_ndk.so", RTLD_NOW);
        if (handle) {
            get_service = (get_service_fn)dlsym(handle, "AServiceManager_getService");
            do_dump = (dump_fn)dlsym(handle, "AIBinder_dump");
            dec_strong = (dec_strong_fn)dlsym(handle, "AIBinder_decStrong");
        }
        if (!get_service || !do_dump || !dec_strong) {
            return false;
        }
    }
    if (!get_service || !do_dump || !dec_strong) return false;

    AIBinder* service = get_service("activity");
    if (!service) return false;

    int fds[2];
    if (pipe(fds) != 0) {
        dec_strong(service);
        return false;
    }

    std::mutex mu;
    std::atomic<bool> found(false);
    char found_pkg[MAX_PACKAGE_LEN] = {0};
    std::thread reader([&]() {
        char tmp[8192];
        std::string tail;
        while (true) {
            ssize_t n = read(fds[0], tmp, sizeof(tmp));
            if (n > 0) {
                if (!found.load()) {
                    std::lock_guard<std::mutex> lock(mu);
                    tail.append(tmp, (size_t)n);
                    if (tail.size() > 65536) {
                        tail.erase(0, tail.size() - 32768);
                    }
                    if (foreground_parse_top_line(tail.data(), tail.size(),
                                                  found_pkg, sizeof(found_pkg))) {
                        found.store(true);
                    }
                }
            } else if (n == 0) {
                break;
            } else if (errno == EINTR) {
                continue;
            } else {
                break;
            }
        }
        close(fds[0]);
    });

    do_dump(service, fds[1]);
    close(fds[1]);
    reader.join();
    dec_strong(service);

    if (!found.load() || !found_pkg[0]) return false;
    safe_strncpy(out_pkg, found_pkg, out_size);
    static int binder_success_logs = 0;
    if (binder_success_logs < 3) {
        binder_success_logs++;
        char msg[192];
        safe_snprintf(msg, sizeof(msg), "binder前台探测 | pkg:%s", out_pkg);
        log_config_message(msg);
    }
    return true;
}

/*
 * Binder dump 的宿主封装：放到子进程里做，避免 AIBinder_dump 内部异常
 * （真机上实测某些系统版本会 SIGSEGV）把调度守护进程一起带走。
 * 子进程成功则通过管道回传 package；超时/崩溃都只当失败，回退 dumpsys。
 */
static bool read_foreground_package_from_binder_dump(char* out_pkg, size_t out_size) {
    if (!out_pkg || out_size == 0) return false;

    int fds[2];
    if (pipe(fds) != 0) return false;

    pid_t child = fork();
    if (child == 0) {
        close(fds[0]);
        char pkg[MAX_PACKAGE_LEN] = {0};
        bool ok = binder_probe_impl(pkg, sizeof(pkg));
        if (ok && pkg[0]) {
            ssize_t ignored = write(fds[1], pkg, strlen(pkg));
            (void)ignored;
        }
        close(fds[1]);
        _exit(ok ? 0 : 1);
    }
    close(fds[1]);
    if (child < 0) {
        close(fds[0]);
        return false;
    }

    bool ok = false;
    struct pollfd pfd = {fds[0], POLLIN, 0};
    int ready = poll(&pfd, 1, 2500);
    if (ready > 0) {
        char buf[MAX_PACKAGE_LEN] = {0};
        ssize_t n = read(fds[0], buf, sizeof(buf) - 1);
        if (n > 0) {
            buf[n] = '\0';
            safe_strncpy(out_pkg, buf, out_size);
            ok = true;
        }
    }
    close(fds[0]);
    kill(child, SIGKILL);
    waitpid(child, NULL, 0);

    static int binder_success_logs = 0;
    static int binder_fail_logs = 0;
    if (ok && binder_success_logs < 3) {
        binder_success_logs++;
        char msg[192];
        safe_snprintf(msg, sizeof(msg), "binder前台探测 | pkg:%s", out_pkg);
        log_config_message(msg);
    } else if (!ok && binder_fail_logs < 2) {
        binder_fail_logs++;
        log_config_message("binder前台探测失败（子进程异常/超时），回退 dumpsys 链路");
    }
    return ok;
}

static bool read_top_resumed_activity_package(char* out_pkg,
                                              size_t out_pkg_size,
                                              char* out_activity,
                                              size_t out_activity_size,
                                              bool* out_ignored_found = NULL) {
    if (!out_pkg || out_pkg_size == 0 || !out_activity || out_activity_size == 0) return false;
    out_pkg[0] = '\0';
    out_activity[0] = '\0';
    if (out_ignored_found) *out_ignored_found = false;

    /*
     * 6.9：优先用 Binder dump 拿前台组件（不再每 2s 拉一个 dumpsys 子进程）。
     * AIBinder_dump 输出直接进管道，读线程边读边找 " TOP " 行；失败/无
     * libbinder_ndk 时回退原来的 dumpsys 链路。缓存 5s，避免频繁 dump。
     */
    {
        static long long last_binder_attempt_ms = 0;
        static long long last_binder_ok_ms = 0;
        static char binder_cached_pkg[MAX_PACKAGE_LEN] = {0};
        static char binder_cached_activity[192] = {0};
        long long now = get_current_time_ms();
        if (binder_cached_pkg[0] && now - last_binder_ok_ms < 5000) {
            if (should_ignore_foreground_package(binder_cached_pkg)) {
                if (out_ignored_found) *out_ignored_found = true;
                return false;
            }
            safe_strncpy(out_pkg, binder_cached_pkg, out_pkg_size);
            safe_strncpy(out_activity, binder_cached_activity, out_activity_size);
            return true;
        }
        if (now - last_binder_attempt_ms >= 5000) {
            last_binder_attempt_ms = now;
            char binder_pkg[MAX_PACKAGE_LEN] = {0};
            if (read_foreground_package_from_binder_dump(binder_pkg, sizeof(binder_pkg))) {
                if (!should_ignore_foreground_package(binder_pkg)) {
                    safe_strncpy(binder_cached_pkg, binder_pkg, sizeof(binder_cached_pkg));
                    binder_cached_activity[0] = '\0';
                    last_binder_ok_ms = now;
                    safe_strncpy(out_pkg, binder_pkg, out_pkg_size);
                    safe_strncpy(out_activity, binder_cached_activity, out_activity_size);
                    return true;
                }
                if (out_ignored_found) *out_ignored_found = true;
                return false;
            }
        }
    }

    bool ignored = false;
    if (read_foreground_component_from_command("dumpsys activity top-resumed-activity 2>/dev/null",
                                               NULL, NULL,
                                               out_pkg, out_pkg_size,
                                               out_activity, out_activity_size, &ignored)) {
        return true;
    }
    if (ignored) { if (out_ignored_found) *out_ignored_found = true; return false; }

    ignored = false;
    if (read_foreground_component_from_command("dumpsys window windows 2>/dev/null",
                                               "mcurrentfocus", "mfocusedapp",
                                               out_pkg, out_pkg_size,
                                               out_activity, out_activity_size, &ignored)) {
        return true;
    }
    if (ignored) { if (out_ignored_found) *out_ignored_found = true; return false; }

    ignored = false;
    bool ok = read_foreground_component_from_command("dumpsys activity activities 2>/dev/null",
                                                     "topresumedactivity", "mresumedactivity",
                                                     out_pkg, out_pkg_size,
                                                     out_activity, out_activity_size, &ignored);
    if (!ok && ignored && out_ignored_found) *out_ignored_found = true;
    return ok;
}

static const char* get_cached_foreground_activity_name() {
    return g_cached_foreground_activity;
}

bool get_foreground_app(char* out_app, size_t out_size) {
    if (!out_app || out_size == 0) return false;
    out_app[0] = '\0';
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
    bool dumpsys_ignored = false;

    if (should_refresh_dumpsys) {
        dumpsys_ok = read_top_resumed_activity_package(dumpsys_pkg, sizeof(dumpsys_pkg),
                                                       dumpsys_activity, sizeof(dumpsys_activity),
                                                       &dumpsys_ignored);
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

    /*
     * 真实前台是桌面 / SystemUI / 输入法这一类"不该被调度"的界面时：
     * 明确按"没有可调度的前台应用"处理，并且**不走 cpuset 猜测**。
     * 之前这里会把 dumpsys 的"被过滤"当成失败，回退到 cpuset 启发式，
     * 而那个启发式取的是 top-app cpuset 里最后一个可接受进程，
     * 结果抓到输入法当游戏调度（实测就是这么错了一个多小时）。
     */
    if (dumpsys_ignored && !dumpsys_ok) {
        safe_strncpy(cached_app, "unknown", sizeof(cached_app));
        safe_strncpy(cached_dumpsys_app, "unknown", sizeof(cached_dumpsys_app));
        cached_cpuset_app[0] = '\0';
        cpuset_stable_hits = 0;
        last_dumpsys_ms = now_ms;
        safe_strncpy(out_app, "unknown", out_size);
        return true;
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
        safe_strncpy(out_app, cached_app, out_size);
        return true;
    }

    if (cached_dumpsys_app[0] && strcmp(cached_dumpsys_app, "unknown") != 0 &&
        now_ms - last_dumpsys_ms < dumpsys_cache_ttl_ms) {
        safe_strncpy(cached_app, cached_dumpsys_app, sizeof(cached_app));
        last_stable_foreground_ms = now_ms;
        safe_strncpy(out_app, cached_dumpsys_app, out_size);
        return true;
    }

    if (cached_app[0] && strcmp(cached_app, "unknown") != 0 &&
        now_ms - last_stable_foreground_ms < stable_foreground_ttl_ms) {
        safe_strncpy(out_app, cached_app, out_size);
        return true;
    }

    if (cpuset_ok && cpuset_stable_hits >= 3 &&
        (!cached_app[0] || strcmp(cached_app, "unknown") == 0 ||
         strcmp(pkg_buf, cached_app) == 0)) {
        safe_strncpy(cached_app, pkg_buf, sizeof(cached_app));
        last_stable_foreground_ms = now_ms;
        safe_strncpy(out_app, cached_app, out_size);
        return true;
    }

    safe_strncpy(out_app, cached_app[0] ? cached_app : "unknown", out_size);
    return out_app[0] != '\0';
}
