static bool read_text_file(const char* path, char* buf, size_t buf_size);
static void refresh_foreground_pid_locked(bool force_refresh);
static void collect_process_threads_for_package_locked(
    pid_t pid,
    const char* package,
    bool use_proc_runtime,
    long long now_ms,
    std::vector<int>& seen);
static void apply_explicit_persistent_actions_for_process_locked(
    pid_t pid,
    const char* package,
    long long now_ms);
static void get_affinity_log_context_for_tid(
    pid_t tid,
    char* out_app,
    size_t out_app_size,
    char* out_comm,
    size_t out_comm_size,
    char* out_source,
    size_t out_source_size);
static void clear_latched_primary_tid_locked(pid_t tid, const char* reason);
static void reset_scheduler_session_locked(const char* reason);

struct ThreadRuntimeState {
    pid_t pid;
    pid_t tid;
    std::string package;
    std::string comm;
    long long discovered_ms;
    unsigned long long last_ticks;
    long long last_sample_ms;
    unsigned long long pending_runtime_ns;
    unsigned long long total_runtime_ns;
    float cpu_usage_pct;
    ThreadClass cls;
    int assigned_cluster;
    long long last_classify_ms;
    long long last_migrate_ms;
    std::vector<int> assigned_cpus;
    std::vector<int> last_target_cpus;
    std::vector<int> frozen_cpus;
    int last_target_cluster;
    int frozen_cluster;
    int hot_sample_count;
    long long first_hot_ms;
    long long target_stable_since_ms;
    long long frozen_at_ms;
    bool frozen;
    bool sched_baseline_captured;
    int baseline_nice;
    int baseline_sched_policy;
    int baseline_sched_priority;
    int applied_nice;
    int applied_sched_policy;
    int applied_sched_priority;
    long long last_event_log_ms;
    /* Phase1~4: 静态规则层专用字段 */
    bool has_explicit_affinity_rule;
    long long last_static_bind_ms;
    long long static_bind_cooldown_ms;
    int selection_epoch;
};

struct SelectionEpochState {
    int epoch;
    int last_lookup_loop_tick;
    std::vector<pid_t> selected_tids;
    long long stable_since_ms;
    bool has_pending;
    std::vector<pid_t> pending_tids;
    long long pending_since_ms;
    long long last_reselect_ms;
    long long selection_change_threshold_ms;
};

static const long long kSelectionChangeThresholdMs = 2000;
static const long long kRebindCooldownMs = 3000;

enum ProcessEventType {
    PROCESS_EVENT_FORK = 1,
    PROCESS_EVENT_EXEC = 2,
    PROCESS_EVENT_SET_AFFINITY = 3
};

struct ProcessEventSignal {
    __s32 pid;
    __s32 tid;
    char comm[16];
    __s32 type;
};

enum ThreadJsonRuleSource {
    THREAD_JSON_RULE_SOURCE_UNKNOWN = 0,
    THREAD_JSON_RULE_SOURCE_ROLE,
    THREAD_JSON_RULE_SOURCE_CUSTOM,
    THREAD_JSON_RULE_SOURCE_DEFAULT,
    THREAD_JSON_RULE_SOURCE_SCENE_DEFAULT
};

struct ThreadJsonResolvedRule {
    const ThreadJsonAction* action;
    const ThreadJsonSafetyConfig* safety;
    int role_kind;
    const char* rule_name;
    ThreadJsonRuleSource source_kind;
    int stable_hold_ms;
    int rebind_cooldown_ms;
    int selection_change_threshold_ms;
    bool pin_once;
    bool prefer_existing_binding;
};

enum SchedulerSessionPhase {
    SESSION_IDLE = 0,
    SESSION_TRACKING
};

struct SchedulerSessionState {
    SchedulerSessionPhase phase;
    pid_t pid;
    char package[MAX_APP_NAME_LEN];
    pid_t latched_main_tid;
    pid_t latched_render_tid;
    pid_t last_lost_main_tid;
    pid_t last_lost_render_tid;
    char latched_main_loss_reason[32];
    char latched_render_loss_reason[32];
};

static std::mutex g_mutex;
static std::unordered_map<int, ThreadRuntimeState> g_threads;
static ClusterPolicy g_cluster_policies[MAX_CLUSTERS];
static ClusterPolicy g_cluster_policy_base[MAX_CLUSTERS];
static int g_cluster_min_overrides[MAX_CLUSTERS] = {0};
static int g_cluster_max_overrides[MAX_CLUSTERS] = {0};
static char g_foreground_package[MAX_APP_NAME_LEN] = "unknown";
static pid_t g_foreground_pid = -1;
static long long g_last_pid_lookup_ms = 0;
static long long g_last_thread_scan_ms = 0;
static AllCpuStats g_prev_stats = {};
static int g_prev_stats_valid = 0;
static bool g_foreground_explicit_thread_rule_active = false;
static std::map<uint64_t, SelectionEpochState> g_selection_epochs;
static long long g_last_epoch_stale_cleanup_ms = 0;
static pthread_t g_runtime_tid;
static std::atomic<int> g_runtime_thread_started(0);
static std::atomic<int> g_runtime_thread_stop(0);
static std::atomic<int> g_bpf_ready(0);
static std::atomic<int> g_bpf_failed(0);
static long long g_last_bpf_poll_error_ms = 0;
static struct bpf_object* g_bpf_obj = nullptr;
static struct bpf_program* g_bpf_switch_prog = nullptr;
static struct bpf_program* g_bpf_wakeup_prog = nullptr;
static struct bpf_program* g_bpf_process_fork_prog = nullptr;
static struct bpf_program* g_bpf_process_exec_prog = nullptr;
static struct bpf_program* g_bpf_process_affinity_prog = nullptr;
static struct bpf_link* g_bpf_switch_link = nullptr;
static struct bpf_link* g_bpf_wakeup_link = nullptr;
static struct bpf_link* g_bpf_process_fork_link = nullptr;
static struct bpf_link* g_bpf_process_exec_link = nullptr;
static struct bpf_link* g_bpf_process_affinity_link = nullptr;
static struct ring_buffer* g_process_event_ring = nullptr;
static int g_target_tgid_map_fd = -1;
static int g_thread_runtime_map_fd = -1;
static int g_cpu_last_ts_map_fd = -1;
static int g_urgent_flag_map_fd = -1;
static int g_target_tid_map_fd = -1;
static int g_process_event_rb_map_fd = -1;
static bool g_foreground_pid_refresh_pending = false;
static pid_t g_installed_target_tgid = -1;
static long long g_boost_until_ms[MAX_CLUSTERS] = {0};
static SchedulerSessionState g_scheduler_session = {};
static long long g_last_render_tid_sync_ms = 0;
static const long long kThreadFreezeLearnMs = 90 * 1000;
static const long long kThreadFreezeStableMs = 15 * 1000;
static const int kThreadFreezeMinSamples = 12;

void describe_frame_probe_state(const char* package, char* out, size_t out_size) {
    if (!out || out_size == 0) return;
    safe_snprintf(out, out_size,
                  "pkg=%s frame_probe=disabled",
                  package ? package : "unknown");
}

static struct bpf_program* find_bpf_program_compat(const char* program_name, const char* section_name) {
    if (!g_bpf_obj) return nullptr;

    struct bpf_program* prog = nullptr;
    if (program_name && program_name[0]) {
        prog = bpf_object__find_program_by_name(g_bpf_obj, program_name);
        if (prog) {
            return prog;
        }
    }

    struct bpf_program* it = nullptr;
    while ((it = bpf_object__next_program(g_bpf_obj, it)) != nullptr) {
        const char* name = bpf_program__name(it);
        const char* sec = bpf_program__section_name(it);
        if (program_name && program_name[0] && name && strcmp(name, program_name) == 0) {
            return it;
        }
        if (section_name && section_name[0] && sec) {
            size_t prefix_len = strlen(section_name);
            if (strncmp(sec, section_name, prefix_len) == 0 &&
                (sec[prefix_len] == '\0' || sec[prefix_len] == '/')) {
                return it;
            }
        }
        if (section_name && section_name[0] && sec && strcmp(sec, section_name) == 0) {
            return it;
        }
    }
    return nullptr;
}

static bool is_render_thread_name(const char* comm);
static bool is_main_thread_name_for_package(const char* package, const char* comm, pid_t tid);

static bool is_hot_priority_thread_locked(pid_t tid, const char* comm, float cpu_usage_pct) {
    if (!comm || !comm[0]) return false;
    std::string name = scheduler_to_lower_copy(comm);
    bool main_like = (name.find("unitymain") != std::string::npos ||
                      name.find("gamethread") != std::string::npos);
    bool render_like = is_render_thread_name(comm);
    if (!main_like && !render_like) return false;

    float min_usage = main_like ? 1.0f : 3.0f;
    if (cpu_usage_pct < min_usage) {
        return false;
    }

    int hotter_same_name = 0;
    for (auto& kv : g_threads) {
        const ThreadRuntimeState& other = kv.second;
        if (other.tid == tid) continue;
        if (scheduler_to_lower_copy(other.comm.c_str()) != name) continue;
        if (other.cpu_usage_pct > cpu_usage_pct + 0.5f) {
            hotter_same_name++;
        }
    }

    int keep_top_n = 1;
    if (!main_like && (name.find("renderthread") != std::string::npos || name.find("glthread") != std::string::npos)) {
        keep_top_n = 2;
    }
    return hotter_same_name < keep_top_n;
}

static bool is_numeric_name(const char* s) {
    if (!s || !s[0]) return false;
    for (int i = 0; s[i]; i++) {
        if (!isdigit((unsigned char)s[i])) return false;
    }
    return true;
}

static bool read_process_package_name(pid_t pid, char* out, size_t out_size) {
    if (!out || out_size == 0) return false;
    out[0] = '\0';
    if (pid <= 0) return false;

    char path[256];
    char cmdline[256] = {0};
    safe_snprintf(path, sizeof(path), "/proc/%d/cmdline", pid);
    if (!read_text_file(path, cmdline, sizeof(cmdline)) || !cmdline[0]) {
        return false;
    }

    std::string trimmed = cmdline;
    trim_process_suffix(&trimmed[0]);
    trimmed.resize(strlen(trimmed.c_str()));
    if (trimmed.empty()) {
        return false;
    }
    safe_strncpy(out, trimmed.c_str(), out_size);
    return true;
}

static pid_t get_thread_owner_pid_locked(pid_t tid) {
    auto it = g_threads.find((int)tid);
    if (it == g_threads.end()) return -1;
    return it->second.pid;
}

static bool matches_any_pattern_lowered(const std::string& lowered_text, const std::vector<std::string>& patterns) {
    if (lowered_text.empty()) return false;
    for (size_t i = 0; i < patterns.size(); i++) {
        if (!patterns[i].empty() && lowered_text.find(patterns[i]) != std::string::npos) {
            return true;
        }
    }
    return false;
}

static bool matches_any_pattern(const char* text, const std::vector<std::string>& patterns) {
    return matches_any_pattern_lowered(scheduler_to_lower_copy(text), patterns);
}

static int score_process_threads_for_scheduler(pid_t pid) {
    if (pid <= 0) return -1;

    char task_path[128];
    safe_snprintf(task_path, sizeof(task_path), "/proc/%d/task", pid);
    DIR* dir = opendir(task_path);
    if (!dir) return -1;

    int score = 0;
    struct dirent* entry = NULL;
    while ((entry = readdir(dir)) != NULL) {
        if (!is_numeric_name(entry->d_name)) continue;
        pid_t tid = (pid_t)atoi(entry->d_name);
        if (tid <= 0) continue;

        char comm_path[160];
        char comm[128] = {0};
        safe_snprintf(comm_path, sizeof(comm_path), "/proc/%d/task/%d/comm", pid, tid);
        if (!read_text_file(comm_path, comm, sizeof(comm))) continue;

        std::string name = scheduler_to_lower_copy(comm);
        if (matches_any_pattern_lowered(name, g_thread_signature_config.main_thread_patterns) ||
            name.find("unitymain") != std::string::npos || name.find("gamethread") != std::string::npos) {
            score += 100;
        } else if (matches_any_pattern_lowered(name, g_thread_signature_config.render_thread_patterns) ||
                   name.find("unitygfx") != std::string::npos || name.find("renderthread") != std::string::npos ||
                   name.find("glthread") != std::string::npos || name.find("gfxdevice") != std::string::npos) {
            score += 80;
        } else if (matches_any_pattern_lowered(name, g_thread_signature_config.worker_thread_patterns) ||
                   matches_any_pattern_lowered(name, g_thread_signature_config.taskgraph_thread_patterns) ||
                   name.find("thread-") != std::string::npos || name.find("worker") != std::string::npos ||
                   name.find("task") != std::string::npos) {
            score += 10;
        } else {
            score += 1;
        }
    }
    closedir(dir);
    return score;
}

static bool read_text_file(const char* path, char* buf, size_t buf_size) {
    if (!path || !buf || buf_size == 0) return false;
    FILE* fp = fopen(path, "r");
    if (!fp) return false;
    size_t n = fread(buf, 1, buf_size - 1, fp);
    fclose(fp);
    if (n == 0) return false;
    buf[n] = '\0';
    return true;
}

static void log_bpf_error_once(const char* prefix, int err_code) {
    long long now_ms = get_current_time_ms();
    if (now_ms - g_last_bpf_poll_error_ms < 1000) return;
    g_last_bpf_poll_error_ms = now_ms;
    char msg[256];
    safe_snprintf(msg, sizeof(msg), "%s: %d", prefix, err_code);
    log_message(msg);
}

static void ensure_thread_state_for_tid_locked(pid_t pid, const char* package, pid_t tid, const char* comm_hint) {
    if (tid <= 0 || pid <= 0) return;
    ThreadRuntimeState& state = g_threads[tid];
    if (state.tid == 0) {
        state.pid = pid;
        state.tid = tid;
        state.package = package ? package : "";
        state.discovered_ms = get_current_time_ms();
        state.last_ticks = 0;
        state.last_sample_ms = get_current_time_ms();
        state.pending_runtime_ns = 0;
        state.total_runtime_ns = 0;
        state.cpu_usage_pct = 0.0f;
        state.cls = THREAD_CLASS_BACKGROUND;
        state.assigned_cluster = -1;
        state.last_classify_ms = 0;
        state.last_migrate_ms = 0;
        state.assigned_cpus.clear();
        state.last_target_cpus.clear();
        state.frozen_cpus.clear();
        state.last_target_cluster = -1;
        state.frozen_cluster = -1;
        state.hot_sample_count = 0;
        state.first_hot_ms = 0;
        state.target_stable_since_ms = 0;
        state.frozen_at_ms = 0;
        state.frozen = false;
        state.sched_baseline_captured = false;
        state.baseline_nice = 0;
        state.baseline_sched_policy = SCHED_OTHER;
        state.baseline_sched_priority = 0;
        state.applied_nice = 0;
        state.applied_sched_policy = SCHED_OTHER;
        state.applied_sched_priority = 0;
    }
    state.pid = pid;
    if (package && package[0]) {
        state.package = package;
    }
    if (comm_hint && comm_hint[0]) {
        state.comm = comm_hint;
    }
}

static void apply_runtime_ns_locked(ThreadRuntimeState& state, unsigned long long runtime_ns, long long now_ms) {
    if (runtime_ns == 0) return;
    long long delta_ms = now_ms - state.last_sample_ms;
    if (delta_ms <= 0) delta_ms = 100;
    double runtime_ms = (double)runtime_ns / 1000000.0;
    state.cpu_usage_pct = (float)((runtime_ms / (double)delta_ms) * 100.0);
    if (state.cpu_usage_pct < 0.0f) state.cpu_usage_pct = 0.0f;
    if (state.cpu_usage_pct > 400.0f) state.cpu_usage_pct = 400.0f;
    state.pending_runtime_ns = runtime_ns;
    state.total_runtime_ns += runtime_ns;
    state.last_sample_ms = now_ms;
}

static bool is_render_thread_name(const char* comm) {
    std::string name = scheduler_to_lower_copy(comm);
    return matches_any_pattern_lowered(name, g_thread_signature_config.render_thread_patterns) ||
           name.find("render") != std::string::npos ||
           name.find("glthread") != std::string::npos ||
           name.find("unitygfx") != std::string::npos ||
           name.find("gfxdevice") != std::string::npos ||
           name.find("rhithread") != std::string::npos ||
           name.find("hwui") != std::string::npos;
}

static bool is_worker_thread_name(const char* comm) {
    std::string name = scheduler_to_lower_copy(comm);
    return matches_any_pattern_lowered(name, g_thread_signature_config.worker_thread_patterns) ||
           name.find("worker") != std::string::npos ||
           name.find("job.worker") != std::string::npos ||
           name.find("task") != std::string::npos;
}

static bool is_game_like_package_name(const char* package) {
    return package && package[0] && is_game_package_name(package);
}

static bool is_game_engine_priority_thread(const char* package, const char* comm, pid_t tid) {
    if (!package || !comm) return false;
    if (!is_game_like_package_name(package)) return false;
    if (is_main_thread_name_for_package(package, comm, tid)) return true;

    std::string name = scheduler_to_lower_copy(comm);
    return matches_any_pattern_lowered(name, g_thread_signature_config.game_priority_thread_patterns) ||
           name.find("unitygfx") != std::string::npos ||
           name.find("gfxdevice") != std::string::npos ||
           name.find("renderthread") != std::string::npos ||
           name.find("glthread") != std::string::npos ||
           name.find("rhithread") != std::string::npos;
}

static bool should_treat_as_priority_game_thread(const char* package, const char* comm, pid_t tid, float cpu_usage_pct) {
    if (!is_game_engine_priority_thread(package, comm, tid)) return false;
    if (is_main_thread_name_for_package(package, comm, tid)) {
        return cpu_usage_pct >= 1.0f;
    }
    return cpu_usage_pct >= 3.0f;
}

static bool is_scheduler_session_foreground_locked() {
    return g_scheduler_session.phase == SESSION_TRACKING &&
           g_scheduler_session.pid > 0 &&
           g_scheduler_session.package[0] &&
           g_foreground_pid == g_scheduler_session.pid &&
           strcmp(g_scheduler_session.package, g_foreground_package) == 0;
}

static char* get_latched_primary_loss_reason_buffer_locked(pid_t* lost_tid, ThreadClass role) {
    if (role == THREAD_CLASS_MAIN) {
        if (lost_tid) *lost_tid = g_scheduler_session.last_lost_main_tid;
        return g_scheduler_session.latched_main_loss_reason;
    }
    if (role == THREAD_CLASS_RENDER) {
        if (lost_tid) *lost_tid = g_scheduler_session.last_lost_render_tid;
        return g_scheduler_session.latched_render_loss_reason;
    }
    if (lost_tid) *lost_tid = 0;
    return NULL;
}

static void set_latched_primary_loss_reason_locked(ThreadClass role, pid_t lost_tid, const char* reason) {
    pid_t ignored_tid = 0;
    char* buffer = get_latched_primary_loss_reason_buffer_locked(&ignored_tid, role);
    if (!buffer) return;
    if (role == THREAD_CLASS_MAIN) {
        g_scheduler_session.last_lost_main_tid = lost_tid;
    } else if (role == THREAD_CLASS_RENDER) {
        g_scheduler_session.last_lost_render_tid = lost_tid;
    }
    if (reason && reason[0]) {
        safe_strncpy(buffer, reason, 32);
    } else {
        buffer[0] = '\0';
    }
}

static const char* get_latched_primary_loss_reason_locked(ThreadClass role, pid_t* out_lost_tid) {
    pid_t lost_tid = 0;
    char* buffer = get_latched_primary_loss_reason_buffer_locked(&lost_tid, role);
    if (out_lost_tid) *out_lost_tid = lost_tid;
    if (!buffer || !buffer[0]) return NULL;
    return buffer;
}

static void ensure_scheduler_session_tracking_locked() {
    if (g_foreground_pid <= 0 || !g_foreground_package[0] || strcmp(g_foreground_package, "unknown") == 0) {
        return;
    }
    if (g_scheduler_session.phase == SESSION_TRACKING &&
        g_scheduler_session.pid == g_foreground_pid &&
        strcmp(g_scheduler_session.package, g_foreground_package) == 0) {
        return;
    }
    g_scheduler_session.phase = SESSION_TRACKING;
    g_scheduler_session.pid = g_foreground_pid;
    safe_strncpy(g_scheduler_session.package, g_foreground_package, sizeof(g_scheduler_session.package));
}

static bool is_latched_session_tid_alive_locked(pid_t tid) {
    if (tid <= 0) return false;
    auto it = g_threads.find((int)tid);
    if (it == g_threads.end()) return false;
    const ThreadRuntimeState& state = it->second;
    return state.pid == g_scheduler_session.pid &&
           !state.package.empty() &&
           strcmp(state.package.c_str(), g_scheduler_session.package) == 0;
}

static void clear_invalid_latched_primary_tids_locked() {
    if (g_scheduler_session.latched_main_tid > 0 &&
        !is_latched_session_tid_alive_locked(g_scheduler_session.latched_main_tid)) {
        clear_latched_primary_tid_locked(g_scheduler_session.latched_main_tid, "tid_not_alive");
    }
    if (g_scheduler_session.latched_render_tid > 0 &&
        !is_latched_session_tid_alive_locked(g_scheduler_session.latched_render_tid)) {
        clear_latched_primary_tid_locked(g_scheduler_session.latched_render_tid, "tid_not_alive");
    }
}

static void clear_latched_primary_tid_locked(pid_t tid, const char* reason) {
    if (tid <= 0) return;
    const char* role = NULL;
    ThreadClass role_class = THREAD_CLASS_BACKGROUND;
    if (g_scheduler_session.latched_main_tid == tid) {
        g_scheduler_session.latched_main_tid = 0;
        role = "main";
        role_class = THREAD_CLASS_MAIN;
    } else if (g_scheduler_session.latched_render_tid == tid) {
        g_scheduler_session.latched_render_tid = 0;
        role = "render";
        role_class = THREAD_CLASS_RENDER;
    }
    if (!role) return;
    set_latched_primary_loss_reason_locked(role_class, tid, reason);
    char msg[256];
    safe_snprintf(msg, sizeof(msg),
                  "锁定线程失效 | role:%s | app:%s | pid:%d | tid:%d | reason:%s",
                  role,
                  g_scheduler_session.package[0] ? g_scheduler_session.package : "unknown",
                  (int)g_scheduler_session.pid,
                  (int)tid,
                  reason && reason[0] ? reason : "unknown");
    log_affinity_message(msg);
}

static void maybe_latch_primary_thread_tid_locked(const ThreadRuntimeState& state) {
    if (state.pid != g_foreground_pid) return;
    if (state.package.empty() || strcmp(state.package.c_str(), g_foreground_package) != 0) return;
    ensure_scheduler_session_tracking_locked();
    clear_invalid_latched_primary_tids_locked();

    bool is_main = is_main_thread_name_for_package(g_foreground_package, state.comm.c_str(), state.tid);
    bool is_render = !is_main &&
                     is_game_engine_priority_thread(g_foreground_package, state.comm.c_str(), state.tid);
    pid_t* slot = NULL;
    const char* role = NULL;
    if (is_main) {
        slot = &g_scheduler_session.latched_main_tid;
        role = "main";
    } else if (is_render) {
        slot = &g_scheduler_session.latched_render_tid;
        role = "render";
    }
    if (!slot || *slot > 0) return;

    *slot = state.tid;
    ThreadClass role_class = is_main ? THREAD_CLASS_MAIN : THREAD_CLASS_RENDER;
    pid_t old_tid = 0;
    const char* loss_reason = get_latched_primary_loss_reason_locked(role_class, &old_tid);
    char msg[320];
    if (loss_reason && old_tid > 0) {
        safe_snprintf(msg, sizeof(msg),
                      "锁定线程补选 | role:%s | app:%s | pid:%d | old_tid:%d | new_tid:%d | reason:%s | comm:%s",
                      role,
                      g_scheduler_session.package,
                      (int)g_scheduler_session.pid,
                      (int)old_tid,
                      (int)state.tid,
                      loss_reason,
                      state.comm.c_str());
    } else {
        safe_snprintf(msg, sizeof(msg),
                      "锁定线程绑定 | role:%s | app:%s | pid:%d | tid:%d | comm:%s",
                      role,
                      g_scheduler_session.package,
                      (int)g_scheduler_session.pid,
                      (int)state.tid,
                      state.comm.c_str());
    }
    set_latched_primary_loss_reason_locked(role_class, 0, "");
    log_affinity_message(msg);
}

static bool should_promote_generic_worker(const char* package, const char* comm, float cpu_usage_pct) {
    if (!comm) return false;
    std::string name = scheduler_to_lower_copy(comm);
    if (matches_any_pattern_lowered(name, g_thread_signature_config.worker_exclude_patterns)) {
        return false;
    }
    if (is_game_like_package_name(package)) {
        if (name.find("asynctask") != std::string::npos ||
            name.find("hwuitask") != std::string::npos ||
            name.find("hwui") != std::string::npos) {
            return false;
        }
    }
    return is_worker_thread_name(comm) && cpu_usage_pct >= 8.0f;
}

static bool is_download_thread_name(const char* package, const char* comm, float cpu_usage_pct) {
    if (!comm) return false;
    std::string name = scheduler_to_lower_copy(comm);
    if (matches_any_pattern_lowered(name, g_thread_signature_config.worker_thread_patterns)) {
        if (name.find("download") != std::string::npos || name.find("transfer") != std::string::npos) {
            return true;
        }
    }
    return name.find("downloader") != std::string::npos || name.find("cdn-") != std::string::npos;
}

static bool is_thread_dash_name(const char* comm) {
    if (!comm || !comm[0]) return false;
    std::string name = scheduler_to_lower_copy(comm);
    return name.rfind("thread-", 0) == 0;
}

static bool is_taskgraph_thread_name(const char* comm) {
    if (!comm || !comm[0]) return false;
    std::string name = scheduler_to_lower_copy(comm);
    return matches_any_pattern_lowered(name, g_thread_signature_config.taskgraph_thread_patterns) ||
           name.find("taskgraphnp") != std::string::npos;
}

static bool is_unity_gfx_thread_name(const char* comm) {
    if (!comm || !comm[0]) return false;
    std::string name = scheduler_to_lower_copy(comm);
    return name.find("unitygfx") != std::string::npos ||
           name.find("gfxdevice") != std::string::npos;
}

static bool is_rhi_thread_name(const char* comm) {
    if (!comm || !comm[0]) return false;
    std::string name = scheduler_to_lower_copy(comm);
    return name.find("rhithread") != std::string::npos;
}

static bool is_single_big_core_game_package(const char* package) {
    if (matches_any_pattern(package, g_thread_signature_config.single_big_core_package_patterns)) {
        return true;
    }
    return package && strcmp(package, "com.tencent.tmgp.sgame") == 0;
}

static bool is_dual_big_core_game_package(const char* package) {
    if (!package || !package[0]) return false;
    std::string name = scheduler_to_lower_copy(package);
    return matches_any_pattern_lowered(name, g_thread_signature_config.dual_big_core_package_patterns) ||
           name.find("genshin") != std::string::npos ||
           name.find("yuanshen") != std::string::npos ||
           name.find("cfm") != std::string::npos ||
           name.find(".cf") != std::string::npos;
}

static bool is_ue_game_package_name(const char* package) {
    if (!package || !package[0]) return false;
    std::string name = scheduler_to_lower_copy(package);
    return matches_any_pattern_lowered(name, g_thread_signature_config.ue_game_package_patterns) ||
           name == "com.tencent.tmgp.pubgmhd" ||
           name == "com.tencent.ig" ||
           name.find("delta") != std::string::npos ||
           name.find("unreal") != std::string::npos;
}

static float get_thread_usage_locked(pid_t tid) {
    auto it = g_threads.find(tid);
    if (it == g_threads.end()) return 0.0f;
    return it->second.cpu_usage_pct;
}

static int get_thread_dash_rank_locked(pid_t tid, const char* comm, float cpu_usage_pct) {
    if (!is_thread_dash_name(comm)) return -1;
    int hotter = 0;
    for (auto& kv : g_threads) {
        const ThreadRuntimeState& other = kv.second;
        if (other.tid == tid) continue;
        if (!is_thread_dash_name(other.comm.c_str())) continue;
        if (other.cpu_usage_pct > cpu_usage_pct + 0.5f) {
            hotter++;
        }
    }
    return hotter;
}

static int get_worker_rank_locked(pid_t tid, const char* comm, float cpu_usage_pct) {
    if (!is_worker_thread_name(comm) && !is_taskgraph_thread_name(comm)) return -1;
    int hotter = 0;
    for (auto& kv : g_threads) {
        const ThreadRuntimeState& other = kv.second;
        if (other.tid == tid) continue;
        if (!is_worker_thread_name(other.comm.c_str()) && !is_taskgraph_thread_name(other.comm.c_str())) continue;
        if (other.cpu_usage_pct > cpu_usage_pct + 0.5f) {
            hotter++;
        }
    }
    return hotter;
}

static bool is_primary_render_like_thread_name(const char* comm) {
    return is_render_thread_name(comm) || is_unity_gfx_thread_name(comm) || is_rhi_thread_name(comm);
}

static int get_non_primary_thread_rank_locked(const char* package, pid_t tid, const char* comm, float cpu_usage_pct) {
    if (is_main_thread_name_for_package(package, comm, tid) || is_primary_render_like_thread_name(comm)) {
        return -1;
    }
    int hotter = 0;
    for (auto& kv : g_threads) {
        const ThreadRuntimeState& other = kv.second;
        if (other.tid == tid) continue;
        if (is_main_thread_name_for_package(package, other.comm.c_str(), other.tid) ||
            is_primary_render_like_thread_name(other.comm.c_str())) {
            continue;
        }
        if (other.cpu_usage_pct > cpu_usage_pct + 0.5f) {
            hotter++;
        }
    }
    return hotter;
}

static int get_other_primary_single_big_cpu_locked(const char* package,
                                                   pid_t tid,
                                                   bool check_render_thread,
                                                   bool check_main_thread) {
    for (auto& kv : g_threads) {
        const ThreadRuntimeState& other = kv.second;
        if (other.tid == tid) continue;
        bool other_is_main = is_main_thread_name_for_package(package, other.comm.c_str(), other.tid);
        bool other_is_render = is_primary_render_like_thread_name(other.comm.c_str());
        if (check_render_thread && !other_is_render) continue;
        if (check_main_thread && !other_is_main) continue;
        const std::vector<int>* cpus = NULL;
        if (other.frozen && !other.frozen_cpus.empty()) {
            cpus = &other.frozen_cpus;
        } else if (!other.assigned_cpus.empty()) {
            cpus = &other.assigned_cpus;
        }
        if (!cpus || cpus->size() != 1) continue;
        int cpu = (*cpus)[0];
        if (cpu >= 6) {
            return cpu;
        }
    }
    return -1;
}

static void normalize_cpu_mask(std::vector<int>& cpus) {
    cpus.erase(std::remove_if(cpus.begin(), cpus.end(), [](int cpu) {
        return cpu < 0 || cpu >= CPU_SETSIZE;
    }), cpus.end());
    std::sort(cpus.begin(), cpus.end());
    cpus.erase(std::unique(cpus.begin(), cpus.end()), cpus.end());
}

static bool cpu_mask_equals(const std::vector<int>& a, const std::vector<int>& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); i++) {
        if (a[i] != b[i]) return false;
    }
    return true;
}

static bool is_thread_affinity_mask_matching_locked(pid_t tid, const std::vector<int>& expected_cpus) {
    if (tid <= 0 || expected_cpus.empty()) return false;
    cpu_set_t expected_mask;
    cpu_set_t actual_mask;
    CPU_ZERO(&expected_mask);
    CPU_ZERO(&actual_mask);
    for (size_t i = 0; i < expected_cpus.size(); i++) {
        if (expected_cpus[i] >= 0 && expected_cpus[i] < CPU_SETSIZE) {
            CPU_SET(expected_cpus[i], &expected_mask);
        }
    }
    if (sched_getaffinity(tid, sizeof(actual_mask), &actual_mask) != 0) return false;
    for (int cpu = 0; cpu < CPU_SETSIZE; cpu++) {
        bool expected = CPU_ISSET(cpu, &expected_mask) != 0;
        bool actual = CPU_ISSET(cpu, &actual_mask) != 0;
        if (expected != actual) return false;
    }
    return true;
}

[[maybe_unused]] static bool is_thread_freeze_candidate_locked(pid_t tid, const char* comm, float cpu_usage_pct) {
    if (!comm || !comm[0]) return false;
    if (!is_game_like_package_name(g_foreground_package)) return false;
    if (is_main_thread_name_for_package(g_foreground_package, comm, tid)) {
        return cpu_usage_pct >= 1.0f;
    }
    if (is_render_thread_name(comm) || is_hot_priority_thread_locked(tid, comm, cpu_usage_pct)) {
        return cpu_usage_pct >= 2.0f;
    }
    int worker_rank = get_worker_rank_locked(tid, comm, cpu_usage_pct);
    if (worker_rank >= 0 && worker_rank < 5 && cpu_usage_pct >= 2.0f) {
        return true;
    }
    int dash_rank = get_thread_dash_rank_locked(tid, comm, cpu_usage_pct);
    return dash_rank >= 0 && dash_rank < 5 && cpu_usage_pct >= 2.0f;
}

[[maybe_unused]] static void update_thread_target_stability_locked(
    ThreadRuntimeState& state,
    int target_cluster,
    const std::vector<int>& explicit_cpus,
    long long now_ms) {
    bool changed = (state.last_target_cluster != target_cluster) ||
                   !cpu_mask_equals(state.last_target_cpus, explicit_cpus);
    if (changed || state.target_stable_since_ms == 0) {
        state.last_target_cluster = target_cluster;
        state.last_target_cpus = explicit_cpus;
        state.target_stable_since_ms = now_ms;
    }
}

[[maybe_unused]] static bool should_freeze_thread_locked(
    const ThreadRuntimeState& state,
    int target_cluster,
    const std::vector<int>& explicit_cpus,
    long long now_ms) {
    if (state.frozen) return false;
    if (state.assigned_cluster < 0) return false;
    if (state.assigned_cluster != target_cluster) return false;
    if (!cpu_mask_equals(state.assigned_cpus, explicit_cpus)) return false;
    if (state.first_hot_ms == 0 || state.target_stable_since_ms == 0) return false;
    if (state.hot_sample_count < kThreadFreezeMinSamples) return false;
    if (now_ms - state.first_hot_ms < kThreadFreezeLearnMs) return false;
    if (now_ms - state.target_stable_since_ms < kThreadFreezeStableMs) return false;
    return true;
}

static bool is_pubgm_main_thread_name(const char* package, const char* comm, pid_t tid) {
    if (!package || !comm) return false;
    if (strcmp(package, "com.tencent.tmgp.pubgmhd") != 0 && strcmp(package, "com.tencent.ig") != 0) {
        return false;
    }
    std::string name = scheduler_to_lower_copy(comm);
    if (name.rfind("thread-", 0) != 0) return false;
    int thread_no = atoi(name.c_str() + 7);
    if (thread_no < 0 || thread_no > 20) return false;
    float usage = get_thread_usage_locked(tid);
    int rank = get_thread_dash_rank_locked(tid, comm, usage);
    return rank == 0;
}

static bool is_main_thread_name_for_package(const char* package, const char* comm, pid_t tid) {
    std::string name = scheduler_to_lower_copy(comm);
    pid_t owner_pid = get_thread_owner_pid_locked(tid);
    if (owner_pid > 0 && tid == owner_pid) return true;
    if (matches_any_pattern_lowered(name, g_thread_signature_config.main_thread_patterns) ||
        name.find("unitymain") != std::string::npos ||
        name == "gamethread" ||
        name.find(" main") != std::string::npos) {
        return true;
    }
    if (is_pubgm_main_thread_name(package, comm, tid)) return true;
    return false;
}

enum TopologyKind {
    TOPOLOGY_GENERIC = 0,
    TOPOLOGY_2_3_2_1,
    TOPOLOGY_3_4_1,
    TOPOLOGY_4_3_1,
    TOPOLOGY_6_2
};

static TopologyKind get_topology_kind() {
    if (cluster_count == 4 &&
        cpu_clusters[0].cpu_count == 2 &&
        cpu_clusters[1].cpu_count == 3 &&
        cpu_clusters[2].cpu_count == 2 &&
        cpu_clusters[3].cpu_count == 1) {
        return TOPOLOGY_2_3_2_1;
    }
    if (cluster_count == 3 &&
        cpu_clusters[0].cpu_count == 3 &&
        cpu_clusters[1].cpu_count == 4 &&
        cpu_clusters[2].cpu_count == 1) {
        return TOPOLOGY_3_4_1;
    }
    if (cluster_count == 3 &&
        cpu_clusters[0].cpu_count == 4 &&
        cpu_clusters[1].cpu_count == 3 &&
        cpu_clusters[2].cpu_count == 1) {
        return TOPOLOGY_4_3_1;
    }
    if (cluster_count == 2 &&
        cpu_clusters[0].cpu_count == 6 &&
        cpu_clusters[1].cpu_count == 2) {
        return TOPOLOGY_6_2;
    }
    return TOPOLOGY_GENERIC;
}

static void append_cluster_mask(std::vector<int>& cpus, int cluster_id, bool skip_cpu0, bool drop_last_cpu) {
    if (cluster_id < 0 || cluster_id >= cluster_count) return;
    int limit = cpu_clusters[cluster_id].cpu_count;
    if (drop_last_cpu && limit > 1) {
        limit -= 1;
    }
    for (int i = 0; i < limit; i++) {
        int cpu = cpu_clusters[cluster_id].cpu_ids[i];
        if (skip_cpu0 && cpu == 0) continue;
        cpus.push_back(cpu);
    }
}

static int get_prime_cluster_id() {
    if (cluster_count <= 0) return -1;
    int candidate = cluster_count - 1;
    if (cluster_count >= 3 && cpu_clusters[candidate].cpu_count == 1) {
        return candidate;
    }
    return -1;
}

static int parse_cluster_tag_to_id(const std::string& tag) {
    if (tag.empty()) return -1;
    if (tag == "little" || tag == "small") {
        return cluster_count > 0 ? 0 : -1;
    }
    if (tag == "mid" || tag == "middle" || tag == "medium") {
        if (cluster_count <= 1) return 0;
        if (cluster_count >= 4) return 1;
        if (cluster_count == 3) return 1;
        return 0;
    }
    if (tag == "big") {
        if (cluster_count <= 1) return 0;
        int prime_cluster = get_prime_cluster_id();
        if (prime_cluster > 0) {
            return prime_cluster - 1;
        }
        return cluster_count - 1;
    }
    if (tag == "prime") {
        int prime_cluster = get_prime_cluster_id();
        return prime_cluster >= 0 ? prime_cluster : (cluster_count > 0 ? cluster_count - 1 : -1);
    }
    if (tag == "top") {
        return cluster_count > 0 ? cluster_count - 1 : -1;
    }
    if (tag.rfind("cluster", 0) == 0) {
        int id = atoi(tag.c_str() + 7);
        return (id >= 0 && id < cluster_count) ? id : -1;
    }
    if (tag.size() >= 2 && tag[0] == 'c' && isdigit((unsigned char)tag[1])) {
        int id = atoi(tag.c_str() + 1);
        return (id >= 0 && id < cluster_count) ? id : -1;
    }
    bool numeric = true;
    for (size_t i = 0; i < tag.size(); i++) {
        if (!isdigit((unsigned char)tag[i])) {
            numeric = false;
            break;
        }
    }
    if (numeric) {
        int id = atoi(tag.c_str());
        return (id >= 0 && id < cluster_count) ? id : -1;
    }
    return -1;
}

static void append_named_cluster_mask(std::vector<int>& cpus, const std::string& tag) {
    if (tag.empty() || cluster_count <= 0) return;
    if (tag == "all") {
        for (int cluster = 0; cluster < cluster_count; cluster++) {
            append_cluster_mask(cpus, cluster, false, false);
        }
        return;
    }
    int cluster_id = parse_cluster_tag_to_id(tag);
    if (cluster_id >= 0) {
        append_cluster_mask(cpus, cluster_id, false, false);
    }
}

static void resolve_thread_json_action_cpus_locked(
    const ThreadJsonAction& action,
    std::vector<int>& cpus) {
    cpus.clear();
    if (action.has_clusters) {
        for (size_t i = 0; i < action.clusters.size(); i++) {
            append_named_cluster_mask(cpus, action.clusters[i]);
        }
    }
    if (cpus.empty() && action.has_cpus) {
        cpus = action.cpus;
    }
    normalize_cpu_mask(cpus);
}

static int highest_cluster_for_mask(const std::vector<int>& cpus) {
    int best_cluster = 0;
    for (size_t i = 0; i < cpus.size(); i++) {
        for (int c = 0; c < cluster_count; c++) {
            for (int j = 0; j < cpu_clusters[c].cpu_count; j++) {
                if (cpu_clusters[c].cpu_ids[j] == cpus[i] && c > best_cluster) {
                    best_cluster = c;
                }
            }
        }
    }
    return best_cluster;
}

static bool cpu_exists_in_topology(int cpu) {
    for (int c = 0; c < cluster_count; c++) {
        for (int i = 0; i < cpu_clusters[c].cpu_count; i++) {
            if (cpu_clusters[c].cpu_ids[i] == cpu) return true;
        }
    }
    return false;
}

static int get_cluster_id_for_cpu(int cpu) {
    for (int c = 0; c < cluster_count; c++) {
        for (int i = 0; i < cpu_clusters[c].cpu_count; i++) {
            if (cpu_clusters[c].cpu_ids[i] == cpu) return c;
        }
    }
    return -1;
}

static bool has_primary_render_companion_for_package_locked(const char* package, pid_t exclude_tid) {
    if (!package || !package[0]) return false;
    for (auto& kv : g_threads) {
        const ThreadRuntimeState& other = kv.second;
        if (other.tid == exclude_tid) continue;
        if (other.package.empty() || strcmp(other.package.c_str(), package) != 0) continue;
        if (!is_primary_render_like_thread_name(other.comm.c_str())) continue;
        return true;
    }
    return false;
}

static bool build_main_spawn_prewarm_mask_locked(int single_cpu, std::vector<int>& cpus) {
    cpus.clear();
    int cluster_id = get_cluster_id_for_cpu(single_cpu);
    if (cluster_id < 0) return false;
    append_cluster_mask(cpus, cluster_id, false, false);
    if (cpus.size() <= 1 && cluster_id > 0) {
        append_cluster_mask(cpus, cluster_id - 1, false, false);
    }
    normalize_cpu_mask(cpus);
    return cpus.size() > 1;
}

static bool maybe_expand_main_single_core_mask_for_spawn_locked(
    ThreadRuntimeState& state,
    const ThreadJsonResolvedRule* matched_rule,
    long long now_ms,
    std::vector<int>& explicit_cpus,
    int& explicit_cluster) {
    static const long long kMainSpawnPrewarmMs = 3000;
    if (!matched_rule || matched_rule->role_kind != THREAD_JSON_ROLE_MAIN) return false;
    if (matched_rule->source_kind != THREAD_JSON_RULE_SOURCE_DEFAULT &&
        matched_rule->source_kind != THREAD_JSON_RULE_SOURCE_SCENE_DEFAULT) {
        return false;
    }
    if (explicit_cpus.size() != 1) return false;
    if (state.pid <= 0 || state.pid != g_foreground_pid) return false;
    if (state.package.empty() || strcmp(state.package.c_str(), g_foreground_package) != 0) return false;
    if (!is_game_package_name(state.package.c_str())) return false;
    if (state.discovered_ms <= 0 || now_ms - state.discovered_ms > kMainSpawnPrewarmMs) return false;
    if (has_primary_render_companion_for_package_locked(state.package.c_str(), state.tid)) return false;

    std::vector<int> prewarm_cpus;
    if (!build_main_spawn_prewarm_mask_locked(explicit_cpus[0], prewarm_cpus)) return false;
    explicit_cpus = prewarm_cpus;
    explicit_cluster = highest_cluster_for_mask(explicit_cpus);
    return true;
}

static void append_cpu_if_present(std::vector<int>& cpus, int cpu) {
    if (!cpu_exists_in_topology(cpu)) return;
    for (size_t i = 0; i < cpus.size(); i++) {
        if (cpus[i] == cpu) return;
    }
    cpus.push_back(cpu);
}

static void append_cpu_range_if_present(std::vector<int>& cpus, int start_cpu, int end_cpu) {
    if (end_cpu < start_cpu) return;
    for (int cpu = start_cpu; cpu <= end_cpu; cpu++) {
        append_cpu_if_present(cpus, cpu);
    }
}

static bool thread_json_match_pattern_lowered(
    const std::string& lowered_text,
    const std::string& pattern,
    int match_kind) {
    if (lowered_text.empty() || pattern.empty()) return false;
    switch (match_kind) {
        case THREAD_JSON_MATCH_EXACT:
            return lowered_text == pattern;
        case THREAD_JSON_MATCH_PREFIX:
            return lowered_text.rfind(pattern, 0) == 0;
        case THREAD_JSON_MATCH_GLOB:
            return fnmatch(pattern.c_str(), lowered_text.c_str(), 0) == 0;
        case THREAD_JSON_MATCH_CONTAINS:
        default:
            return lowered_text.find(pattern) != std::string::npos;
    }
}

static bool thread_json_matches_any_pattern_lowered(
    const std::string& lowered_text,
    const std::vector<std::string>& patterns,
    int match_kind) {
    for (size_t i = 0; i < patterns.size(); i++) {
        if (thread_json_match_pattern_lowered(lowered_text, patterns[i], match_kind)) {
            return true;
        }
    }
    return false;
}

static bool thread_json_package_matches_rule(const char* package, const ThreadJsonAppRule& rule) {
    std::string lowered_pkg = scheduler_to_lower_copy(package);
    if (lowered_pkg.empty()) return false;
    for (size_t i = 0; i < rule.packages.size(); i++) {
        const std::string& item = rule.packages[i];
        if (item.empty()) continue;
        if (item.find('*') != std::string::npos || item.find('?') != std::string::npos) {
            if (fnmatch(item.c_str(), lowered_pkg.c_str(), 0) == 0) {
                return true;
            }
            continue;
        }
        // Package rules should not use implicit substring matching, otherwise
        // shorter ids like com.tencent.tmgp.cod will hijack com.tencent.tmgp.codev.
        if (lowered_pkg == item) {
            return true;
        }
    }
    return false;
}

static const ThreadJsonAppRule* find_persistent_thread_app_rule_for_package_locked(const char* package) {
    if (!package || !package[0]) return NULL;
    for (size_t i = 0; i < g_thread_app_rules.size(); i++) {
        const ThreadJsonAppRule& rule = g_thread_app_rules[i];
        if (!rule.persistent_scope) continue;
        if (thread_json_package_matches_rule(package, rule)) {
            return &rule;
        }
    }
    return NULL;
}

static int find_thread_app_rule_index_for_package_locked(const char* package) {
    for (size_t i = 0; i < g_thread_app_rules.size(); i++) {
        if (thread_json_package_matches_rule(package, g_thread_app_rules[i])) {
            return (int)i;
        }
    }
    return -1;
}

static const ThreadJsonAppRule* find_thread_app_rule_for_package_locked(const char* package) {
    int index = find_thread_app_rule_index_for_package_locked(package);
    if (index < 0 || index >= (int)g_thread_app_rules.size()) return NULL;
    return &g_thread_app_rules[(size_t)index];
}

static bool thread_json_app_rule_has_explicit_affinity_locked(const ThreadJsonAppRule& app_rule);

static void refresh_active_thread_app_rule_locked(const char* package) {
    g_active_thread_app_rule_index = find_thread_app_rule_index_for_package_locked(package);
    g_foreground_explicit_thread_rule_active = false;
    if (!package || !package[0] || strcmp(package, "unknown") == 0) return;
    if (g_active_thread_app_rule_index < 0 ||
        g_active_thread_app_rule_index >= (int)g_thread_app_rules.size()) return;
    g_foreground_explicit_thread_rule_active = thread_json_app_rule_has_explicit_affinity_locked(
        g_thread_app_rules[(size_t)g_active_thread_app_rule_index]);
}

static bool thread_json_action_has_explicit_affinity_locked(const ThreadJsonAction& action) {
    return (action.has_cpus && !action.cpus.empty()) ||
           (action.has_clusters && !action.clusters.empty());
}

static bool thread_json_app_rule_has_explicit_affinity_locked(const ThreadJsonAppRule& app_rule) {
    if (app_rule.has_default_action &&
        thread_json_action_has_explicit_affinity_locked(app_rule.default_action)) {
        return true;
    }
    for (int i = 0; i < THREAD_JSON_ROLE_COUNT; i++) {
        const ThreadJsonRoleRule& role_rule = app_rule.roles[i];
        if (!role_rule.enabled) continue;
        if (thread_json_action_has_explicit_affinity_locked(role_rule.action)) {
            return true;
        }
    }
    for (size_t i = 0; i < app_rule.custom_rules.size(); i++) {
        const ThreadJsonCustomRule& custom_rule = app_rule.custom_rules[i];
        if (!custom_rule.rule.enabled) continue;
        if (thread_json_action_has_explicit_affinity_locked(custom_rule.rule.action)) {
            return true;
        }
    }
    return false;
}

[[maybe_unused]] static bool active_thread_app_rule_uses_explicit_affinity_locked() {
    if (g_active_thread_app_rule_index < 0 ||
        g_active_thread_app_rule_index >= (int)g_thread_app_rules.size()) {
        return false;
    }
    return thread_json_app_rule_has_explicit_affinity_locked(
        g_thread_app_rules[(size_t)g_active_thread_app_rule_index]);
}

static bool thread_json_selector_basic_match_locked(
    const ThreadJsonSelector& selector,
    pid_t tid,
    const char* comm) {
    if (!comm || !comm[0]) return false;
    std::string lowered_name = scheduler_to_lower_copy(comm);
    if (!thread_json_matches_any_pattern_lowered(lowered_name, selector.patterns, selector.match_kind)) {
        return false;
    }
    return true;
}

static int thread_json_selector_rank_locked(
    const ThreadJsonSelector& selector,
    pid_t tid,
    const char* comm) {
    if (!thread_json_selector_basic_match_locked(selector, tid, comm)) {
        return -1;
    }
    auto target_it = g_threads.find((int)tid);
    if (target_it == g_threads.end()) {
        return 1;
    }
    const ThreadRuntimeState& target_state = target_it->second;
    std::vector<std::pair<float, pid_t>> ranked;
    for (auto& kv : g_threads) {
        const ThreadRuntimeState& other = kv.second;
        if (other.pid != target_state.pid) continue;
        if (other.package != target_state.package) continue;
        if (!thread_json_selector_basic_match_locked(selector, other.tid, other.comm.c_str())) {
            continue;
        }
        ranked.push_back(std::make_pair(other.cpu_usage_pct, other.tid));
    }
    std::sort(ranked.begin(), ranked.end(), [](const std::pair<float, pid_t>& a,
                                               const std::pair<float, pid_t>& b) {
        if (a.first != b.first) return a.first > b.first;
        return a.second < b.second;
    });
    for (size_t i = 0; i < ranked.size(); i++) {
        if (ranked[i].second == tid) {
            return (int)i + 1;
        }
    }
    return 1;
}

static bool thread_json_selector_rank_allowed(const ThreadJsonSelector& selector, int rank) {
    if (rank <= 0) return false;
    if (selector.cpu_rank > 0) {
        return rank == selector.cpu_rank;
    }
    if (selector.cpu_rank_start > 0 && selector.cpu_rank_end >= selector.cpu_rank_start) {
        return rank >= selector.cpu_rank_start && rank <= selector.cpu_rank_end;
    }
    return true;
}

static bool thread_json_role_selector_match_locked(
    const ThreadJsonRoleRule& rule,
    pid_t tid,
    const char* comm) {
    for (size_t i = 0; i < rule.selectors.size(); i++) {
        int rank = thread_json_selector_rank_locked(rule.selectors[i], tid, comm);
        if (thread_json_selector_rank_allowed(rule.selectors[i], rank)) {
            return true;
        }
    }
    return false;
}

static bool thread_json_role_limit_allowed_locked(
    const ThreadJsonRoleRule& rule,
    pid_t tid) {
    if (rule.limit <= 0) return true;
    auto target_it = g_threads.find((int)tid);
    if (target_it == g_threads.end()) return true;

    const ThreadRuntimeState& target_state = target_it->second;
    long long now_ms = get_current_time_ms();
    uint64_t rule_key = (uint64_t)(uintptr_t)&rule;
    SelectionEpochState& epoch = g_selection_epochs[rule_key];
    int loop_tick = g_runtime_stats.loop_ticks.load();
    bool computed_this_tick = (epoch.last_lookup_loop_tick == loop_tick);

    if (!computed_this_tick) {
        std::vector<std::pair<float, pid_t>> ranked;
        for (auto& kv : g_threads) {
            const ThreadRuntimeState& other = kv.second;
            if (other.pid != target_state.pid) continue;
            if (other.package != target_state.package) continue;
            if (!thread_json_role_selector_match_locked(rule, other.tid, other.comm.c_str())) continue;
            ranked.push_back(std::make_pair(other.cpu_usage_pct, other.tid));
        }
        std::sort(ranked.begin(), ranked.end(), [](const std::pair<float, pid_t>& a,
                                                   const std::pair<float, pid_t>& b) {
            if (a.first != b.first) return a.first > b.first;
            return a.second < b.second;
        });
        std::vector<pid_t> current_top;
        for (int i = 0; i < (int)ranked.size() && i < rule.limit; i++) {
            current_top.push_back(ranked[(size_t)i].second);
        }

        if (epoch.selected_tids.empty()) {
            epoch.epoch = 1;
            epoch.selected_tids = current_top;
            epoch.stable_since_ms = now_ms;
            epoch.last_reselect_ms = now_ms;
            epoch.selection_change_threshold_ms = rule.selection_change_threshold_ms > 0 ?
                rule.selection_change_threshold_ms : kSelectionChangeThresholdMs;
        } else {
            bool same_set = (current_top.size() == epoch.selected_tids.size());
            if (same_set) {
                for (size_t i = 0; i < current_top.size(); i++) {
                    if (current_top[i] != epoch.selected_tids[i]) { same_set = false; break; }
                }
            }
            if (same_set) {
                epoch.has_pending = false;
                epoch.pending_tids.clear();
            } else {
                if (!epoch.has_pending) {
                    epoch.has_pending = true;
                    epoch.pending_tids = current_top;
                    epoch.pending_since_ms = now_ms;
                } else if (epoch.pending_tids == current_top) {
                    if (now_ms - epoch.pending_since_ms >= epoch.selection_change_threshold_ms) {
                        epoch.epoch++;
                        epoch.selected_tids = current_top;
                        epoch.stable_since_ms = now_ms;
                        epoch.last_reselect_ms = now_ms;
                        epoch.has_pending = false;
                        epoch.pending_tids.clear();
                    }
                } else {
                    epoch.pending_tids = current_top;
                    epoch.pending_since_ms = now_ms;
                }
            }
        }
        epoch.last_lookup_loop_tick = loop_tick;
    }

    for (size_t i = 0; i < epoch.selected_tids.size(); i++) {
        if (epoch.selected_tids[i] == (pid_t)tid) {
            target_it->second.selection_epoch = epoch.epoch;
            return true;
        }
    }
    return false;
}

static void cleanup_stale_selection_epochs_locked() {
    long long now_ms = get_current_time_ms();
    if (now_ms - g_last_epoch_stale_cleanup_ms < 60000) return;
    g_last_epoch_stale_cleanup_ms = now_ms;
    for (auto it = g_selection_epochs.begin(); it != g_selection_epochs.end(); ) {
        bool any_alive = false;
        for (pid_t t : it->second.selected_tids) {
            if (g_threads.find((int)t) != g_threads.end()) { any_alive = true; break; }
        }
        if (!any_alive) { it = g_selection_epochs.erase(it); }
        else { ++it; }
    }
}

static void clear_all_selection_epochs_locked() {
    g_selection_epochs.clear();
}

static bool thread_json_role_matches_locked(
    const ThreadJsonRoleRule& rule,
    pid_t tid,
    const char* comm) {
    if (!rule.enabled || rule.selectors.empty()) return false;
    if (rule.loading_only && !g_scene_policy.game_loading) return false;
    if (!thread_json_role_selector_match_locked(rule, tid, comm)) return false;
    return thread_json_role_limit_allowed_locked(rule, tid);
}

static const SceneCategoryRule* find_scene_category_rule_for_foreground_locked(const char* package) {
    if (!package || !package[0] || !g_foreground_package[0]) return NULL;
    if (strcmp(package, g_foreground_package) != 0) return NULL;
    if (!g_scene_policy.foreground_category[0]) return NULL;
    for (size_t i = 0; i < g_scene_category_rules.size(); i++) {
        const SceneCategoryRule& rule = g_scene_category_rules[i];
        if (!rule.has_default_action) continue;
        if (rule.category == g_scene_policy.foreground_category) {
            return &rule;
        }
    }
    return NULL;
}

static bool scene_category_allows_thread_json_fallback_locked(const char* category, ThreadClass cls) {
    if (!category || !category[0]) return false;
    if (strcmp(category, "HonorOfKings") == 0) {
        return cls == THREAD_CLASS_MAIN || cls == THREAD_CLASS_RENDER || cls == THREAD_CLASS_BURST;
    }
    if (strcmp(category, "GenshinImpact") == 0) {
        return cls == THREAD_CLASS_MAIN || cls == THREAD_CLASS_RENDER;
    }
    if (strcmp(category, "Game") == 0) {
        return cls == THREAD_CLASS_MAIN || cls == THREAD_CLASS_RENDER;
    }
    return cls == THREAD_CLASS_MAIN || cls == THREAD_CLASS_RENDER;
}

static const ThreadJsonResolvedRule* resolve_thread_json_rule_locked(
    const char* package,
    pid_t tid,
    const char* comm,
    float cpu_usage_pct,
    ThreadClass cls,
    ThreadJsonResolvedRule* out_rule) {
    (void)cpu_usage_pct;
    if (!out_rule) return NULL;
    out_rule->action = NULL;
    out_rule->safety = NULL;
    out_rule->role_kind = THREAD_JSON_ROLE_OTHER;
    out_rule->rule_name = "unknown";
    out_rule->source_kind = THREAD_JSON_RULE_SOURCE_UNKNOWN;
    out_rule->stable_hold_ms = 0;
    out_rule->rebind_cooldown_ms = 0;
    out_rule->selection_change_threshold_ms = 0;
    out_rule->pin_once = false;
    out_rule->prefer_existing_binding = false;
    const ThreadJsonAppRule* app_rule_ptr = find_thread_app_rule_for_package_locked(package);
    if (app_rule_ptr) {
        const ThreadJsonAppRule& app_rule = *app_rule_ptr;
        static const int role_order[] = {
            THREAD_JSON_ROLE_MAIN,
            THREAD_JSON_ROLE_GFX,
            THREAD_JSON_ROLE_RENDER,
            THREAD_JSON_ROLE_WORKER,
            THREAD_JSON_ROLE_DOWNLOAD,
            THREAD_JSON_ROLE_OTHER
        };
        static const char* role_names[] = {
            "main", "gfx", "render", "worker", "download", "other"
        };
        for (size_t i = 0; i < sizeof(role_order) / sizeof(role_order[0]); i++) {
            int role = role_order[i];
            if (thread_json_role_matches_locked(app_rule.roles[role], tid, comm)) {
                out_rule->action = &app_rule.roles[role].action;
                out_rule->safety = &app_rule.safety;
                out_rule->role_kind = role;
                out_rule->rule_name = role_names[role];
                out_rule->source_kind = THREAD_JSON_RULE_SOURCE_ROLE;
                /* 复制角色级稳定参数 */
                out_rule->stable_hold_ms = app_rule.roles[role].stable_hold_ms;
                out_rule->rebind_cooldown_ms = app_rule.roles[role].rebind_cooldown_ms;
                out_rule->selection_change_threshold_ms = app_rule.roles[role].selection_change_threshold_ms;
                out_rule->pin_once = app_rule.roles[role].pin_once;
                out_rule->prefer_existing_binding = app_rule.roles[role].prefer_existing_binding;
                return out_rule;
            }
        }
        for (size_t i = 0; i < app_rule.custom_rules.size(); i++) {
            const ThreadJsonCustomRule& custom_rule = app_rule.custom_rules[i];
            if (thread_json_role_matches_locked(custom_rule.rule, tid, comm)) {
                out_rule->action = &custom_rule.rule.action;
                out_rule->safety = &app_rule.safety;
                out_rule->role_kind = THREAD_JSON_ROLE_OTHER;
                out_rule->rule_name = custom_rule.name.c_str();
                out_rule->source_kind = THREAD_JSON_RULE_SOURCE_CUSTOM;
                /* 复制自定义规则稳定参数 */
                out_rule->stable_hold_ms = custom_rule.rule.stable_hold_ms;
                out_rule->rebind_cooldown_ms = custom_rule.rule.rebind_cooldown_ms;
                out_rule->selection_change_threshold_ms = custom_rule.rule.selection_change_threshold_ms;
                out_rule->pin_once = custom_rule.rule.pin_once;
                out_rule->prefer_existing_binding = custom_rule.rule.prefer_existing_binding;
                return out_rule;
            }
        }
        if (app_rule.has_default_action) {
            out_rule->action = &app_rule.default_action;
            out_rule->safety = &app_rule.safety;
            out_rule->role_kind = THREAD_JSON_ROLE_OTHER;
            out_rule->rule_name = "default";
            out_rule->source_kind = THREAD_JSON_RULE_SOURCE_DEFAULT;
            return out_rule;
        }
    }
    const SceneCategoryRule* scene_rule = find_scene_category_rule_for_foreground_locked(package);
    if (scene_rule && scene_rule->has_default_action &&
        scene_category_allows_thread_json_fallback_locked(scene_rule->category.c_str(), cls)) {
        static ThreadJsonSafetyConfig kSceneDefaultSafety = {};
        out_rule->action = &scene_rule->default_action;
        out_rule->safety = app_rule_ptr ? &app_rule_ptr->safety : &kSceneDefaultSafety;
        out_rule->role_kind = THREAD_JSON_ROLE_OTHER;
        out_rule->rule_name = scene_rule->category.c_str();
        out_rule->source_kind = THREAD_JSON_RULE_SOURCE_SCENE_DEFAULT;
        return out_rule;
    }
    return NULL;
}

static int thread_json_sched_policy_to_native(int sched_policy) {
    switch (sched_policy) {
        case THREAD_JSON_SCHED_RR:
            return SCHED_RR;
        case THREAD_JSON_SCHED_FIFO:
            return SCHED_FIFO;
        case THREAD_JSON_SCHED_OTHER:
        default:
            return SCHED_OTHER;
    }
}

static bool capture_thread_sched_baseline_locked(ThreadRuntimeState& state) {
    if (state.sched_baseline_captured) return true;
    errno = 0;
    int current_nice = getpriority(PRIO_PROCESS, state.tid);
    if (errno != 0) {
        current_nice = 0;
    }
    int current_policy = sched_getscheduler(state.tid);
    if (current_policy < 0) {
        current_policy = SCHED_OTHER;
    }
    struct sched_param param = {};
    if (sched_getparam(state.tid, &param) != 0) {
        param.sched_priority = 0;
    }
    state.baseline_nice = current_nice;
    state.baseline_sched_policy = current_policy;
    state.baseline_sched_priority = param.sched_priority;
    state.sched_baseline_captured = true;
    return true;
}

static void restore_thread_runtime_adjustments_locked(ThreadRuntimeState& state, const char* reason) {
    if (!state.sched_baseline_captured) return;
    if (state.applied_sched_policy != state.baseline_sched_policy ||
        state.applied_sched_priority != state.baseline_sched_priority) {
        struct sched_param param = {};
        param.sched_priority = state.baseline_sched_priority;
        if (sched_setscheduler(state.tid, state.baseline_sched_policy, &param) != 0) {
            char msg[256];
            safe_snprintf(msg, sizeof(msg),
                          "恢复线程调度失败 | tid:%d | comm:%s | reason:%s | err:%s",
                          (int)state.tid,
                          state.comm.c_str(),
                          reason ? reason : "unknown",
                          strerror(errno));
            log_message(msg);
        }
    }
    if (state.applied_nice != state.baseline_nice) {
        if (setpriority(PRIO_PROCESS, state.tid, state.baseline_nice) != 0) {
            char msg[256];
            safe_snprintf(msg, sizeof(msg),
                          "恢复线程优先级失败 | tid:%d | comm:%s | reason:%s | err:%s",
                          (int)state.tid,
                          state.comm.c_str(),
                          reason ? reason : "unknown",
                          strerror(errno));
            log_message(msg);
        }
    }
    state.applied_nice = state.baseline_nice;
    state.applied_sched_policy = state.baseline_sched_policy;
    state.applied_sched_priority = state.baseline_sched_priority;
}

static void restore_all_thread_runtime_adjustments_locked(const char* reason) {
    for (auto& kv : g_threads) {
        restore_thread_runtime_adjustments_locked(kv.second, reason);
    }
}

static int count_active_rt_threads_locked() {
    int count = 0;
    for (auto& kv : g_threads) {
        int policy = kv.second.applied_sched_policy;
        if (policy == SCHED_RR || policy == SCHED_FIFO) {
            count++;
        }
    }
    return count;
}

static void apply_thread_json_action_locked(
    ThreadRuntimeState& state,
    const ThreadJsonResolvedRule* resolved_rule) {
    if (!resolved_rule || !resolved_rule->action) {
        restore_thread_runtime_adjustments_locked(state, "rule_missing");
        return;
    }

    const ThreadJsonAction& action = *resolved_rule->action;
    capture_thread_sched_baseline_locked(state);

    if (action.has_nice && action.nice != state.applied_nice) {
        if (setpriority(PRIO_PROCESS, state.tid, action.nice) == 0) {
            state.applied_nice = action.nice;
        }
    } else if (!action.has_nice && state.applied_nice != state.baseline_nice) {
        if (setpriority(PRIO_PROCESS, state.tid, state.baseline_nice) == 0) {
            state.applied_nice = state.baseline_nice;
        }
    }

    int target_policy = state.baseline_sched_policy;
    int target_priority = state.baseline_sched_priority;
    if (action.has_scheduler) {
        target_policy = thread_json_sched_policy_to_native(action.sched_policy);
        target_priority = action.sched_priority;
    }
    if (target_policy == SCHED_RR || target_policy == SCHED_FIFO) {
        const ThreadJsonSafetyConfig* safety = resolved_rule->safety;
        bool allow_rt = safety && safety->allow_rt;
        bool allow_fifo = safety && safety->allow_fifo;
        bool role_forbidden = safety &&
                              resolved_rule->role_kind >= 0 &&
                              resolved_rule->role_kind < THREAD_JSON_ROLE_COUNT &&
                              safety->forbid_rt_roles[resolved_rule->role_kind];
        if (!allow_rt || role_forbidden || (target_policy == SCHED_FIFO && !allow_fifo)) {
            target_policy = SCHED_OTHER;
            target_priority = 0;
        } else {
            if (safety) {
                if (target_priority < safety->rt_priority_min) {
                    target_priority = safety->rt_priority_min;
                }
                if (target_priority > safety->rt_priority_max) {
                    target_priority = safety->rt_priority_max;
                }
            }
            if (target_priority <= 0) {
                target_priority = 1;
            }
            bool already_rt = (state.applied_sched_policy == SCHED_RR || state.applied_sched_policy == SCHED_FIFO);
            if (!already_rt && safety && safety->rt_max_threads > 0 &&
                count_active_rt_threads_locked() >= safety->rt_max_threads) {
                target_policy = SCHED_OTHER;
                target_priority = 0;
            }
        }
    }
    if (!action.has_scheduler && state.applied_sched_policy != state.baseline_sched_policy) {
        target_policy = state.baseline_sched_policy;
        target_priority = state.baseline_sched_priority;
    }
    if (target_policy == SCHED_OTHER) {
        target_priority = 0;
    }
    if (target_policy != state.applied_sched_policy || target_priority != state.applied_sched_priority) {
        struct sched_param param = {};
        param.sched_priority = target_priority;
        if (sched_setscheduler(state.tid, target_policy, &param) == 0) {
            state.applied_sched_policy = target_policy;
            state.applied_sched_priority = target_priority;
        }
    }
}

static float get_cpu_idle_ratio(int cpu_id, const AllCpuStats& stats) {
    if (!g_prev_stats_valid || cpu_id < 0 || cpu_id >= 32) return 1.0f;
    uint64_t idle_now = stats.idle[cpu_id];
    uint64_t total_now = stats.total[cpu_id];
    uint64_t idle_prev = g_prev_stats.idle[cpu_id];
    uint64_t total_prev = g_prev_stats.total[cpu_id];
    if (total_now <= total_prev) return 1.0f;
    uint64_t idle_delta = idle_now >= idle_prev ? (idle_now - idle_prev) : 0;
    uint64_t total_delta = total_now - total_prev;
    if (total_delta == 0) return 1.0f;
    float ratio = (float)idle_delta / (float)total_delta;
    if (ratio < 0.0f) ratio = 0.0f;
    if (ratio > 1.0f) ratio = 1.0f;
    return ratio;
}

static int count_big_cluster_cpus_in_mask(const std::vector<int>& cpus) {
    if (cluster_count <= 0) return 0;
    int big_cluster = cluster_count - 1;
    int count = 0;
    for (size_t i = 0; i < cpus.size(); i++) {
        for (int j = 0; j < cpu_clusters[big_cluster].cpu_count; j++) {
            if (cpu_clusters[big_cluster].cpu_ids[j] == cpus[i]) {
                count++;
                break;
            }
        }
    }
    return count;
}

static float score_cpu_mask(const std::vector<int>& cpus, const AllCpuStats& stats, bool prefer_dual_big, bool single_big_only) {
    if (cpus.empty()) return -1000.0f;
    float idle_sum = 0.0f;
    for (size_t i = 0; i < cpus.size(); i++) {
        idle_sum += get_cpu_idle_ratio(cpus[i], stats);
    }
    float score = idle_sum / (float)cpus.size();
    int big_cpu_count = count_big_cluster_cpus_in_mask(cpus);
    if (prefer_dual_big) {
        score += 0.18f * (float)big_cpu_count;
    }
    if (single_big_only && big_cpu_count > 1) {
        score -= 0.40f * (float)(big_cpu_count - 1);
    }
    score -= 0.015f * (float)cpus.size();
    return score;
}

static void add_mask_candidate(std::vector<std::vector<int>>& candidates, const int* cpu_list, int cpu_count) {
    std::vector<int> mask;
    for (int i = 0; i < cpu_count; i++) {
        append_cpu_if_present(mask, cpu_list[i]);
    }
    if (!mask.empty()) {
        candidates.push_back(mask);
    }
}

static void add_range_candidate(std::vector<std::vector<int>>& candidates, int start_cpu, int end_cpu) {
    std::vector<int> mask;
    append_cpu_range_if_present(mask, start_cpu, end_cpu);
    if (!mask.empty()) {
        candidates.push_back(mask);
    }
}

static bool choose_best_mask_from_candidates(const std::vector<std::vector<int>>& candidates,
                                             const AllCpuStats& stats,
                                             bool prefer_dual_big,
                                             bool single_big_only,
                                             std::vector<int>& out) {
    if (candidates.empty()) return false;
    int best_index = -1;
    float best_score = -1000.0f;
    for (size_t i = 0; i < candidates.size(); i++) {
        float score = score_cpu_mask(candidates[i], stats, prefer_dual_big, single_big_only);
        score -= 0.001f * (float)i;
        if (best_index < 0 || score > best_score) {
            best_index = (int)i;
            best_score = score;
        }
    }
    if (best_index < 0) return false;
    out = candidates[(size_t)best_index];
    return !out.empty();
}

static bool scene_category_matches_locked(const char* expected) {
    if (!expected || !expected[0]) return false;
    return strcmp(g_scene_policy.foreground_category, expected) == 0;
}

static bool resolve_scene_affinity_template_locked(const char* package,
                                                   pid_t tid,
                                                   const char* comm,
                                                   ThreadClass cls,
                                                   float cpu_usage_pct,
                                                   const AllCpuStats& stats,
                                                   bool is_main,
                                                   bool is_render,
                                                   bool is_worker,
                                                   bool is_taskgraph,
                                                   std::vector<int>& cpus,
                                                   int& primary_cluster) {
    (void)package;
    cpus.clear();
    primary_cluster = -1;

    bool hok_scene = scene_category_matches_locked("HonorOfKings");
    bool genshin_scene = scene_category_matches_locked("GenshinImpact");
    bool generic_game_scene = scene_category_matches_locked("Game");
    bool camera_or_scanner_scene =
        g_scene_policy.camera_active ||
        g_scene_policy.scanner_active ||
        scene_category_matches_locked("Camera") ||
        scene_category_matches_locked("Scanner");
    bool mini_program_scene =
        g_scene_policy.mini_program_active ||
        scene_category_matches_locked("MiniProgram");
    if (!camera_or_scanner_scene && !mini_program_scene &&
        !hok_scene && !genshin_scene && !generic_game_scene) {
        return false;
    }

    bool is_primary_ui = tid == g_foreground_pid ||
                         is_main ||
                         is_render ||
                         cls == THREAD_CLASS_MAIN ||
                         cls == THREAD_CLASS_RENDER;
    bool is_support_worker = is_worker ||
                             is_taskgraph ||
                             cls == THREAD_CLASS_BURST ||
                             cpu_usage_pct >= (camera_or_scanner_scene ? 8.0f :
                                               (hok_scene ? 8.0f :
                                                (genshin_scene ? 10.0f : 12.0f)));
    if (!is_primary_ui && !is_support_worker) {
        return false;
    }

    TopologyKind topo = get_topology_kind();
    std::vector<std::vector<int>> candidates;

    if (camera_or_scanner_scene) {
        switch (topo) {
            case TOPOLOGY_6_2:
                if (is_primary_ui) {
                    add_range_candidate(candidates, 4, 7);
                    add_range_candidate(candidates, 4, 5);
                    int cpu6[] = {6};
                    int cpu7[] = {7};
                    add_mask_candidate(candidates, cpu6, 1);
                    add_mask_candidate(candidates, cpu7, 1);
                } else {
                    add_range_candidate(candidates, 4, 5);
                    add_range_candidate(candidates, 3, 5);
                    add_range_candidate(candidates, 1, 5);
                }
                break;
            default:
                if (is_primary_ui) {
                    int start = cluster_count > 1 ? cluster_count - 2 : 0;
                    for (int c = start; c < cluster_count; c++) {
                        append_cluster_mask(cpus, c, false, false);
                    }
                } else {
                    int target = cluster_count > 2 ? cluster_count - 2 : (cluster_count > 1 ? 1 : 0);
                    append_cluster_mask(cpus, target, false, false);
                }
                break;
        }
    } else if (hok_scene) {
        switch (topo) {
            case TOPOLOGY_6_2:
                if (is_primary_ui) {
                    add_range_candidate(candidates, 5, 7);
                    add_range_candidate(candidates, 4, 7);
                    add_range_candidate(candidates, 6, 7);
                } else {
                    add_range_candidate(candidates, 4, 6);
                    add_range_candidate(candidates, 4, 5);
                    add_range_candidate(candidates, 3, 5);
                }
                break;
            default:
                if (is_primary_ui) {
                    int start = cluster_count > 2 ? cluster_count - 2 : (cluster_count > 1 ? 1 : 0);
                    for (int c = start; c < cluster_count; c++) {
                        append_cluster_mask(cpus, c, false, false);
                    }
                } else {
                    int target = cluster_count > 2 ? cluster_count - 2 : (cluster_count > 1 ? 1 : 0);
                    append_cluster_mask(cpus, target, false, false);
                    if (cluster_count > 2) {
                        append_cluster_mask(cpus, cluster_count - 1, false, false);
                    }
                }
                break;
        }
    } else if (genshin_scene) {
        switch (topo) {
            case TOPOLOGY_6_2:
                if (is_primary_ui) {
                    add_range_candidate(candidates, 4, 7);
                    add_range_candidate(candidates, 4, 5);
                    add_range_candidate(candidates, 6, 7);
                } else {
                    add_range_candidate(candidates, 4, 5);
                    add_range_candidate(candidates, 3, 5);
                    add_range_candidate(candidates, 1, 5);
                }
                break;
            default:
                if (is_primary_ui) {
                    int start = cluster_count > 1 ? cluster_count - 2 : 0;
                    for (int c = start; c < cluster_count; c++) {
                        append_cluster_mask(cpus, c, false, false);
                    }
                } else {
                    int target = cluster_count > 2 ? cluster_count - 2 : (cluster_count > 1 ? 1 : 0);
                    append_cluster_mask(cpus, target, false, false);
                }
                break;
        }
    } else if (generic_game_scene) {
        switch (topo) {
            case TOPOLOGY_6_2:
                if (is_primary_ui) {
                    add_range_candidate(candidates, 4, 5);
                    add_range_candidate(candidates, 3, 5);
                    add_range_candidate(candidates, 1, 5);
                } else {
                    add_range_candidate(candidates, 3, 4);
                    add_range_candidate(candidates, 1, 4);
                }
                break;
            default:
                if (is_primary_ui) {
                    int target = cluster_count > 1 ? cluster_count - 2 : 0;
                    append_cluster_mask(cpus, target, false, false);
                    if (cluster_count > 2) {
                        append_cluster_mask(cpus, cluster_count - 1, false, false);
                    }
                } else {
                    int target = cluster_count > 2 ? cluster_count - 2 : 0;
                    append_cluster_mask(cpus, target, false, true);
                }
                break;
        }
    } else if (mini_program_scene) {
        switch (topo) {
            case TOPOLOGY_6_2:
                if (is_primary_ui) {
                    add_range_candidate(candidates, 3, 5);
                    add_range_candidate(candidates, 4, 5);
                    add_range_candidate(candidates, 1, 5);
                } else {
                    add_range_candidate(candidates, 3, 4);
                    add_range_candidate(candidates, 1, 4);
                }
                break;
            default:
                if (is_primary_ui) {
                    int target = cluster_count > 1 ? cluster_count - 2 : 0;
                    append_cluster_mask(cpus, target, false, false);
                    if (cluster_count > 2) {
                        append_cluster_mask(cpus, cluster_count - 1, false, false);
                    }
                } else {
                    int target = cluster_count > 2 ? cluster_count - 2 : 0;
                    append_cluster_mask(cpus, target, false, true);
                }
                break;
        }
    }

    if (!candidates.empty()) {
        choose_best_mask_from_candidates(candidates, stats, false, false, cpus);
    }
    if (cpus.empty()) {
        return false;
    }
    primary_cluster = highest_cluster_for_mask(cpus);
    return primary_cluster >= 0;
}

[[maybe_unused]] static bool resolve_affinity_template(const char* package, pid_t tid, const char* comm, ThreadClass cls, float cpu_usage_pct, const AllCpuStats& stats, std::vector<int>& cpus, int& primary_cluster) {
    cpus.clear();
    primary_cluster = -1;
    bool is_main = is_main_thread_name_for_package(package, comm, tid);
    bool is_render = is_render_thread_name(comm);
    bool is_worker = is_worker_thread_name(comm);
    bool is_download = is_download_thread_name(package, comm, cpu_usage_pct);
    bool is_game_priority = should_treat_as_priority_game_thread(package, comm, tid, cpu_usage_pct);
    bool is_gfx_like = is_unity_gfx_thread_name(comm) || is_rhi_thread_name(comm) || is_render;
    bool is_taskgraph = is_taskgraph_thread_name(comm);
    int thread_dash_rank = get_thread_dash_rank_locked(tid, comm, cpu_usage_pct);
    int worker_rank = get_worker_rank_locked(tid, comm, cpu_usage_pct);
    bool top_thread_dash = thread_dash_rank >= 0 && thread_dash_rank < 3 && cpu_usage_pct >= 3.0f;
    bool top_worker = worker_rank >= 0 && worker_rank < 2 && cpu_usage_pct >= 2.0f;
    int non_primary_rank = get_non_primary_thread_rank_locked(package, tid, comm, cpu_usage_pct);
    bool allow_non_primary_single_big = non_primary_rank >= 0 && non_primary_rank < 2 && cpu_usage_pct >= 3.0f;
    bool single_big_only = is_single_big_core_game_package(package);
    bool prefer_dual_big = is_dual_big_core_game_package(package);
    bool is_ue_game = is_ue_game_package_name(package);
    bool main_dual_big = prefer_dual_big ||
                         matches_any_pattern(package, g_thread_signature_config.main_dual_big_package_patterns);
    bool render_dual_big = prefer_dual_big ||
                           matches_any_pattern(package, g_thread_signature_config.render_dual_big_package_patterns) ||
                           (is_ue_game &&
                            cpu_usage_pct >= g_thread_signature_config.render_dual_big_min_usage_pct);
    TopologyKind topo = get_topology_kind();

    if (is_download) {
        for (int c = 0; c < cluster_count; c++) {
            append_cluster_mask(cpus, c, false, false);
        }
        primary_cluster = cluster_count - 1;
        return !cpus.empty();
    }

    if (resolve_scene_affinity_template_locked(
            package, tid, comm, cls, cpu_usage_pct, stats,
            is_main, is_render, is_worker, is_taskgraph,
            cpus, primary_cluster)) {
        return true;
    }

    bool generic_hot_thread = (cls == THREAD_CLASS_MAIN || cls == THREAD_CLASS_RENDER || is_game_priority ||
                               (cls == THREAD_CLASS_BURST && cpu_usage_pct >= 12.0f));
    if (!is_main && !is_render && !is_worker && !generic_hot_thread && !top_thread_dash && !top_worker && !is_taskgraph) {
        return false;
    }

    std::vector<std::vector<int>> candidates;

    switch (topo) {
        case TOPOLOGY_2_3_2_1:
            if (is_main || top_thread_dash) {
                int mask[] = {7};
                add_mask_candidate(candidates, mask, 1);
            } else if (is_gfx_like || top_worker || is_taskgraph) {
                add_range_candidate(candidates, 2, 4);
                add_range_candidate(candidates, 2, 6);
                add_range_candidate(candidates, 5, 6);
            } else {
                add_range_candidate(candidates, 2, 6);
                add_range_candidate(candidates, 2, 4);
            }
            break;
        case TOPOLOGY_3_4_1:
            if (is_main || top_thread_dash) {
                int mask[] = {7};
                add_mask_candidate(candidates, mask, 1);
            } else if (is_gfx_like || top_worker || is_taskgraph) {
                add_range_candidate(candidates, 4, 6);
                add_range_candidate(candidates, 4, 5);
                int mask1[] = {6};
                int mask2[] = {3, 6};
                int mask3[] = {3, 4};
                int mask4[] = {3, 5};
                add_mask_candidate(candidates, mask1, 1);
                add_mask_candidate(candidates, mask2, 2);
                add_mask_candidate(candidates, mask3, 2);
                add_mask_candidate(candidates, mask4, 2);
            } else {
                add_range_candidate(candidates, 3, 6);
                add_range_candidate(candidates, 4, 6);
                int mask[] = {3, 5};
                add_mask_candidate(candidates, mask, 2);
            }
            break;
        case TOPOLOGY_4_3_1:
            if (is_main || top_thread_dash) {
                int mask[] = {7};
                add_mask_candidate(candidates, mask, 1);
            } else if (is_gfx_like || top_worker || is_taskgraph) {
                add_range_candidate(candidates, 4, 6);
                add_range_candidate(candidates, 4, 5);
                int mask[] = {6};
                add_mask_candidate(candidates, mask, 1);
            } else {
                add_range_candidate(candidates, 4, 6);
                add_range_candidate(candidates, 4, 5);
            }
            break;
        case TOPOLOGY_6_2:
            if (is_main) {
                bool render_single_big_active = get_other_primary_single_big_cpu_locked(package, tid, true, false) >= 0;
                if (main_dual_big && !single_big_only) {
                    int pair_mask[] = {6, 7};
                    add_mask_candidate(candidates, pair_mask, 2);
                }
                if (!render_single_big_active) {
                    int cpu6[] = {6};
                    int cpu7[] = {7};
                    add_mask_candidate(candidates, cpu6, 1);
                    add_mask_candidate(candidates, cpu7, 1);
                }
                add_range_candidate(candidates, 4, 5);
                add_range_candidate(candidates, 1, 5);
            } else if (is_gfx_like) {
                bool main_single_big_active = get_other_primary_single_big_cpu_locked(package, tid, false, true) >= 0;
                if (render_dual_big && !single_big_only) {
                    int pair_mask[] = {6, 7};
                    add_mask_candidate(candidates, pair_mask, 2);
                }
                add_range_candidate(candidates, 1, 5);
                add_range_candidate(candidates, 1, 4);
                if (!main_single_big_active) {
                    int cpu6[] = {6};
                    int cpu7[] = {7};
                    add_mask_candidate(candidates, cpu6, 1);
                    add_mask_candidate(candidates, cpu7, 1);
                }
            } else if (top_thread_dash) {
                int mask1[] = {4, 5};
                int mask2[] = {3, 4, 5};
                add_mask_candidate(candidates, mask1, 2);
                add_mask_candidate(candidates, mask2, 3);
                add_range_candidate(candidates, 1, 4);
                if (allow_non_primary_single_big) {
                    int cpu6[] = {6};
                    int cpu7[] = {7};
                    add_mask_candidate(candidates, cpu6, 1);
                    add_mask_candidate(candidates, cpu7, 1);
                }
            } else if (top_worker || is_taskgraph) {
                int mask1[] = {4, 5};
                int mask2[] = {3, 4};
                int mask3[] = {3, 4, 5};
                add_mask_candidate(candidates, mask1, 2);
                add_mask_candidate(candidates, mask2, 2);
                add_mask_candidate(candidates, mask3, 3);
                add_range_candidate(candidates, 1, 4);
                if (allow_non_primary_single_big) {
                    int cpu6[] = {6};
                    int cpu7[] = {7};
                    add_mask_candidate(candidates, cpu6, 1);
                    add_mask_candidate(candidates, cpu7, 1);
                }
            } else {
                add_range_candidate(candidates, 1, 5);
                add_range_candidate(candidates, 1, 4);
            }
            break;
        default:
            if (is_main || cls == THREAD_CLASS_MAIN || top_thread_dash) {
                int start = cluster_count > 1 ? cluster_count - 2 : 0;
                for (int c = start; c < cluster_count; c++) {
                    append_cluster_mask(cpus, c, false, false);
                }
            } else if (is_gfx_like || cls == THREAD_CLASS_RENDER || top_worker || is_taskgraph) {
                int start = cluster_count > 1 ? cluster_count - 2 : 0;
                for (int c = start; c < cluster_count; c++) {
                    append_cluster_mask(cpus, c, false, false);
                }
            } else if (cls == THREAD_CLASS_BURST) {
                int target = cluster_count > 2 ? cluster_count - 2 : 0;
                append_cluster_mask(cpus, target, false, false);
            }
            break;
    }

    if (!candidates.empty()) {
        choose_best_mask_from_candidates(candidates, stats, prefer_dual_big, single_big_only, cpus);
    }
    if (cpus.empty()) return false;
    primary_cluster = highest_cluster_for_mask(cpus);
    return true;
}

static float get_busy_thread_usage_locked() {
    float best = 0.0f;
    for (auto& kv : g_threads) {
        const ThreadRuntimeState& state = kv.second;
        if (is_main_thread_name_for_package(g_foreground_package, state.comm.c_str(), state.tid) ||
            is_render_thread_name(state.comm.c_str())) {
            if (state.cpu_usage_pct > best) {
                best = state.cpu_usage_pct;
            }
        }
    }
    return best;
}

static bool has_active_download_thread_locked() {
    for (auto& kv : g_threads) {
        const ThreadRuntimeState& state = kv.second;
        if (is_download_thread_name(g_foreground_package, state.comm.c_str(), state.cpu_usage_pct)) {
            return true;
        }
    }
    return false;
}

float get_busy_thread_usage() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return get_busy_thread_usage_locked();
}

bool has_active_download_thread() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return has_active_download_thread_locked();
}

pid_t get_foreground_pid_snapshot() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_foreground_pid;
}

static void detach_frame_probe_locked() {
}

static void clear_bpf_u32_key_map_locked(int map_fd) {
    if (map_fd < 0) return;
    std::vector<uint32_t> keys;
    uint32_t key = 0;
    int err = bpf_map_get_next_key(map_fd, NULL, &key);
    while (err == 0) {
        keys.push_back(key);
        uint32_t next_key = 0;
        err = bpf_map_get_next_key(map_fd, &key, &next_key);
        key = next_key;
    }
    for (size_t i = 0; i < keys.size(); i++) {
        bpf_map_delete_elem(map_fd, &keys[i]);
    }
}

static void clear_thread_runtime_map_locked() {
    clear_bpf_u32_key_map_locked(g_thread_runtime_map_fd);
}

static void clear_target_tid_map_locked() {
    clear_bpf_u32_key_map_locked(g_target_tid_map_fd);
}

static void sync_target_tgid_locked() {
    if (!g_bpf_ready.load() || g_target_tgid_map_fd < 0) return;
    uint32_t key = 0;
    uint32_t tgid = g_foreground_pid > 0 ? (uint32_t)g_foreground_pid : 0;
    if ((pid_t)tgid == g_installed_target_tgid) return;
    if (bpf_map_update_elem(g_target_tgid_map_fd, &key, &tgid, BPF_ANY) != 0) {
        log_bpf_error_once("eBPF写入目标TGID失败", errno);
        return;
    }
    g_installed_target_tgid = (pid_t)tgid;
    if (tgid == 0) {
        clear_thread_runtime_map_locked();
        clear_target_tid_map_locked();
    }
}

static void sync_render_thread_targets_locked(bool force) {
    if (!g_bpf_ready.load() || g_target_tid_map_fd < 0) return;
    long long now_ms = get_current_time_ms();
    if (!force && now_ms - g_last_render_tid_sync_ms < 500) return;
    g_last_render_tid_sync_ms = now_ms;

    clear_target_tid_map_locked();
    clear_invalid_latched_primary_tids_locked();
    bool latched_added = false;
    auto add_target_tid = [&](pid_t tid) -> bool {
        if (tid <= 0) return true;
        auto it = g_threads.find((int)tid);
        if (it == g_threads.end()) return false;
        const ThreadRuntimeState& state = it->second;
        if (state.pid != g_foreground_pid) return false;
        if (state.package.empty() || strcmp(state.package.c_str(), g_foreground_package) != 0) return false;
        uint32_t key = (uint32_t)state.tid;
        uint32_t enabled = 1;
        if (bpf_map_update_elem(g_target_tid_map_fd, &key, &enabled, BPF_ANY) != 0) {
            log_bpf_error_once("eBPF写入渲染线程TID失败", errno);
            return false;
        }
        latched_added = true;
        return true;
    };
    if (is_scheduler_session_foreground_locked()) {
        add_target_tid(g_scheduler_session.latched_main_tid);
        add_target_tid(g_scheduler_session.latched_render_tid);
        if (latched_added) return;
    }
    for (auto& kv : g_threads) {
        ThreadRuntimeState& state = kv.second;
        bool render_like = should_treat_as_priority_game_thread(
            g_foreground_package, state.comm.c_str(), state.tid, state.cpu_usage_pct);
        if (!render_like) continue;
        if (!is_hot_priority_thread_locked(state.tid, state.comm.c_str(), state.cpu_usage_pct)) continue;
        uint32_t tid = (uint32_t)state.tid;
        uint32_t enabled = 1;
        if (bpf_map_update_elem(g_target_tid_map_fd, &tid, &enabled, BPF_ANY) != 0) {
            log_bpf_error_once("eBPF写入渲染线程TID失败", errno);
            break;
        }
    }
}

static bool consume_urgent_render_flag_locked() {
    if (!g_bpf_ready.load() || g_urgent_flag_map_fd < 0) return false;
    uint32_t key = 0;
    uint32_t urgent = 0;
    if (bpf_map_lookup_elem(g_urgent_flag_map_fd, &key, &urgent) != 0) {
        return false;
    }
    if (urgent == 0) return false;
    uint32_t zero = 0;
    bpf_map_update_elem(g_urgent_flag_map_fd, &key, &zero, BPF_ANY);
    return true;
}

static void pull_bpf_runtimes_locked(long long now_ms) {
    if (!g_bpf_ready.load() || g_thread_runtime_map_fd < 0) return;
    std::vector<uint32_t> keys;
    uint32_t key = 0;
    int err = bpf_map_get_next_key(g_thread_runtime_map_fd, NULL, &key);
    while (err == 0) {
        keys.push_back(key);
        uint32_t next_key = 0;
        err = bpf_map_get_next_key(g_thread_runtime_map_fd, &key, &next_key);
        key = next_key;
    }

    for (size_t i = 0; i < keys.size(); i++) {
        uint64_t runtime_ns = 0;
        if (bpf_map_lookup_elem(g_thread_runtime_map_fd, &keys[i], &runtime_ns) == 0) {
            auto it = g_threads.find((int)keys[i]);
            if (it != g_threads.end()) {
                apply_runtime_ns_locked(it->second, runtime_ns, now_ms);
            }
        }
        bpf_map_delete_elem(g_thread_runtime_map_fd, &keys[i]);
    }
}

static int handle_process_event_signal(void* ctx, void* data, size_t size) {
    (void)ctx;
    if (!data || size < sizeof(ProcessEventSignal)) return 0;
    const ProcessEventSignal* signal = reinterpret_cast<const ProcessEventSignal*>(data);
    if (signal->pid <= 0) return 0;

    char package[MAX_PACKAGE_LEN] = {0};
    if (!read_process_package_name(signal->pid, package, sizeof(package))) {
        return 0;
    }

    if (g_foreground_package[0] &&
        strcmp(g_foreground_package, "unknown") != 0 &&
        strcmp(package, g_foreground_package) == 0 &&
        (signal->type == PROCESS_EVENT_EXEC || signal->type == PROCESS_EVENT_FORK)) {
        g_foreground_pid_refresh_pending = true;
    }

    const ThreadJsonAppRule* rule = find_persistent_thread_app_rule_for_package_locked(package);
    if (!rule) {
        return 0;
    }

    long long now_ms = get_current_time_ms();
    std::vector<int> seen;
    collect_process_threads_for_package_locked(signal->pid, package, true, now_ms, seen);
    apply_explicit_persistent_actions_for_process_locked(signal->pid, package, now_ms);

    if (!seen.empty()) {
        char comm_buf[sizeof(signal->comm) + 1] = {0};
        memcpy(comm_buf, signal->comm, sizeof(signal->comm));
        comm_buf[sizeof(signal->comm)] = '\0';

        const char* event_names[] = { "unknown", "fork", "exec", "setaffinity" };
        const char* event_name = (signal->type >= 1 && signal->type <= 3) ? event_names[signal->type] : "unknown";

        char msg[384];
        safe_snprintf(
            msg, sizeof(msg),
            "persistent eBPF事件 | app:%s | pid:%d | tid:%d | event:%s | comm:%s | threads:%d",
            package,
            (int)signal->pid,
            (int)signal->tid,
            event_name,
            comm_buf[0] ? comm_buf : "unknown",
            (int)seen.size());
        log_scheduler_probe_once(msg);
    }
    return 0;
}

static void poll_process_event_ring_locked() {
    if (!g_process_event_ring) return;
    int err = ring_buffer__poll(g_process_event_ring, 0);
    if (err < 0 && err != -EINTR) {
        log_bpf_error_once("process_event ring_buffer__poll", -err);
        return;
    }
    if (g_foreground_pid_refresh_pending) {
        g_foreground_pid_refresh_pending = false;
        refresh_foreground_pid_locked(true);
    }
}

bool init_bpf() {
    if (g_bpf_ready.load()) return true;
    if (g_bpf_failed.load()) return false;

    libbpf_set_print(NULL);
    g_bpf_obj = bpf_object__open_file(BPF_OBJECT_FILE, NULL);
    if (!g_bpf_obj) {
        log_message("eBPF初始化失败: bpf_object__open_file");
        g_bpf_failed.store(1);
        return false;
    }

    int err = bpf_object__load(g_bpf_obj);
    if (err) {
        char msg[256];
        safe_snprintf(msg, sizeof(msg), "eBPF初始化失败: bpf_object__load=%d", err);
        log_message(msg);
        bpf_object__close(g_bpf_obj);
        g_bpf_obj = nullptr;
        g_bpf_failed.store(1);
        return false;
    }

    g_bpf_switch_prog = find_bpf_program_compat("handle_sched_switch", "raw_tracepoint/sched_switch");
    g_bpf_wakeup_prog = find_bpf_program_compat("handle_sched_wakeup", "raw_tracepoint/sched_wakeup");
    g_bpf_process_fork_prog = find_bpf_program_compat("handle_process_fork", "raw_tracepoint/sched_process_fork");
    g_bpf_process_exec_prog = find_bpf_program_compat("handle_process_exec", "raw_tracepoint/sched_process_exec");
    g_bpf_process_affinity_prog = find_bpf_program_compat("handle_sched_setaffinity", "raw_tracepoint/sys_enter");
    if (!g_bpf_switch_prog || !g_bpf_wakeup_prog ||
        !g_bpf_process_fork_prog || !g_bpf_process_exec_prog || !g_bpf_process_affinity_prog) {
        char msg[320];
        safe_snprintf(msg, sizeof(msg),
                      "eBPF初始化失败: 程序缺失 | switch=%d wakeup=%d fork=%d exec=%d affinity=%d",
                      g_bpf_switch_prog ? 1 : 0,
                      g_bpf_wakeup_prog ? 1 : 0,
                      g_bpf_process_fork_prog ? 1 : 0,
                      g_bpf_process_exec_prog ? 1 : 0,
                      g_bpf_process_affinity_prog ? 1 : 0);
        log_message(msg);
        bpf_object__close(g_bpf_obj);
        g_bpf_obj = nullptr;
        g_bpf_failed.store(1);
        return false;
    }

    g_bpf_switch_link = bpf_program__attach(g_bpf_switch_prog);
    if (libbpf_get_error(g_bpf_switch_link)) {
        log_message("eBPF初始化失败: attach sched_switch");
        g_bpf_switch_link = nullptr;
        bpf_object__close(g_bpf_obj);
        g_bpf_obj = nullptr;
        g_bpf_failed.store(1);
        return false;
    }

    g_bpf_wakeup_link = bpf_program__attach(g_bpf_wakeup_prog);
    if (libbpf_get_error(g_bpf_wakeup_link)) {
        log_message("eBPF初始化失败: attach sched_wakeup");
        g_bpf_wakeup_link = nullptr;
        bpf_link__destroy(g_bpf_switch_link);
        g_bpf_switch_link = nullptr;
        bpf_object__close(g_bpf_obj);
        g_bpf_obj = nullptr;
        g_bpf_failed.store(1);
        return false;
    }

    g_bpf_process_fork_link = bpf_program__attach(g_bpf_process_fork_prog);
    if (libbpf_get_error(g_bpf_process_fork_link)) {
        log_message("eBPF初始化失败: attach sched_process_fork");
        g_bpf_process_fork_link = nullptr;
        bpf_link__destroy(g_bpf_wakeup_link);
        g_bpf_wakeup_link = nullptr;
        bpf_link__destroy(g_bpf_switch_link);
        g_bpf_switch_link = nullptr;
        bpf_object__close(g_bpf_obj);
        g_bpf_obj = nullptr;
        g_bpf_failed.store(1);
        return false;
    }

    g_bpf_process_exec_link = bpf_program__attach(g_bpf_process_exec_prog);
    if (libbpf_get_error(g_bpf_process_exec_link)) {
        log_message("eBPF初始化失败: attach sched_process_exec");
        g_bpf_process_exec_link = nullptr;
        bpf_link__destroy(g_bpf_process_fork_link);
        g_bpf_process_fork_link = nullptr;
        bpf_link__destroy(g_bpf_wakeup_link);
        g_bpf_wakeup_link = nullptr;
        bpf_link__destroy(g_bpf_switch_link);
        g_bpf_switch_link = nullptr;
        bpf_object__close(g_bpf_obj);
        g_bpf_obj = nullptr;
        g_bpf_failed.store(1);
        return false;
    }

    g_bpf_process_affinity_link = bpf_program__attach(g_bpf_process_affinity_prog);
    if (libbpf_get_error(g_bpf_process_affinity_link)) {
        log_message("eBPF初始化失败: attach sys_enter_sched_setaffinity");
        g_bpf_process_affinity_link = nullptr;
        bpf_link__destroy(g_bpf_process_exec_link);
        g_bpf_process_exec_link = nullptr;
        bpf_link__destroy(g_bpf_process_fork_link);
        g_bpf_process_fork_link = nullptr;
        bpf_link__destroy(g_bpf_wakeup_link);
        g_bpf_wakeup_link = nullptr;
        bpf_link__destroy(g_bpf_switch_link);
        g_bpf_switch_link = nullptr;
        bpf_object__close(g_bpf_obj);
        g_bpf_obj = nullptr;
        g_bpf_failed.store(1);
        return false;
    }

    struct bpf_map* target_map = bpf_object__find_map_by_name(g_bpf_obj, "target_tgid_map");
    struct bpf_map* runtime_map = bpf_object__find_map_by_name(g_bpf_obj, "thread_runtime_map");
    struct bpf_map* cpu_last_map = bpf_object__find_map_by_name(g_bpf_obj, "cpu_last_ts_map");
    struct bpf_map* urgent_map = bpf_object__find_map_by_name(g_bpf_obj, "urgent_flag_map");
    struct bpf_map* target_tid_map = bpf_object__find_map_by_name(g_bpf_obj, "target_tid_map");
    struct bpf_map* process_event_map = bpf_object__find_map_by_name(g_bpf_obj, "process_event_rb");
    if (!target_map || !runtime_map || !cpu_last_map || !urgent_map || !target_tid_map ||
        !process_event_map) {
        log_message("eBPF初始化失败: 找不到聚合 map");
        bpf_link__destroy(g_bpf_process_affinity_link);
        g_bpf_process_affinity_link = nullptr;
        bpf_link__destroy(g_bpf_process_exec_link);
        g_bpf_process_exec_link = nullptr;
        bpf_link__destroy(g_bpf_process_fork_link);
        g_bpf_process_fork_link = nullptr;
        bpf_link__destroy(g_bpf_wakeup_link);
        g_bpf_wakeup_link = nullptr;
        bpf_link__destroy(g_bpf_switch_link);
        g_bpf_switch_link = nullptr;
        bpf_object__close(g_bpf_obj);
        g_bpf_obj = nullptr;
        g_bpf_failed.store(1);
        return false;
    }

    g_target_tgid_map_fd = bpf_map__fd(target_map);
    g_thread_runtime_map_fd = bpf_map__fd(runtime_map);
    g_cpu_last_ts_map_fd = bpf_map__fd(cpu_last_map);
    g_urgent_flag_map_fd = bpf_map__fd(urgent_map);
    g_target_tid_map_fd = bpf_map__fd(target_tid_map);
    g_process_event_rb_map_fd = bpf_map__fd(process_event_map);
    ring_buffer_opts rb_opts = {};
    rb_opts.sz = sizeof(rb_opts);
    g_process_event_ring = ring_buffer__new(
        g_process_event_rb_map_fd, handle_process_event_signal, NULL, &rb_opts);
    if (!g_process_event_ring) {
        log_message("eBPF初始化失败: process_event ring_buffer__new");
        bpf_link__destroy(g_bpf_process_affinity_link);
        g_bpf_process_affinity_link = nullptr;
        bpf_link__destroy(g_bpf_process_exec_link);
        g_bpf_process_exec_link = nullptr;
        bpf_link__destroy(g_bpf_process_fork_link);
        g_bpf_process_fork_link = nullptr;
        bpf_link__destroy(g_bpf_wakeup_link);
        g_bpf_wakeup_link = nullptr;
        bpf_link__destroy(g_bpf_switch_link);
        g_bpf_switch_link = nullptr;
        bpf_object__close(g_bpf_obj);
        g_bpf_obj = nullptr;
        g_bpf_failed.store(1);
        return false;
    }
    g_installed_target_tgid = -1;
    memset(g_boost_until_ms, 0, sizeof(g_boost_until_ms));
    g_bpf_ready.store(1);
    log_message("eBPF内核聚合与事件刷新已启用");
    return true;
}

void cleanup_bpf() {
    if (g_bpf_ready.load()) {
        uint32_t key = 0;
        uint32_t zero = 0;
        if (g_target_tgid_map_fd >= 0) {
            bpf_map_update_elem(g_target_tgid_map_fd, &key, &zero, BPF_ANY);
        }
        if (g_urgent_flag_map_fd >= 0) {
            bpf_map_update_elem(g_urgent_flag_map_fd, &key, &zero, BPF_ANY);
        }
    }
    detach_frame_probe_locked();
    if (g_process_event_ring) {
        ring_buffer__free(g_process_event_ring);
        g_process_event_ring = nullptr;
    }
    if (g_bpf_process_affinity_link) {
        bpf_link__destroy(g_bpf_process_affinity_link);
        g_bpf_process_affinity_link = nullptr;
    }
    if (g_bpf_process_exec_link) {
        bpf_link__destroy(g_bpf_process_exec_link);
        g_bpf_process_exec_link = nullptr;
    }
    if (g_bpf_process_fork_link) {
        bpf_link__destroy(g_bpf_process_fork_link);
        g_bpf_process_fork_link = nullptr;
    }
    if (g_bpf_wakeup_link) {
        bpf_link__destroy(g_bpf_wakeup_link);
        g_bpf_wakeup_link = nullptr;
    }
    if (g_bpf_switch_link) {
        bpf_link__destroy(g_bpf_switch_link);
        g_bpf_switch_link = nullptr;
    }
    if (g_bpf_obj) {
        bpf_object__close(g_bpf_obj);
        g_bpf_obj = nullptr;
    }
    g_bpf_switch_prog = nullptr;
    g_bpf_wakeup_prog = nullptr;
    g_bpf_process_fork_prog = nullptr;
    g_bpf_process_exec_prog = nullptr;
    g_bpf_process_affinity_prog = nullptr;
    g_target_tgid_map_fd = -1;
    g_thread_runtime_map_fd = -1;
    g_cpu_last_ts_map_fd = -1;
    g_urgent_flag_map_fd = -1;
    g_target_tid_map_fd = -1;
    g_process_event_rb_map_fd = -1;
    g_installed_target_tgid = -1;
    g_bpf_ready.store(0);
}

static pid_t find_foreground_pid_by_package(const char* package) {
    if (!package || package[0] == '\0' || strcmp(package, "unknown") == 0) {
        return -1;
    }

    DIR* dir = opendir("/proc");
    if (!dir) return -1;

    auto read_proc_int_value = [](pid_t pid, const char* name, int fallback) -> int {
        char path[256];
        char buf[64] = {0};
        safe_snprintf(path, sizeof(path), "/proc/%d/%s", pid, name);
        if (!read_text_file(path, buf, sizeof(buf))) return fallback;
        return atoi(buf);
    };

    auto score_foreground_candidate = [&](pid_t pid, bool exact_match, bool fallback_match) -> int {
        int score = score_process_threads_for_scheduler(pid);
        if (score < 0) score = 0;

        int oom_score_adj = read_proc_int_value(pid, "oom_score_adj", 10000);
        int oom_adj = read_proc_int_value(pid, "oom_adj", 10000);
        if (oom_score_adj != 10000 && oom_score_adj <= 103) {
            score += 400;
        } else if (oom_score_adj != 10000 && oom_score_adj <= 200) {
            score += 260;
        } else if (oom_score_adj != 10000 && oom_score_adj >= 500) {
            score -= 400;
        }
        if (oom_adj != 10000 && oom_adj <= 1) {
            score += 220;
        } else if (oom_adj != 10000 && oom_adj >= 8) {
            score -= 240;
        }

        char cgroup_path[256];
        char cgroup_buf[1024] = {0};
        safe_snprintf(cgroup_path, sizeof(cgroup_path), "/proc/%d/cgroup", pid);
        if (read_text_file(cgroup_path, cgroup_buf, sizeof(cgroup_buf))) {
            if (strstr(cgroup_buf, "/top-app") != NULL) {
                score += 320;
            } else if (strstr(cgroup_buf, "/foreground") != NULL) {
                score += 180;
            }
        }

        if (exact_match) {
            score += 500;
        } else if (fallback_match) {
            score += 260;
        }
        return score;
    };

    pid_t best_exact_pid = -1;
    int best_exact_score = -2147483647;
    pid_t best_fallback_pid = -1;
    int best_fallback_score = -2147483647;
    struct dirent* entry = NULL;
    while ((entry = readdir(dir)) != NULL) {
        if (!is_numeric_name(entry->d_name)) continue;
        pid_t pid = (pid_t)atoi(entry->d_name);
        if (pid <= 0) continue;

        char path[256];
        char cmdline[256];
        safe_snprintf(path, sizeof(path), "/proc/%d/cmdline", pid);
        if (!read_text_file(path, cmdline, sizeof(cmdline))) continue;

        std::string raw_cmdline = cmdline;
        std::string trimmed_cmdline = raw_cmdline;
        trim_process_suffix(&trimmed_cmdline[0]);
        trimmed_cmdline.resize(strlen(trimmed_cmdline.c_str()));

        bool exact_match = (raw_cmdline == package);
        bool fallback_match = false;
        if (!exact_match && trimmed_cmdline == package && raw_cmdline.size() > strlen(package)) {
            fallback_match = true;
        }

        if (!exact_match && !fallback_match) {
            continue;
        }

        int score = score_foreground_candidate(pid, exact_match, fallback_match);

        if (exact_match) {
            if (score > best_exact_score || (score == best_exact_score && pid > best_exact_pid)) {
                best_exact_score = score;
                best_exact_pid = pid;
            }
        } else {
            if (score > best_fallback_score || (score == best_fallback_score && pid > best_fallback_pid)) {
                best_fallback_score = score;
                best_fallback_pid = pid;
            }
        }
    }
    closedir(dir);
    return best_exact_pid > 0 ? best_exact_pid : best_fallback_pid;
}

static void find_process_pids_by_package(const char* package, std::vector<pid_t>& out_pids) {
    out_pids.clear();
    if (!package || package[0] == '\0' || strcmp(package, "unknown") == 0) {
        return;
    }

    DIR* dir = opendir("/proc");
    if (!dir) return;

    struct dirent* entry = NULL;
    while ((entry = readdir(dir)) != NULL) {
        if (!is_numeric_name(entry->d_name)) continue;
        pid_t pid = (pid_t)atoi(entry->d_name);
        if (pid <= 0) continue;

        char path[256];
        char cmdline[256];
        safe_snprintf(path, sizeof(path), "/proc/%d/cmdline", pid);
        if (!read_text_file(path, cmdline, sizeof(cmdline))) continue;

        std::string raw_cmdline = cmdline;
        std::string trimmed_cmdline = raw_cmdline;
        trim_process_suffix(&trimmed_cmdline[0]);
        trimmed_cmdline.resize(strlen(trimmed_cmdline.c_str()));
        if (raw_cmdline == package || trimmed_cmdline == package) {
            out_pids.push_back(pid);
        }
    }
    closedir(dir);
    std::sort(out_pids.begin(), out_pids.end());
    out_pids.erase(std::unique(out_pids.begin(), out_pids.end()), out_pids.end());
}

static unsigned long long read_thread_ticks(pid_t pid, pid_t tid) {
    char path[256];
    safe_snprintf(path, sizeof(path), "/proc/%d/task/%d/stat", pid, tid);
    char buf[1024];
    if (!read_text_file(path, buf, sizeof(buf))) return 0;

    char* rparen = strrchr(buf, ')');
    if (!rparen || !rparen[1]) return 0;
    char* fields = rparen + 2;
    char* saveptr = NULL;
    char* token = strtok_r(fields, " ", &saveptr);
    int index = 0;
    unsigned long long utime = 0;
    unsigned long long stime = 0;
    while (token) {
        if (index == 11) utime = strtoull(token, NULL, 10);
        if (index == 12) {
            stime = strtoull(token, NULL, 10);
            break;
        }
        token = strtok_r(NULL, " ", &saveptr);
        index++;
    }
    return utime + stime;
}

static void sample_proc_runtime_locked(
    long long now_ms,
    pid_t pid,
    const char* package,
    pid_t tid,
    unsigned long long total_ticks) {
    long hz = sysconf(_SC_CLK_TCK);
    if (hz <= 0) hz = 100;
    ThreadRuntimeState& state = g_threads[tid];
    if (state.tid == 0) {
        state.pid = pid;
        state.tid = tid;
        state.package = package ? package : "";
        state.discovered_ms = now_ms;
        state.last_ticks = total_ticks;
        state.last_sample_ms = now_ms;
        state.pending_runtime_ns = 0;
        state.total_runtime_ns = 0;
        state.cpu_usage_pct = 0.0f;
        state.cls = THREAD_CLASS_BACKGROUND;
        state.assigned_cluster = -1;
        state.last_classify_ms = 0;
        state.last_migrate_ms = 0;
        state.assigned_cpus.clear();
        state.last_target_cpus.clear();
        state.frozen_cpus.clear();
        state.last_target_cluster = -1;
        state.frozen_cluster = -1;
        state.hot_sample_count = 0;
        state.first_hot_ms = 0;
        state.target_stable_since_ms = 0;
        state.frozen_at_ms = 0;
        state.frozen = false;
        state.last_event_log_ms = 0;
        return;
    }

    state.pid = pid;
    if (package && package[0]) {
        state.package = package;
    }
    long long delta_ms = now_ms - state.last_sample_ms;
    unsigned long long delta_ticks = total_ticks >= state.last_ticks ? (total_ticks - state.last_ticks) : 0;
    if (delta_ms > 0) {
        double thread_ms = ((double)delta_ticks * 1000.0) / (double)hz;
        state.cpu_usage_pct = (float)((thread_ms / (double)delta_ms) * 100.0);
        if (state.cpu_usage_pct < 0.0f) state.cpu_usage_pct = 0.0f;
        if (state.cpu_usage_pct > 400.0f) state.cpu_usage_pct = 400.0f;
    }
    state.last_ticks = total_ticks;
    state.last_sample_ms = now_ms;
}

static void refresh_foreground_pid_locked(bool force_refresh) {
    long long now_ms = get_current_time_ms();
    if (!force_refresh && now_ms - g_last_pid_lookup_ms < 2000) {
        return;
    }
    g_last_pid_lookup_ms = now_ms;
    pid_t new_pid = -1;
    if (!g_foreground_package[0] || strcmp(g_foreground_package, "unknown") == 0) {
        g_foreground_pid = -1;
        return;
    }
    new_pid = find_foreground_pid_by_package(g_foreground_package);
    if (new_pid <= 0) {
        char cpuset_pkg[MAX_PACKAGE_LEN] = {0};
        pid_t cpuset_pid = read_top_app_pid_from_cpuset(cpuset_pkg, sizeof(cpuset_pkg));
        if (cpuset_pid > 0 && cpuset_pkg[0] && strcmp(cpuset_pkg, g_foreground_package) == 0) {
            new_pid = cpuset_pid;
        }
    }
    if (new_pid != g_foreground_pid) {
        if (g_scheduler_session.pid > 0 && g_scheduler_session.pid != new_pid) {
            reset_scheduler_session_locked("foreground_pid_changed");
        }
        char msg[256];
        safe_snprintf(msg, sizeof(msg), "前台PID更新 | app:%s | pid:%d", g_foreground_package, (int)new_pid);
        log_message(msg);
    }
    g_foreground_pid = new_pid;
    if (g_foreground_pid > 0) {
        ensure_scheduler_session_tracking_locked();
    }
}

static void collect_process_threads_for_package_locked(
    pid_t pid,
    const char* package,
    bool use_proc_runtime,
    long long now_ms,
    std::vector<int>& seen) {
    if (pid <= 0 || !package || !package[0]) return;

    char task_path[256];
    safe_snprintf(task_path, sizeof(task_path), "/proc/%d/task", pid);
    DIR* dir = opendir(task_path);
    if (!dir) {
        return;
    }

    struct dirent* entry = NULL;
    while ((entry = readdir(dir)) != NULL) {
        if (!is_numeric_name(entry->d_name)) continue;
        pid_t tid = (pid_t)atoi(entry->d_name);
        if (tid <= 0) continue;

        char comm_path[256];
        char comm_buf[64] = {0};
        safe_snprintf(comm_path, sizeof(comm_path), "/proc/%d/task/%d/comm", pid, tid);
        read_text_file(comm_path, comm_buf, sizeof(comm_buf));
        comm_buf[strcspn(comm_buf, "\r\n")] = '\0';

        ensure_thread_state_for_tid_locked(pid, package, tid, comm_buf);
        ThreadRuntimeState& state = g_threads[tid];
        if (!state.comm.size()) {
            state.comm = comm_buf;
        }
        if (use_proc_runtime) {
            unsigned long long total_ticks = read_thread_ticks(pid, tid);
            sample_proc_runtime_locked(now_ms, pid, package, tid, total_ticks);
        }
        state.pid = pid;
        state.package = package;
        seen.push_back(tid);
    }
    closedir(dir);
}

static void apply_explicit_persistent_actions_for_process_locked(
    pid_t pid,
    const char* package,
    long long now_ms) {
    if (pid <= 0 || !package || !package[0]) return;
    if (should_pause_our_game_scheduler()) return;

    for (auto& kv : g_threads) {
        ThreadRuntimeState& state = kv.second;
        if (state.pid != pid || state.package != package) {
            continue;
        }

        ThreadJsonResolvedRule resolved_rule = {};
        const ThreadJsonResolvedRule* matched_rule = resolve_thread_json_rule_locked(
            package, state.tid, state.comm.c_str(), state.cpu_usage_pct, state.cls, &resolved_rule);
        if (!matched_rule || !matched_rule->action) {
            continue;
        }

        apply_thread_json_action_locked(state, matched_rule);

        // 记录规则应用日志，带节流
        if (now_ms - state.last_event_log_ms > 15000) {
            char log_msg[320];
            safe_snprintf(log_msg, sizeof(log_msg),
                          "persistent规则应用 | app:%s | tid:%d | comm:%s | rule:%s",
                          package, (int)state.tid, state.comm.c_str(), matched_rule->rule_name);
            log_scheduler_probe_once(log_msg);
            state.last_event_log_ms = now_ms;
        }

        if (!(matched_rule->action->has_clusters || matched_rule->action->has_cpus)) {
            continue;
        }

        std::vector<int> explicit_cpus;
        resolve_thread_json_action_cpus_locked(*matched_rule->action, explicit_cpus);
        normalize_cpu_mask(explicit_cpus);
        if (explicit_cpus.empty()) {
            continue;
        }

        int explicit_cluster = highest_cluster_for_mask(explicit_cpus);
        bool need_apply = (state.assigned_cluster != explicit_cluster) ||
                          !cpu_mask_equals(state.assigned_cpus, explicit_cpus);
        if (!need_apply) {
            continue;
        }
        if (state.last_migrate_ms != 0 && now_ms - state.last_migrate_ms < 300) {
            continue;
        }

        if (set_thread_affinity_mask(state.tid, explicit_cpus)) {
            state.assigned_cluster = explicit_cluster;
            state.last_migrate_ms = now_ms;
            state.assigned_cpus = explicit_cpus;
        }
    }
}

static void collect_foreground_threads_locked() {
    long long now_ms = get_current_time_ms();
    int scan_interval_ms = g_bpf_ready.load() ? 400 : 150;
    if (g_last_thread_scan_ms > 0 && now_ms - g_last_thread_scan_ms < scan_interval_ms) {
        return;
    }

    std::vector<int> seen;
    if (g_foreground_pid > 0 && g_foreground_package[0] && strcmp(g_foreground_package, "unknown") != 0) {
        collect_process_threads_for_package_locked(
            g_foreground_pid, g_foreground_package, !g_bpf_ready.load(), now_ms, seen);
    }

    for (size_t i = 0; i < g_thread_app_rules.size(); i++) {
        const ThreadJsonAppRule& rule = g_thread_app_rules[i];
        if (!rule.persistent_scope) continue;
        for (size_t j = 0; j < rule.packages.size(); j++) {
            const std::string& package = rule.packages[j];
            if (package.empty()) continue;
            if (g_foreground_pid > 0 &&
                package == scheduler_to_lower_copy(g_foreground_package)) {
                continue;
            }
            std::vector<pid_t> pids;
            find_process_pids_by_package(package.c_str(), pids);
            for (size_t pid_index = 0; pid_index < pids.size(); pid_index++) {
                collect_process_threads_for_package_locked(
                    pids[pid_index], package.c_str(), true, now_ms, seen);
            }
        }
    }

    g_last_thread_scan_ms = now_ms;

    if (seen.empty()) {
        restore_all_thread_runtime_adjustments_locked("no_scan_targets");
        if (g_scheduler_session.phase != SESSION_IDLE) {
            reset_scheduler_session_locked("no_scan_targets");
        }
        g_threads.clear();
        return;
    }

    if (is_game_package_name(g_foreground_package)) {
        char candidates[256] = {0};
        size_t pos = 0;
        int candidate_count = 0;
        for (auto& kv : g_threads) {
            const ThreadRuntimeState& state = kv.second;
            bool hot_priority = is_hot_priority_thread_locked(state.tid, state.comm.c_str(), state.cpu_usage_pct);
            bool priority_like = hot_priority &&
                                 should_treat_as_priority_game_thread(
                                     g_foreground_package, state.comm.c_str(), state.tid, state.cpu_usage_pct);
            if (!priority_like) {
                continue;
            }
            int written = safe_snprintf(
                candidates + pos,
                sizeof(candidates) - pos,
                "%s%s(%d)",
                pos == 0 ? "" : ", ",
                state.comm.c_str(),
                (int)state.tid);
            if (written <= 0 || (size_t)written >= sizeof(candidates) - pos) {
                break;
            }
            pos += (size_t)written;
            candidate_count++;
            if (candidate_count >= 4) break;
        }

        char msg[512];
        safe_snprintf(
            msg,
            sizeof(msg),
            "线程采样 | app:%s | pid:%d | threads:%d | candidates:%s",
            g_foreground_package,
            (int)g_foreground_pid,
            (int)g_threads.size(),
            candidates[0] ? candidates : "none");
        log_scheduler_probe_once(msg);
    }

    for (auto it = g_threads.begin(); it != g_threads.end(); ) {
        bool found = false;
        for (size_t i = 0; i < seen.size(); i++) {
            if (seen[i] == it->first) {
                found = true;
                break;
            }
        }
        if (!found) {
            clear_latched_primary_tid_locked(it->second.tid, "thread_gone");
            restore_thread_runtime_adjustments_locked(it->second, "thread_gone");
            it = g_threads.erase(it);
        } else {
            ++it;
        }
    }
}

ThreadClass classify_thread(pid_t tid, const char* comm, float cpu_usage_pct) {
    TopologyKind topo = get_topology_kind();
    int thread_dash_rank = get_thread_dash_rank_locked(tid, comm, cpu_usage_pct);
    if (thread_dash_rank >= 0 && thread_dash_rank < 3 && cpu_usage_pct >= 3.0f) {
        return thread_dash_rank == 0 ? THREAD_CLASS_MAIN : THREAD_CLASS_BURST;
    }
    int worker_rank = get_worker_rank_locked(tid, comm, cpu_usage_pct);
    if (is_main_thread_name_for_package(g_foreground_package, comm, tid)) {
        return THREAD_CLASS_MAIN;
    }
    if (is_download_thread_name(g_foreground_package, comm, cpu_usage_pct)) {
        return THREAD_CLASS_BURST;
    }
    if (should_treat_as_priority_game_thread(g_foreground_package, comm, tid, cpu_usage_pct)) {
        return THREAD_CLASS_RENDER;
    }
    if ((worker_rank >= 0 && worker_rank < 2 && cpu_usage_pct >= 2.0f) || is_taskgraph_thread_name(comm)) {
        return THREAD_CLASS_BURST;
    }
    if (should_promote_generic_worker(g_foreground_package, comm, cpu_usage_pct)) {
        return THREAD_CLASS_BURST;
    }
    if (tid == g_foreground_pid && cpu_usage_pct >= 3.0f) {
        return THREAD_CLASS_MAIN;
    }
    std::string name = scheduler_to_lower_copy(comm);
    if ((name.find("binder") != std::string::npos || name.find("pool") != std::string::npos) &&
        cpu_usage_pct < 8.0f) {
        return THREAD_CLASS_BACKGROUND;
    }
    if (topo == TOPOLOGY_6_2 && cpu_usage_pct >= 10.0f) {
        return THREAD_CLASS_BURST;
    }
    if (cpu_usage_pct >= 18.0f) {
        return THREAD_CLASS_BURST;
    }
    return THREAD_CLASS_BACKGROUND;
}

[[maybe_unused]] static const char* thread_class_to_string(ThreadClass cls) {
    switch (cls) {
    case THREAD_CLASS_MAIN:
        return "main";
    case THREAD_CLASS_RENDER:
        return "render";
    case THREAD_CLASS_BURST:
        return "worker";
    case THREAD_CLASS_BACKGROUND:
    default:
        return "other";
    }
}

static void reset_scheduler_session_locked(const char* reason) {
    char old_package[MAX_APP_NAME_LEN];
    safe_strncpy(old_package, g_scheduler_session.package, sizeof(old_package));
    pid_t old_pid = g_scheduler_session.pid;
    g_scheduler_session.phase = SESSION_IDLE;
    g_scheduler_session.pid = -1;
    g_scheduler_session.package[0] = '\0';
    g_scheduler_session.latched_main_tid = 0;
    g_scheduler_session.latched_render_tid = 0;
    g_scheduler_session.last_lost_main_tid = 0;
    g_scheduler_session.last_lost_render_tid = 0;
    g_scheduler_session.latched_main_loss_reason[0] = '\0';
    g_scheduler_session.latched_render_loss_reason[0] = '\0';
    if (reason && reason[0]) {
        char msg[320];
        safe_snprintf(msg, sizeof(msg),
                      "调度会话重置 | app:%s | pid:%d | reason:%s",
                      old_package[0] ? old_package : "none",
                      (int)old_pid,
                      reason);
        log_affinity_message(msg);
        trace_instantf("murong_session", "reset app=%s pid=%d reason=%s",
                       old_package[0] ? old_package : "none",
                       (int)old_pid,
                       reason);
    }
}

[[maybe_unused]] static float avg_idle_ratio(int cluster_id, const AllCpuStats& stats) {
    if (!g_prev_stats_valid) return 1.0f;
    uint64_t idle_delta = 0;
    uint64_t total_delta = 0;
    for (int i = 0; i < cpu_clusters[cluster_id].cpu_count; i++) {
        int cpu_id = cpu_clusters[cluster_id].cpu_ids[i];
        uint64_t idle_now = stats.idle[cpu_id];
        uint64_t total_now = stats.total[cpu_id];
        uint64_t idle_prev = g_prev_stats.idle[cpu_id];
        uint64_t total_prev = g_prev_stats.total[cpu_id];
        if (idle_now >= idle_prev) idle_delta += idle_now - idle_prev;
        if (total_now >= total_prev) total_delta += total_now - total_prev;
    }
    if (total_delta == 0) return 1.0f;
    float ratio = (float)idle_delta / (float)total_delta;
    if (ratio < 0.0f) ratio = 0.0f;
    if (ratio > 1.0f) ratio = 1.0f;
    return ratio;
}

[[maybe_unused]] static float get_freq_headroom(int cluster_id) {
    if (cluster_id < 0 || cluster_id >= cluster_count) return 0.0f;
    int policy_num = cpu_clusters[cluster_id].policies[0];
    char path[256];
    safe_snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpufreq/policy%d/scaling_cur_freq", policy_num);
    FILE* fp = fopen(path, "r");
    int cur_freq = 0;
    if (fp) {
        fscanf(fp, "%d", &cur_freq);
        fclose(fp);
    }
    int min_freq = g_cluster_policies[cluster_id].min_freq;
    int max_freq = g_cluster_policies[cluster_id].max_freq;
    if (max_freq <= min_freq) return 0.0f;
    float headroom = (float)(max_freq - cur_freq) / (float)(max_freq - min_freq);
    if (headroom < 0.0f) headroom = 0.0f;
    if (headroom > 1.0f) headroom = 1.0f;
    return headroom;
}

[[maybe_unused]] static float estimate_thermal_pressure() {
    char buf[64];
    if (!read_text_file("/sys/class/thermal/thermal_zone0/temp", buf, sizeof(buf))) {
        return 0.0f;
    }
    int temp = atoi(buf);
    if (temp <= 0) return 0.0f;
    float celsius = temp > 1000 ? ((float)temp / 1000.0f) : (float)temp;
    float pressure = (celsius - 45.0f) / 35.0f;
    if (pressure < 0.0f) pressure = 0.0f;
    if (pressure > 1.0f) pressure = 1.0f;
    return pressure;
}

static void read_tid_allowed_cpu_list(pid_t tid, char* out, size_t out_size) {
    if (!out || out_size == 0) return;
    out[0] = '\0';

    char path[128];
    safe_snprintf(path, sizeof(path), "/proc/%d/status", tid);
    FILE* fp = fopen(path, "r");
    if (!fp) return;

    char line[256];
    while (fgets(line, sizeof(line), fp)) {
        if (strncmp(line, "Cpus_allowed_list:", 18) == 0) {
            const char* value = line + 18;
            while (*value == ' ' || *value == '\t') value++;
            safe_strncpy(out, value, out_size);
            out[strcspn(out, "\r\n")] = '\0';
            break;
        }
    }
    fclose(fp);
}

static bool write_tid_to_cpuset_tasks(const char* group_name, pid_t tid) {
    if (!group_name || !group_name[0] || tid <= 0) return false;

    char path[128];
    safe_snprintf(path, sizeof(path), "/dev/cpuset/%s/tasks", group_name);
    if (!file_exists(path)) return false;

    char tid_str[32];
    safe_snprintf(tid_str, sizeof(tid_str), "%d", tid);

    FILE* fp = fopen(path, "w");
    if (!fp) return false;
    int ok = fprintf(fp, "%s", tid_str);
    int close_ok = fclose(fp);
    return ok > 0 && close_ok == 0;
}

static bool recover_cpuset_and_retry_affinity(pid_t tid, cpu_set_t* mask, const char* mask_desc) {
    if (tid <= 0 || !mask || !mask_desc) return false;

    char allowed_before[128] = {0};
    char allowed_after[128] = {0};
    read_tid_allowed_cpu_list(tid, allowed_before, sizeof(allowed_before));

    bool moved = write_tid_to_cpuset_tasks("top-app", tid);
    if (!moved) {
        moved = write_tid_to_cpuset_tasks("foreground", tid);
    }
    if (!moved) {
        return false;
    }

    bool ok = sched_setaffinity(tid, sizeof(*mask), mask) == 0;
    read_tid_allowed_cpu_list(tid, allowed_after, sizeof(allowed_after));

    char msg[384];
    safe_snprintf(
        msg,
        sizeof(msg),
        "tid:%d | mask:%s | cpuset_recover:%s | allowed_before:%s | allowed_after:%s",
        tid,
        mask_desc,
        ok ? "ok" : strerror(errno),
        allowed_before[0] ? allowed_before : "unknown",
        allowed_after[0] ? allowed_after : "unknown");
    log_affinity_message(msg);
    return ok;
}

static void get_affinity_log_context_for_tid(
    pid_t tid,
    char* out_app,
    size_t out_app_size,
    char* out_comm,
    size_t out_comm_size,
    char* out_source,
    size_t out_source_size) {
    if (out_app && out_app_size > 0) out_app[0] = '\0';
    if (out_comm && out_comm_size > 0) out_comm[0] = '\0';
    if (out_source && out_source_size > 0) safe_strncpy(out_source, "unknown", out_source_size);

    auto it = g_threads.find(tid);
    if (it == g_threads.end()) {
        return;
    }

    const ThreadRuntimeState& state = it->second;
    if (out_app && out_app_size > 0 && !state.package.empty()) {
        safe_strncpy(out_app, state.package.c_str(), out_app_size);
    }
    if (out_comm && out_comm_size > 0 && !state.comm.empty()) {
        safe_strncpy(out_comm, state.comm.c_str(), out_comm_size);
    }

    if (out_source && out_source_size > 0) {
        if (state.pid > 0 && state.pid == g_foreground_pid &&
            !state.package.empty() && strcmp(state.package.c_str(), g_foreground_package) == 0) {
            safe_strncpy(out_source, "foreground", out_source_size);
        } else if (!state.package.empty()) {
            safe_strncpy(out_source, "persistent", out_source_size);
        }
    }
}

bool set_thread_affinity(pid_t tid, int target_cluster_id) {
    if (tid <= 0 || target_cluster_id < 0 || target_cluster_id >= cluster_count) {
        return false;
    }
    cpu_set_t mask;
    CPU_ZERO(&mask);
    for (int i = 0; i < cpu_clusters[target_cluster_id].cpu_count; i++) {
        CPU_SET(cpu_clusters[target_cluster_id].cpu_ids[i], &mask);
    }
    bool ok = sched_setaffinity(tid, sizeof(mask), &mask) == 0;
    if (!ok && errno == EINVAL) {
        char mask_desc[64];
        safe_snprintf(mask_desc, sizeof(mask_desc), "cluster-%d", target_cluster_id);
        ok = recover_cpuset_and_retry_affinity(tid, &mask, mask_desc);
    }
    char app_buf[MAX_PACKAGE_LEN] = {0};
    char comm_buf[64] = {0};
    char source_buf[32] = {0};
    get_affinity_log_context_for_tid(
        tid, app_buf, sizeof(app_buf), comm_buf, sizeof(comm_buf), source_buf, sizeof(source_buf));
    if (ok) {
        g_runtime_stats.affinity_apply_count.fetch_add(1);
        char msg[256];
        safe_snprintf(msg, sizeof(msg),
                      "tid:%d | app:%s | comm:%s | source:%s | cluster:%d | mask:cluster-%d",
                      tid,
                      app_buf[0] ? app_buf : "unknown",
                      comm_buf[0] ? comm_buf : "unknown",
                      source_buf[0] ? source_buf : "unknown",
                      target_cluster_id,
                      target_cluster_id);
        log_affinity_message(msg);
        log_affinity_success_once(tid, msg);
    } else {
        g_runtime_stats.affinity_fail_count.fetch_add(1);
        char msg[320];
        safe_snprintf(msg, sizeof(msg),
                      "tid:%d | app:%s | comm:%s | source:%s | cluster:%d | setaffinity_failed:%s",
                      tid,
                      app_buf[0] ? app_buf : "unknown",
                      comm_buf[0] ? comm_buf : "unknown",
                      source_buf[0] ? source_buf : "unknown",
                      target_cluster_id,
                      strerror(errno));
        log_affinity_message(msg);
        log_affinity_failure_once(msg);
    }
    return ok;
}

bool set_thread_affinity_mask(pid_t tid, const std::vector<int>& cpus) {
    if (tid <= 0 || cpus.empty()) {
        return false;
    }
    cpu_set_t mask;
    CPU_ZERO(&mask);
    for (size_t i = 0; i < cpus.size(); i++) {
        if (cpus[i] >= 0 && cpus[i] < CPU_SETSIZE) {
            CPU_SET(cpus[i], &mask);
        }
    }
    char cpu_list[128] = {0};
    size_t pos = 0;
    for (size_t i = 0; i < cpus.size(); i++) {
        int written = safe_snprintf(cpu_list + pos, sizeof(cpu_list) - pos, "%s%d", i == 0 ? "" : ",", cpus[i]);
        if (written <= 0 || (size_t)written >= sizeof(cpu_list) - pos) break;
        pos += (size_t)written;
    }
    bool ok = sched_setaffinity(tid, sizeof(mask), &mask) == 0;
    if (!ok && errno == EINVAL) {
        ok = recover_cpuset_and_retry_affinity(tid, &mask, cpu_list);
    }
    char app_buf[MAX_PACKAGE_LEN] = {0};
    char comm_buf[64] = {0};
    char source_buf[32] = {0};
    get_affinity_log_context_for_tid(
        tid, app_buf, sizeof(app_buf), comm_buf, sizeof(comm_buf), source_buf, sizeof(source_buf));
    if (ok) {
        g_runtime_stats.affinity_apply_count.fetch_add(1);
        char msg[384];
        safe_snprintf(msg, sizeof(msg),
                      "tid:%d | app:%s | comm:%s | source:%s | explicit_mask:%s",
                      tid,
                      app_buf[0] ? app_buf : "unknown",
                      comm_buf[0] ? comm_buf : "unknown",
                      source_buf[0] ? source_buf : "unknown",
                      cpu_list);
        log_affinity_message(msg);
        log_affinity_success_once(tid, cpu_list);
    } else {
        g_runtime_stats.affinity_fail_count.fetch_add(1);
        char msg[384];
        safe_snprintf(msg, sizeof(msg),
                      "tid:%d | app:%s | comm:%s | source:%s | explicit_mask:%s | setaffinity_failed:%s",
                      tid,
                      app_buf[0] ? app_buf : "unknown",
                      comm_buf[0] ? comm_buf : "unknown",
                      source_buf[0] ? source_buf : "unknown",
                      cpu_list,
                      strerror(errno));
        log_affinity_message(msg);
        log_affinity_failure_once(msg);
    }
    return ok;
}

static int read_int_from_file(const char* path, int fallback) {
    if (!path || !path[0]) return fallback;
    FILE* fp = fopen(path, "r");
    if (!fp) return fallback;
    int value = fallback;
    if (fscanf(fp, "%d", &value) != 1) {
        value = fallback;
    }
    fclose(fp);
    return value;
}

void update_cluster_freq(int cluster_id, int target_min_khz, int target_max_khz) {
    static int last_min[MAX_CLUSTERS] = {0};
    static int last_max[MAX_CLUSTERS] = {0};
    if (cluster_id < 0 || cluster_id >= cluster_count) return;
    int policy_num = cpu_clusters[cluster_id].policies[0];
    if (target_min_khz < 0) target_min_khz = 0;
    if (target_max_khz > 0 && target_max_khz < target_min_khz) target_max_khz = target_min_khz;
    char path_min[256], path_max[256], min_str[32], max_str[32];
    safe_snprintf(path_min, sizeof(path_min), "/sys/devices/system/cpu/cpufreq/policy%d/scaling_min_freq", policy_num);
    safe_snprintf(path_max, sizeof(path_max), "/sys/devices/system/cpu/cpufreq/policy%d/scaling_max_freq", policy_num);
    int current_min = read_int_from_file(path_min, last_min[cluster_id]);
    int current_max = read_int_from_file(path_max, last_max[cluster_id]);
    if (current_min == target_min_khz && current_max == target_max_khz) {
        last_min[cluster_id] = target_min_khz;
        last_max[cluster_id] = target_max_khz;
        return;
    }
    safe_snprintf(min_str, sizeof(min_str), "%d", target_min_khz);
    safe_snprintf(max_str, sizeof(max_str), "%d", target_max_khz);
    bool lowering = current_min > 0 && target_max_khz > 0 && target_max_khz < current_min;
    if (lowering) {
        if (target_min_khz > 0) lock_val(path_min, min_str);
        if (target_max_khz > 0) lock_val(path_max, max_str);
    } else {
        if (target_max_khz > 0) lock_val(path_max, max_str);
        if (target_min_khz > 0) lock_val(path_min, min_str);
    }
    last_min[cluster_id] = target_min_khz;
    last_max[cluster_id] = target_max_khz;
    trace_instantf("murong_freq", "cluster=%d min=%d max=%d", cluster_id, target_min_khz, target_max_khz);
}

void request_performance_boost(int cluster_id, int duration_ms) {
    if (cluster_id < 0 || cluster_id >= cluster_count || duration_ms <= 0) return;
    g_runtime_stats.boost_request_count.fetch_add(1);
    long long until_ms = get_current_time_ms() + duration_ms;
    if (until_ms > g_boost_until_ms[cluster_id]) {
        g_boost_until_ms[cluster_id] = until_ms;
    }
}

bool is_performance_boost_active(int cluster_id, long long now_ms) {
    if (cluster_id < 0 || cluster_id >= cluster_count) return false;
    return now_ms < g_boost_until_ms[cluster_id];
}

void clear_performance_boosts() {
    memset(g_boost_until_ms, 0, sizeof(g_boost_until_ms));
}

void adjust_policy_by_fps() {
    for (int i = 0; i < cluster_count; i++) {
        g_cluster_policies[i] = g_cluster_policy_base[i];
    }
}
