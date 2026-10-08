static bool read_text_file(const char* path, char* buf, size_t buf_size);
static void refresh_foreground_pid_locked(bool force_refresh);
static void collect_process_threads_for_package_locked(
    pid_t pid,
    const char* package,
    bool use_proc_runtime,
    long long now_ms,
    std::unordered_set<int>& seen);
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
    /* === 静态规则层专用字段 (Phase 1) === */
    bool has_explicit_affinity_rule;     /* 线程是否命中了显式 cpus/clusters 规则 */
    long long last_static_bind_ms;       /* 上次静态绑核时间戳，用于冷却判断 */
    long long static_bind_cooldown_ms;   /* 静态绑核冷却间隔(ms)，默认 3000 */
    /* Phase2: 稳定选择纪元 */
    int selection_epoch;                 /* 当前线程所在的选择纪元版本，用于追踪 top-N 集合变化 */
    /* 锁定守护的让步机制：同一个线程被外部反复改写时，我们不该无限重打 */
    int guard_reapply_streak;            /* 连续重打次数 */
    bool guard_yielded;                  /* 已让位：不再与外部写入者争这条线程 */
    /* 绑核收敛退避：线程被 cpuset 限制、掩码永远无法等于目标时，不要每 500ms 重写一次 */
    int affinity_retry_streak;           /* 连续"写了但没收敛"的次数 */
    long long affinity_yield_until_ms;   /* 退避截止时间 */
};

/* Phase2: 每个(规则+角色)的 limit 选择稳定追踪 */
struct SelectionEpochState {
    int epoch;                       /* 当前选择版本，每次发生实际切换时递增 */
    int last_lookup_loop_tick;       /* 上次计算的循环 tick，避免同轮重复计算 */
    std::vector<pid_t> selected_tids;/* 当前生效的 top-N 选定线程 TID */
    long long stable_since_ms;       /* 当前选定集合稳定开始时间 */
    bool has_pending;                /* 是否有待处理的集合变更 */
    std::vector<pid_t> pending_tids; /* 待处理的新候选集合 */
    long long pending_since_ms;      /* 候选集合首次出现的时间 */
    long long last_reselect_ms;      /* 上次实际选择变更的时间 */
    long long selection_change_threshold_ms; /* Phase5: 规则级阈值，0=使用全局默认 */
};

/* Phase2: 全局稳定阈值 */
static const long long kSelectionChangeThresholdMs = 2000;
static const long long kRebindCooldownMs = 3000;
/* 同一条线程被外部连续改写这么多次之后，锁定守护让位（不再互踩）。 */
static const int kGuardYieldStreak = 5;
static const long long kFastLockMinLearningMs = 10000;
static const long long kFastLockMaxLearningMs = 15000;
static const long long kFastLockStableWindowMs = 2500;

enum ProcessEventType {
    PROCESS_EVENT_FORK = 1,
    PROCESS_EVENT_EXEC = 2,
    PROCESS_EVENT_SET_AFFINITY = 3,
    PROCESS_EVENT_EXIT = 4
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
    /* Phase5: 从原角色规则中复制的稳定参数 */
    int stable_hold_ms;
    int rebind_cooldown_ms;
    int selection_change_threshold_ms;
    bool pin_once;
    bool prefer_existing_binding;
};

struct LockedClassAssignment {
    bool valid;
    int cluster;
    std::vector<int> cpus;
};

enum SchedulerSessionPhase {
    SESSION_IDLE = 0,
    SESSION_LEARNING,
    SESSION_LOCKED,
    SESSION_BACKGROUND_HOLD
};

struct SchedulerSessionState {
    SchedulerSessionPhase phase;
    pid_t pid;
    char package[MAX_APP_NAME_LEN];
    long long learning_started_ms;
    long long locked_at_ms;
    long long background_since_ms;
    long long last_guard_ms;
    pid_t latched_main_tid;
    pid_t latched_render_tid;
    char latched_main_loss_reason[32];
    char latched_render_loss_reason[32];
    LockedClassAssignment class_assignments[4];
};

struct ProcessPackageCacheEntry {
    pid_t pid;
    std::string raw_cmdline;
    std::string base_package;
};

struct ThreadJsonSelectorRankCacheEntry {
    int loop_tick;
    std::vector<pid_t> ranked_tids;
    std::unordered_map<int, int> rank_by_tid;
};

struct ThreadJsonRoleMatchCacheEntry {
    int loop_tick;
    std::unordered_map<int, bool> matched_by_tid;
};

static std::unordered_map<int, ThreadRuntimeState> g_threads;
static std::unordered_map<int, ProcessPackageCacheEntry> g_process_package_cache;
static std::unordered_map<std::string, std::vector<pid_t>> g_process_pids_by_package_cache;
static std::unordered_map<uint64_t, ThreadJsonSelectorRankCacheEntry> g_thread_json_selector_rank_cache;
static std::unordered_map<uint64_t, ThreadJsonRoleMatchCacheEntry> g_thread_json_role_match_cache;
static std::unordered_set<int> g_render_target_tids_installed;
static ClusterPolicy g_cluster_policies[MAX_CLUSTERS];
static int g_cluster_min_overrides[MAX_CLUSTERS] = {0};
static int g_cluster_max_overrides[MAX_CLUSTERS] = {0};
static char g_foreground_package[MAX_APP_NAME_LEN] = "unknown";
static bool g_foreground_explicit_thread_rule_active = false;
static pid_t g_foreground_pid = -1;
static long long g_last_pid_lookup_ms = 0;
static long long g_last_thread_scan_ms = 0;
static long long g_last_process_package_cache_ms = 0;
static long long g_last_thread_self_heal_scan_ms = 0;
static AllCpuStats g_prev_stats = {};
static int g_prev_stats_valid = 0;
/* 动态线程层被显式规则压制时的防抖日志控制 */
static long long g_last_dynamic_suppressed_log_ms = 0;
static pthread_t g_runtime_tid;
static std::atomic<int> g_runtime_thread_started(0);
static std::atomic<int> g_runtime_thread_stop(0);
static std::atomic<int> g_bpf_ready(0);
static std::atomic<int> g_bpf_failed(0);
static long long g_last_bpf_poll_error_ms = 0;
static struct bpf_object* g_bpf_obj = nullptr;
static struct bpf_program* g_bpf_switch_prog = nullptr;
static struct bpf_program* g_bpf_wakeup_prog = nullptr;
static struct bpf_program* g_bpf_queuebuffer_prog = nullptr;
static struct bpf_program* g_bpf_process_fork_prog = nullptr;
static struct bpf_program* g_bpf_process_exec_prog = nullptr;
static struct bpf_program* g_bpf_process_affinity_prog = nullptr;
/* 退出事件：新版 BPF 才有，允许缺失（缺失时退化为低频 /proc 自愈校验）。 */
static struct bpf_program* g_bpf_process_exit_prog = nullptr;
static struct bpf_link* g_bpf_switch_link = nullptr;
static struct bpf_link* g_bpf_wakeup_link = nullptr;
static struct bpf_link* g_bpf_process_fork_link = nullptr;
static struct bpf_link* g_bpf_process_exec_link = nullptr;
static struct bpf_link* g_bpf_process_affinity_link = nullptr;
static struct bpf_link* g_bpf_process_exit_link = nullptr;
static struct bpf_link* g_bpf_queuebuffer_link_v1 = nullptr;
static struct bpf_link* g_bpf_queuebuffer_link_v2 = nullptr;
static struct bpf_link* g_bpf_queuebuffer_link_v3 = nullptr;
static struct bpf_link* g_bpf_queuebuffer_link_v4 = nullptr;

/* 四个 queueBuffer 符号候选（老系统 V1/V2、Android 13+ V3/V4）。 */
static struct bpf_link** queuebuffer_links[] = {
    &g_bpf_queuebuffer_link_v1,
    &g_bpf_queuebuffer_link_v2,
    &g_bpf_queuebuffer_link_v3,
    &g_bpf_queuebuffer_link_v4,
};
static const char* queuebuffer_symbol_names[] = {
    QUEUEBUFFER_SYMBOL_V1,
    QUEUEBUFFER_SYMBOL_V2,
    QUEUEBUFFER_SYMBOL_V3,
    QUEUEBUFFER_SYMBOL_V4,
};
static const int kQueuebufferSymbolCount =
    (int)(sizeof(queuebuffer_symbol_names) / sizeof(queuebuffer_symbol_names[0]));

static int count_queuebuffer_links_locked() {
    int n = 0;
    for (int i = 0; i < kQueuebufferSymbolCount; i++) {
        if (*queuebuffer_links[i]) n++;
    }
    return n;
}
static struct ring_buffer* g_frame_signal_ring = nullptr;
static struct ring_buffer* g_process_event_ring = nullptr;
static int g_target_tgid_map_fd = -1;
static int g_thread_runtime_map_fd = -1;
static int g_cpu_last_ts_map_fd = -1;
static int g_urgent_flag_map_fd = -1;
static int g_target_tid_map_fd = -1;
static int g_frame_signal_rb_map_fd = -1;
static int g_process_event_rb_map_fd = -1;
static bool g_foreground_pid_refresh_pending = false;
/* 事件流健康度：收到过事件说明 BPF → 用户态的通道是活的，
   此时可以把 /proc 全量枚举降级为低频自愈校验。 */
static bool g_process_event_seen = false;
static long long g_last_process_event_ms = 0;
static long long g_last_persistent_event_scan_ms = 0;
static bool g_dynamic_affinity_recheck_pending = false;
static pid_t g_installed_target_tgid = -1;
static pid_t g_queuebuffer_attached_pid = -1;
static char g_queuebuffer_attached_package[MAX_PACKAGE_LEN] = "";
static long long g_boost_until_ms[MAX_CLUSTERS] = {0};
static SchedulerSessionState g_scheduler_session = {};
static long long g_last_render_tid_sync_ms = 0;
static const long long kThreadFreezeLearnMs = 90 * 1000;
static const long long kThreadFreezeStableMs = 15 * 1000;
static const int kThreadFreezeMinSamples = 12;
static const long long kProcessPackageCacheTtlMs = 2000;
/* 事件通道健康时，pid→package 表只做低频兜底校验（15s），不再每 2s 全量重建。 */
static const long long kProcessPackageCacheVerifyMs = 15000;
static const long long kThreadSelfHealRescanMs = 5000;

/* Phase2: 每个(规则指针)的 limit 选择稳定追踪 */
static std::map<uint64_t, SelectionEpochState> g_selection_epochs;
static long long g_last_epoch_stale_cleanup_ms = 0;

void describe_queuebuffer_state(const char* package, char* out, size_t out_size) {
    if (!out || out_size == 0) return;
    out[0] = '\0';

    std::shared_lock<std::shared_mutex> lock_g(g_mutex);
    std::lock_guard<std::mutex> lock_f(g_frame_sample_mutex);

    long long now_ms = get_current_time_ms();
    QueueBufferSurfaceState* primary = select_primary_queuebuffer_surface_locked();
    long long sample_age_ms = (primary && primary->last_wall_ms > 0) ? (now_ms - primary->last_wall_ms) : -1;
    safe_snprintf(
        out, out_size,
        "pkg=%s attached_app=%s pid=%d links=%d/%d signals=%llu accepted=%llu rejected=%llu last_delta_ns=%llu ewma_ms=%.2f surfaces=%d samples=%d age_ms=%lld surface=0x%llx cache_pkg=%s cache_pid=%d",
        package ? package : "unknown",
        g_queuebuffer_attached_package[0] ? g_queuebuffer_attached_package : "none",
        (int)g_queuebuffer_attached_pid,
        count_queuebuffer_links_locked(),
        kQueuebufferSymbolCount,
        (unsigned long long)g_queuebuffer_signal_count,
        (unsigned long long)(primary ? primary->accepted_count : 0),
        (unsigned long long)(primary ? primary->rejected_count : 0),
        (unsigned long long)(primary ? primary->last_delta_ns : 0),
        (double)(primary ? primary->interval_ewma_ms : 0.0),
        (int)g_queuebuffer_surface_states.size(),
        (int)(primary ? primary->frametimes_ms.size() : 0),
        sample_age_ms,
        (unsigned long long)(primary ? primary->surface_key : 0),
        g_queuebuffer_package[0] ? g_queuebuffer_package : "none",
        (int)g_queuebuffer_pid);
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

static void clear_process_package_cache_locked() {
    g_process_package_cache.clear();
    g_process_pids_by_package_cache.clear();
    g_last_process_package_cache_ms = 0;
}

/*
 * 进程退出后把 pid→package 缓存里的对应项删掉。
 * 之前只有全量重建才会清理，所以死掉的 pid 会一直挂在缓存里被当成候选。
 */
static void remove_process_from_package_cache_locked(pid_t pid) {
    if (pid <= 0) return;
    auto it = g_process_package_cache.find((int)pid);
    if (it == g_process_package_cache.end()) return;

    const std::string base = it->second.base_package;
    const std::string raw = it->second.raw_cmdline;
    g_process_package_cache.erase(it);

    auto erase_pid = [pid](std::unordered_map<std::string, std::vector<pid_t>>& map,
                           const std::string& key) {
        if (key.empty()) return;
        auto found = map.find(key);
        if (found == map.end()) return;
        std::vector<pid_t>& pids = found->second;
        pids.erase(std::remove(pids.begin(), pids.end(), pid), pids.end());
        if (pids.empty()) map.erase(found);
    };
    erase_pid(g_process_pids_by_package_cache, base);
    erase_pid(g_process_pids_by_package_cache, raw);
}

static void clear_thread_rule_match_caches_locked() {
    g_thread_json_selector_rank_cache.clear();
    g_thread_json_role_match_cache.clear();
}

static void reset_process_discovery_state_locked(bool reset_foreground_pid, bool request_pid_refresh) {
    if (reset_foreground_pid) {
        g_foreground_pid = -1;
    }
    g_last_pid_lookup_ms = 0;
    g_last_thread_scan_ms = 0;
    clear_process_package_cache_locked();
    clear_thread_rule_match_caches_locked();
    g_last_thread_self_heal_scan_ms = 0;
    g_last_render_tid_sync_ms = 0;
    g_foreground_pid_refresh_pending = request_pid_refresh;
}

static void refresh_process_package_cache_locked(bool force_refresh) {
    long long now_ms = get_current_time_ms();
    if (!force_refresh &&
        g_last_process_package_cache_ms > 0 &&
        now_ms - g_last_process_package_cache_ms < kProcessPackageCacheTtlMs) {
        return;
    }

    /*
     * 稳态下不再周期性重建整张 pid→package 表。
     *
     * 一次重建 = 打开 /proc + 对每个 pid 读一次 cmdline，这正是"每次采样扫一遍
     * 全表"那笔开销。事件通道健康时，新进程由 fork/exec 事件增量写入、退出由
     * exit 事件删除，所以这里只需要低频兜底校验一次，用来修复漏掉的事件。
     */
    const bool incremental_mode =
        g_bpf_ready.load() && g_process_event_seen && !g_process_package_cache.empty();
    if (!force_refresh && incremental_mode) {
        if (g_last_process_package_cache_ms > 0 &&
            now_ms - g_last_process_package_cache_ms < kProcessPackageCacheVerifyMs) {
            return;
        }
    }

    /* 验收用计数：这是真正的一次 /proc 全量枚举。 */
    g_runtime_stats.thread_proc_scan_count.fetch_add(1);

    std::unordered_map<int, ProcessPackageCacheEntry> new_pid_cache;
    std::unordered_map<std::string, std::vector<pid_t>> new_pkg_cache;
    DIR* dir = opendir("/proc");
    if (!dir) {
        return;
    }

    struct dirent* entry = NULL;
    while ((entry = readdir(dir)) != NULL) {
        if (!is_numeric_name(entry->d_name)) continue;
        pid_t pid = (pid_t)atoi(entry->d_name);
        if (pid <= 0) continue;

        char path[256];
        char cmdline[256] = {0};
        safe_snprintf(path, sizeof(path), "/proc/%d/cmdline", pid);
        if (!read_text_file(path, cmdline, sizeof(cmdline)) || !cmdline[0]) {
            continue;
        }

        ProcessPackageCacheEntry item = {};
        item.pid = pid;
        item.raw_cmdline = cmdline;
        item.base_package = item.raw_cmdline;
        trim_process_suffix(&item.base_package[0]);
        item.base_package.resize(strlen(item.base_package.c_str()));
        if (item.base_package.empty()) {
            continue;
        }

        new_pid_cache[pid] = item;
        new_pkg_cache[item.base_package].push_back(pid);
        if (!item.raw_cmdline.empty() && item.raw_cmdline != item.base_package) {
            new_pkg_cache[item.raw_cmdline].push_back(pid);
        }
    }
    closedir(dir);

    for (auto& kv : new_pkg_cache) {
        std::vector<pid_t>& pids = kv.second;
        std::sort(pids.begin(), pids.end());
        pids.erase(std::unique(pids.begin(), pids.end()), pids.end());
    }

    g_process_package_cache.swap(new_pid_cache);
    g_process_pids_by_package_cache.swap(new_pkg_cache);
    g_last_process_package_cache_ms = now_ms;
}

static bool read_process_package_name(pid_t pid, char* out, size_t out_size) {
    if (!out || out_size == 0) return false;
    out[0] = '\0';
    if (pid <= 0) return false;

    refresh_process_package_cache_locked(false);
    auto it = g_process_package_cache.find((int)pid);
    if (it != g_process_package_cache.end() && !it->second.base_package.empty()) {
        safe_strncpy(out, it->second.base_package.c_str(), out_size);
        return true;
    }

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

    ProcessPackageCacheEntry item = {};
    item.pid = pid;
    item.raw_cmdline = cmdline;
    item.base_package = trimmed;
    g_process_package_cache[(int)pid] = item;
    g_process_pids_by_package_cache[item.base_package].push_back(pid);
    if (!item.raw_cmdline.empty() && item.raw_cmdline != item.base_package) {
        g_process_pids_by_package_cache[item.raw_cmdline].push_back(pid);
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
        /*
         * 以下字段以前没有初始化。默认构造只构造了 std::string 成员，
         * 这些数值/布尔成员是"不确定值"，第一次入册就可能被当成有效状态读到
         * （例如 has_explicit_affinity_rule 被读成 true，锁定守护就会永远跳过它）。
         */
        state.last_event_log_ms = 0;
        state.has_explicit_affinity_rule = false;
        state.last_static_bind_ms = 0;
        state.static_bind_cooldown_ms = 0;
        state.selection_epoch = 0;
        state.guard_reapply_streak = 0;
        state.guard_yielded = false;
        state.affinity_retry_streak = 0;
        state.affinity_yield_until_ms = 0;
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
    return g_scheduler_session.pid > 0 &&
           g_foreground_pid == g_scheduler_session.pid &&
           g_scheduler_session.package[0] &&
           strcmp(g_scheduler_session.package, g_foreground_package) == 0;
}

static bool is_latched_session_tid_alive_locked(pid_t tid) {
    if (tid <= 0) return false;
    auto is_tid_visible_in_session_proc = [tid]() -> bool {
        if (g_scheduler_session.pid <= 0) return false;
        char comm_path[128];
        char comm_buf[32];
        safe_snprintf(comm_path, sizeof(comm_path), "/proc/%d/task/%d/comm", (int)g_scheduler_session.pid, (int)tid);
        return read_text_file(comm_path, comm_buf, sizeof(comm_buf));
    };
    auto it = g_threads.find((int)tid);
    if (it != g_threads.end()) {
        const ThreadRuntimeState& state = it->second;
        if (state.pid == g_scheduler_session.pid &&
            !state.package.empty() &&
            strcmp(state.package.c_str(), g_scheduler_session.package) == 0) {
            return true;
        }
    }
    return is_tid_visible_in_session_proc();
}

static char* get_latched_primary_loss_reason_buffer_locked(ThreadClass role) {
    if (role == THREAD_CLASS_MAIN) return g_scheduler_session.latched_main_loss_reason;
    if (role == THREAD_CLASS_RENDER) return g_scheduler_session.latched_render_loss_reason;
    return NULL;
}

static void set_latched_primary_loss_reason_locked(ThreadClass role, const char* reason) {
    char* buffer = get_latched_primary_loss_reason_buffer_locked(role);
    if (!buffer) return;
    if (reason && reason[0]) {
        safe_strncpy(buffer, reason, 32);
    } else {
        buffer[0] = '\0';
    }
}

static const char* get_latched_primary_loss_reason_locked(ThreadClass role) {
    char* buffer = get_latched_primary_loss_reason_buffer_locked(role);
    if (!buffer || !buffer[0]) return "unknown";
    return buffer;
}

static void invalidate_latched_primary_assignment_locked(ThreadClass role, const char* reason) {
    if (role != THREAD_CLASS_MAIN && role != THREAD_CLASS_RENDER) return;
    LockedClassAssignment& assignment = g_scheduler_session.class_assignments[(int)role];
    assignment.valid = false;
    assignment.cluster = -1;
    assignment.cpus.clear();
    set_latched_primary_loss_reason_locked(role, reason);
    g_scheduler_session.last_guard_ms = 0;
    g_dynamic_affinity_recheck_pending = true;
}

static void clear_invalid_latched_primary_tids_locked() {
    if (g_scheduler_session.latched_main_tid > 0 &&
        !is_latched_session_tid_alive_locked(g_scheduler_session.latched_main_tid)) {
        invalidate_latched_primary_assignment_locked(THREAD_CLASS_MAIN, "tid_not_alive");
        g_scheduler_session.latched_main_tid = 0;
    }
    if (g_scheduler_session.latched_render_tid > 0 &&
        !is_latched_session_tid_alive_locked(g_scheduler_session.latched_render_tid)) {
        invalidate_latched_primary_assignment_locked(THREAD_CLASS_RENDER, "tid_not_alive");
        g_scheduler_session.latched_render_tid = 0;
    }
}

static bool is_latched_main_tid_locked(pid_t tid) {
    clear_invalid_latched_primary_tids_locked();
    return tid > 0 && g_scheduler_session.latched_main_tid == tid;
}

static bool is_latched_render_tid_locked(pid_t tid) {
    clear_invalid_latched_primary_tids_locked();
    return tid > 0 && g_scheduler_session.latched_render_tid == tid;
}

static bool is_latched_primary_tid_locked(pid_t tid) {
    return is_latched_main_tid_locked(tid) || is_latched_render_tid_locked(tid);
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
    invalidate_latched_primary_assignment_locked(role_class, reason);
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
    if (!is_scheduler_session_foreground_locked()) return;
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
    set_latched_primary_loss_reason_locked(is_main ? THREAD_CLASS_MAIN : THREAD_CLASS_RENDER, "");
    char msg[256];
    safe_snprintf(msg, sizeof(msg),
                  "锁定线程绑定 | role:%s | app:%s | pid:%d | tid:%d | comm:%s",
                  role,
                  g_scheduler_session.package,
                  (int)g_scheduler_session.pid,
                  (int)state.tid,
                  state.comm.c_str());
    log_affinity_message(msg);
}

static ThreadClass apply_latched_primary_role_override_locked(
    const ThreadRuntimeState& state,
    ThreadClass current_role) {
    if (!is_scheduler_session_foreground_locked()) {
        return current_role;
    }
    clear_invalid_latched_primary_tids_locked();

    if (state.tid == g_scheduler_session.latched_main_tid) {
        return THREAD_CLASS_MAIN;
    }
    if (state.tid == g_scheduler_session.latched_render_tid) {
        return THREAD_CLASS_RENDER;
    }

    bool is_main = is_main_thread_name_for_package(g_foreground_package, state.comm.c_str(), state.tid);
    if (is_main && g_scheduler_session.latched_main_tid > 0) {
        return THREAD_CLASS_BURST;
    }

    bool is_render = !is_main &&
                     is_game_engine_priority_thread(g_foreground_package, state.comm.c_str(), state.tid);
    if (is_render && g_scheduler_session.latched_render_tid > 0) {
        return THREAD_CLASS_BURST;
    }
    return current_role;
}

static bool has_locked_primary_reselection_candidate_locked(ThreadClass role) {
    if (role != THREAD_CLASS_MAIN && role != THREAD_CLASS_RENDER) return false;
    if (!is_scheduler_session_foreground_locked()) return false;
    for (auto& kv : g_threads) {
        const ThreadRuntimeState& state = kv.second;
        if (state.pid != g_scheduler_session.pid) continue;
        if (state.package != g_scheduler_session.package) continue;
        if (state.assigned_cluster < 0) continue;
        bool is_main =
            is_main_thread_name_for_package(g_scheduler_session.package, state.comm.c_str(), state.tid);
        if (role == THREAD_CLASS_MAIN) {
            if (is_main) {
                return true;
            }
            continue;
        }
        if (!is_main &&
            is_game_engine_priority_thread(g_scheduler_session.package, state.comm.c_str(), state.tid)) {
            return true;
        }
    }
    return false;
}

static void log_locked_primary_reselection_locked(ThreadClass role) {
    if (role != THREAD_CLASS_MAIN && role != THREAD_CLASS_RENDER) return;
    const char* role_name = role == THREAD_CLASS_MAIN ? "main" : "render";
    const char* reason = get_latched_primary_loss_reason_locked(role);
    char msg[256];
    safe_snprintf(msg, sizeof(msg),
                  "锁定线程补选 | role:%s | app:%s | pid:%d | reason:%s",
                  role_name,
                  g_scheduler_session.package[0] ? g_scheduler_session.package : "unknown",
                  (int)g_scheduler_session.pid,
                  reason);
    log_affinity_message(msg);
    trace_instantf("murong_session", "reselect_%s app=%s pid=%d reason=%s",
                   role_name,
                   g_scheduler_session.package[0] ? g_scheduler_session.package : "unknown",
                   (int)g_scheduler_session.pid,
                   reason);
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
        int cpu = expected_cpus[i];
        if (cpu >= 0 && cpu < CPU_SETSIZE) {
            CPU_SET(cpu, &expected_mask);
        }
    }
    if (sched_getaffinity(tid, sizeof(actual_mask), &actual_mask) != 0) {
        return false;
    }
    return CPU_EQUAL(&expected_mask, &actual_mask);
}

static bool is_thread_freeze_candidate_locked(pid_t tid, const char* comm, float cpu_usage_pct) {
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

static void update_thread_target_stability_locked(
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

static bool should_freeze_thread_locked(
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
    size_t range_open = pattern.find('[');
    size_t range_close = pattern.find(']');
    if (range_open != std::string::npos &&
        range_close != std::string::npos &&
        range_close > range_open + 1) {
        std::string range_text = pattern.substr(range_open + 1, range_close - range_open - 1);
        size_t dash = range_text.find('-');
        bool digits_valid = dash != std::string::npos &&
                            dash > 0 &&
                            dash + 1 < range_text.size();
        if (digits_valid) {
            bool left_digits = true;
            bool right_digits = true;
            for (size_t i = 0; i < dash; i++) {
                if (!isdigit((unsigned char)range_text[i])) {
                    left_digits = false;
                    break;
                }
            }
            for (size_t i = dash + 1; i < range_text.size(); i++) {
                if (!isdigit((unsigned char)range_text[i])) {
                    right_digits = false;
                    break;
                }
            }
            if (left_digits && right_digits) {
                std::string prefix = trim_copy(pattern.substr(0, range_open));
                std::string suffix = trim_copy(pattern.substr(range_close + 1));
                if (lowered_text.size() >= prefix.size() + suffix.size() &&
                    lowered_text.rfind(prefix, 0) == 0 &&
                    (suffix.empty() ||
                     lowered_text.compare(lowered_text.size() - suffix.size(),
                                          suffix.size(),
                                          suffix) == 0)) {
                    size_t middle_start = prefix.size();
                    size_t middle_len = lowered_text.size() - prefix.size() - suffix.size();
                    std::string middle = trim_copy(lowered_text.substr(middle_start, middle_len));
                    bool middle_digits = !middle.empty();
                    for (size_t i = 0; i < middle.size(); i++) {
                        if (!isdigit((unsigned char)middle[i])) {
                            middle_digits = false;
                            break;
                        }
                    }
                    if (middle_digits) {
                        int start_value = atoi(range_text.substr(0, dash).c_str());
                        int end_value = atoi(range_text.substr(dash + 1).c_str());
                        int current_value = atoi(middle.c_str());
                        if (current_value >= start_value && current_value <= end_value) {
                            return true;
                        }
                    }
                }
            }
        }
    }
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
    if (!package || !package[0] || strcmp(package, "unknown") == 0) {
        return;
    }
    if (g_active_thread_app_rule_index < 0 ||
        g_active_thread_app_rule_index >= (int)g_thread_app_rules.size()) {
        return;
    }
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

static bool active_thread_app_rule_uses_explicit_affinity_locked() {
    if (g_active_thread_app_rule_index < 0 ||
        g_active_thread_app_rule_index >= (int)g_thread_app_rules.size()) {
        return false;
    }
    if (!is_custom_thread_enabled_for_app(g_foreground_package)) {
        return false;
    }
    return thread_json_app_rule_has_explicit_affinity_locked(
        g_thread_app_rules[(size_t)g_active_thread_app_rule_index]);
}

static bool should_suppress_dynamic_thread_scheduler_for_package_locked(const char* package) {
    if (!package || !package[0] || strcmp(package, "unknown") == 0) {
        return false;
    }
    if (!g_foreground_explicit_thread_rule_active) {
        return false;
    }
    if (!g_foreground_package[0] || strcmp(package, g_foreground_package) != 0) {
        return false;
    }
    return is_custom_thread_enabled_for_app(package);
}

static void apply_active_scheduler_session_override_locked() {
    g_scheduler_session_config = g_scheduler_session_config_base;
    const SceneCategoryRule* scene_rule = find_scene_category_rule_global(
        g_foreground_package, g_scene_policy.foreground_category);
    if (scene_rule && scene_rule->has_session) {
        if (scene_rule->session.learning_duration_ms >= 10000) {
            g_scheduler_session_config.learning_duration_ms = scene_rule->session.learning_duration_ms;
        }
        if (scene_rule->session.guard_interval_ms >= 100) {
            g_scheduler_session_config.guard_interval_ms = scene_rule->session.guard_interval_ms;
        }
    }
    if (g_active_thread_app_rule_index < 0 ||
        g_active_thread_app_rule_index >= (int)g_thread_app_rules.size()) {
        return;
    }
    if (!is_custom_thread_enabled_for_app(g_foreground_package)) {
        return;
    }
    const ThreadJsonAppRule& app_rule = g_thread_app_rules[(size_t)g_active_thread_app_rule_index];
    if (!app_rule.has_session) {
        return;
    }
    if (app_rule.session.learning_duration_ms >= 10000) {
        g_scheduler_session_config.learning_duration_ms = app_rule.session.learning_duration_ms;
    }
    if (app_rule.session.guard_interval_ms >= 100) {
        g_scheduler_session_config.guard_interval_ms = app_rule.session.guard_interval_ms;
    }
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

static uint64_t make_thread_json_selector_rank_cache_key_locked(
    const ThreadJsonSelector& selector,
    pid_t pid) {
    uint64_t selector_key = (uint64_t)(uintptr_t)&selector;
    uint64_t pid_key = (uint64_t)(uint32_t)pid;
    return (selector_key >> 4) ^ (pid_key << 32) ^ pid_key;
}

static uint64_t make_thread_json_role_match_cache_key_locked(
    const ThreadJsonRoleRule& rule,
    pid_t pid) {
    uint64_t rule_key = (uint64_t)(uintptr_t)&rule;
    uint64_t pid_key = (uint64_t)(uint32_t)pid;
    return (rule_key >> 4) ^ (pid_key << 32) ^ pid_key;
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
    int loop_tick = g_runtime_stats.loop_ticks.load();
    uint64_t cache_key = make_thread_json_selector_rank_cache_key_locked(selector, target_state.pid);
    ThreadJsonSelectorRankCacheEntry& cache = g_thread_json_selector_rank_cache[cache_key];
    if (cache.loop_tick != loop_tick) {
        cache.loop_tick = loop_tick;
        cache.ranked_tids.clear();
        cache.rank_by_tid.clear();

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
        cache.ranked_tids.reserve(ranked.size());
        cache.rank_by_tid.reserve(ranked.size());
        for (size_t i = 0; i < ranked.size(); i++) {
            pid_t ranked_tid = ranked[i].second;
            cache.ranked_tids.push_back(ranked_tid);
            cache.rank_by_tid[(int)ranked_tid] = (int)i + 1;
        }
    }
    auto rank_it = cache.rank_by_tid.find((int)tid);
    if (rank_it != cache.rank_by_tid.end()) {
        return rank_it->second;
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

/* Phase2: 清理不再使用的 epoch 状态（防内存泄漏） */
static void cleanup_stale_selection_epochs_locked() {
    long long now_ms = get_current_time_ms();
    if (now_ms - g_last_epoch_stale_cleanup_ms < 60000) return;
    g_last_epoch_stale_cleanup_ms = now_ms;
    for (auto it = g_selection_epochs.begin(); it != g_selection_epochs.end(); ) {
        // 检查 epoch 中的 TID 是否还有效
        const SelectionEpochState& st = it->second;
        bool any_alive = false;
        for (pid_t t : st.selected_tids) {
            if (g_threads.find((int)t) != g_threads.end()) {
                any_alive = true;
                break;
            }
        }
        if (!any_alive) {
            it = g_selection_epochs.erase(it);
        } else {
            ++it;
        }
    }
}

/* Phase2: 带稳定选择纪元的 limit 判定 — 解决 *thread* + limit=N 频繁换人问题 */
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

    /*
     * 稳定集合里的 tid 全部退出时（游戏重启、切前后台、进程被杀都会这样），
     * 必须当场重选：否则这条 limit 规则会一直"匹配不到任何线程"，
     * 表现就是"配置里明明写了 main/gfx，实际一条都没绑上去"，
     * 而且只有重启 daemon 才会恢复（实测踩过）。
     */
    if (!computed_this_tick && !epoch.selected_tids.empty()) {
        bool any_alive = false;
        for (size_t i = 0; i < epoch.selected_tids.size(); i++) {
            if (kill(epoch.selected_tids[i], 0) == 0) { any_alive = true; break; }
        }
        if (!any_alive) {
            epoch.selected_tids.clear();
            epoch.has_pending = false;
            epoch.pending_tids.clear();
        }
    }

    if (!computed_this_tick) {
        /* 计算当前 top-N */
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
            /* 首次初始化 */
            epoch.epoch = 1;
            epoch.selected_tids = current_top;
            epoch.stable_since_ms = now_ms;
            epoch.last_reselect_ms = now_ms;
            /* Phase5: 规则级阈值覆盖 > 全局默认 */
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
                    /* 首次检测到变化 */
                    epoch.has_pending = true;
                    epoch.pending_tids = current_top;
                    epoch.pending_since_ms = now_ms;
                } else if (epoch.pending_tids == current_top) {
                    /* 变化持续中，检查是否超阈值（Phase5: 规则级 > 全局） */
                    if (now_ms - epoch.pending_since_ms >= epoch.selection_change_threshold_ms) {
                        epoch.epoch++;
                        epoch.selected_tids = current_top;
                        epoch.stable_since_ms = now_ms;
                        epoch.last_reselect_ms = now_ms;
                        epoch.has_pending = false;
                        epoch.pending_tids.clear();
                        char msg[256];
                        safe_snprintf(msg, sizeof(msg),
                            "稳定选择切换 | epoch:%d | limit:%d | count:%d",
                            epoch.epoch, rule.limit, (int)current_top.size());
                        log_scheduler_probe_once(msg);
                    }
                } else {
                    /* 还在振荡 → 重新计时 */
                    epoch.pending_tids = current_top;
                    epoch.pending_since_ms = now_ms;
                }
            }
        }
        epoch.last_lookup_loop_tick = loop_tick;
    }

    /* 判断 tid 是否在当前稳定集合中 */
    for (size_t i = 0; i < epoch.selected_tids.size(); i++) {
        if (epoch.selected_tids[i] == (pid_t)tid) {
            target_it->second.selection_epoch = epoch.epoch;
            return true;
        }
    }
    return false;
}

/* 清理陈旧的 epoch 状态（主动调用点） */
static void clear_all_selection_epochs_locked() {
    g_selection_epochs.clear();
}

static bool thread_json_role_matches_locked(
    const ThreadJsonRoleRule& rule,
    pid_t tid,
    const char* comm) {
    if (!rule.enabled || rule.selectors.empty()) return false;
    if (rule.loading_only && !g_scene_policy.game_loading) return false;
    auto target_it = g_threads.find((int)tid);
    if (target_it == g_threads.end()) {
        if (!thread_json_role_selector_match_locked(rule, tid, comm)) return false;
        return thread_json_role_limit_allowed_locked(rule, tid);
    }

    const ThreadRuntimeState& target_state = target_it->second;
    int loop_tick = g_runtime_stats.loop_ticks.load();
    uint64_t cache_key = make_thread_json_role_match_cache_key_locked(rule, target_state.pid);
    ThreadJsonRoleMatchCacheEntry& cache = g_thread_json_role_match_cache[cache_key];
    if (cache.loop_tick != loop_tick) {
        cache.loop_tick = loop_tick;
        cache.matched_by_tid.clear();

        std::vector<pid_t> selector_matched_tids;
        for (auto& kv : g_threads) {
            const ThreadRuntimeState& other = kv.second;
            if (other.pid != target_state.pid) continue;
            if (other.package != target_state.package) continue;
            if (!thread_json_role_selector_match_locked(rule, other.tid, other.comm.c_str())) {
                continue;
            }
            selector_matched_tids.push_back(other.tid);
        }

        cache.matched_by_tid.reserve(selector_matched_tids.size());
        if (rule.limit <= 0) {
            for (size_t i = 0; i < selector_matched_tids.size(); i++) {
                cache.matched_by_tid[(int)selector_matched_tids[i]] = true;
            }
        } else {
            for (size_t i = 0; i < selector_matched_tids.size(); i++) {
                pid_t matched_tid = selector_matched_tids[i];
                if (thread_json_role_limit_allowed_locked(rule, matched_tid)) {
                    cache.matched_by_tid[(int)matched_tid] = true;
                }
            }
        }
    }

    auto match_it = cache.matched_by_tid.find((int)tid);
    return match_it != cache.matched_by_tid.end() && match_it->second;
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
                /* Phase5: 复制角色级稳定参数 */
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
                /* Phase5: 复制自定义规则稳定参数 */
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

/*
 * 线程原始 CPU 掩码的账本。
 *
 * 独立于 g_threads：线程记录会被清理/重建（例如切场景、失焦后再回来），
 * 如果把基线存在 ThreadRuntimeState 里，重建时会把我们自己写下的窄掩码当成
 * "原始掩码"，撤回就会越撤越窄。这里按 TID 长期保存，只有线程真正退出才丢弃。
 */
static std::unordered_map<pid_t, cpu_set_t> g_affinity_baseline_masks;
static std::unordered_set<pid_t> g_affinity_dirty_tids;

/* 必须在第一次 sched_setaffinity 之前调用，否则存下的就是我们自己的掩码。 */
static bool capture_thread_affinity_baseline_locked(pid_t tid) {
    if (tid <= 0) return false;
    if (g_affinity_baseline_masks.find(tid) != g_affinity_baseline_masks.end()) {
        return true;
    }

    /* 账本上限保护：偶尔清理已经退出的线程，避免跨场景长期运行后无上限增长。 */
    if (g_affinity_baseline_masks.size() >= 2048) {
        for (auto it = g_affinity_baseline_masks.begin(); it != g_affinity_baseline_masks.end();) {
            if (it->first != tid && kill(it->first, 0) != 0 && errno == ESRCH) {
                g_affinity_dirty_tids.erase(it->first);
                it = g_affinity_baseline_masks.erase(it);
            } else {
                ++it;
            }
        }
    }

    cpu_set_t mask;
    CPU_ZERO(&mask);
    if (sched_getaffinity(tid, sizeof(mask), &mask) != 0) {
        return false;
    }
    g_affinity_baseline_masks[tid] = mask;
    return true;
}

static void mark_thread_affinity_dirty_locked(pid_t tid) {
    if (tid > 0) {
        g_affinity_dirty_tids.insert(tid);
    }
}

static void forget_thread_affinity_locked(pid_t tid) {
    g_affinity_baseline_masks.erase(tid);
    g_affinity_dirty_tids.erase(tid);
}

/* cpuset 成员身份账本：tid → 我们改动前它所属的 cpuset 组名。 */
static std::unordered_map<pid_t, std::string> g_cpuset_membership_baseline;
static std::unordered_set<pid_t> g_cpuset_dirty_tids;

static std::string read_tid_cpuset_group_locked(pid_t tid) {
    char path[128];
    safe_snprintf(path, sizeof(path), "/proc/%d/cgroup", tid);

    FILE* fp = fopen(path, "r");
    if (!fp) return std::string();

    std::string group;
    char line[512];
    while (fgets(line, sizeof(line), fp)) {
        /* 形如: 5:cpuset:/top-app */
        if (strstr(line, ":cpuset:") == NULL) continue;
        const char* colon = strrchr(line, ':');
        if (!colon || !colon[1]) continue;
        group = colon + 1;
        group.erase(group.find_last_not_of("\r\n") + 1);
        break;
    }
    fclose(fp);
    return group;
}

static void record_cpuset_membership_baseline_locked(pid_t tid) {
    if (tid <= 0) return;
    if (g_cpuset_membership_baseline.find(tid) != g_cpuset_membership_baseline.end()) {
        return;
    }

    if (g_cpuset_membership_baseline.size() >= 2048) {
        for (auto it = g_cpuset_membership_baseline.begin(); it != g_cpuset_membership_baseline.end();) {
            if (it->first != tid && kill(it->first, 0) != 0 && errno == ESRCH) {
                g_cpuset_dirty_tids.erase(it->first);
                it = g_cpuset_membership_baseline.erase(it);
            } else {
                ++it;
            }
        }
    }

    std::string group = read_tid_cpuset_group_locked(tid);
    if (group.empty()) return;
    g_cpuset_membership_baseline[tid] = group;
    g_cpuset_dirty_tids.insert(tid);
}

static void forget_thread_cpuset_locked(pid_t tid) {
    g_cpuset_dirty_tids.erase(tid);
    g_cpuset_membership_baseline.erase(tid);
}

/* 解析 "0-5,7" 这类 CPU 列表到掩码。 */
static bool parse_cpu_list_text_to_mask(const char* text, cpu_set_t* out_mask) {
    if (!text || !out_mask) return false;
    CPU_ZERO(out_mask);
    int count = 0;
    const char* p = text;
    while (*p) {
        while (*p == ' ' || *p == '\t' || *p == ',' || *p == '\n' || *p == '\r') p++;
        if (!*p) break;
        char* end = NULL;
        long first = strtol(p, &end, 10);
        if (end == p) break;
        long last = first;
        p = end;
        if (*p == '-') {
            p++;
            long parsed = strtol(p, &end, 10);
            if (end == p) break;
            last = parsed;
            p = end;
        }
        if (first < 0) first = 0;
        if (last >= CPU_SETSIZE) last = CPU_SETSIZE - 1;
        for (long cpu = first; cpu <= last; cpu++) {
            if (!CPU_ISSET((int)cpu, out_mask)) {
                CPU_SET((int)cpu, out_mask);
                count++;
            }
        }
    }
    return count > 0;
}

/*
 * 线程"真正允许跑的核" = 它所属 cpuset 分组的 cpus。
 *
 * 新线程诞生时带着的掩码是从父线程继承来的，绝不能拿它当基线：父线程被我们绑到
 * 1-4 之后，子线程一出生掩码就是 1-4，撤回时"恢复到 1-4"等于没撤（实测 233 条
 * 线程里约 20 条是这样）。cpuset 才是真正的外部约束，所以基线取 cpuset 允许集。
 */
static bool resolve_thread_allowed_mask_locked(pid_t tid, cpu_set_t* out_mask) {
    if (tid <= 0 || !out_mask) return false;
    CPU_ZERO(out_mask);

    std::string group = read_tid_cpuset_group_locked(tid);
    size_t start = group.find_first_not_of('/');
    if (start != std::string::npos) {
        size_t end = group.find('/', start);
        std::string name = group.substr(start, end == std::string::npos ? std::string::npos : end - start);
        if (!name.empty()) {
            char path[160];
            char text[128] = {0};
            safe_snprintf(path, sizeof(path), "/dev/cpuset/%s/cpus", name.c_str());
            if (read_text_file(path, text, sizeof(text)) &&
                parse_cpu_list_text_to_mask(text, out_mask)) {
                return true;
            }
        }
    }

    /* 拿不到 cpuset 就退化为"全部在线核"。 */
    for (int c = 0; c < cluster_count; c++) {
        for (int i = 0; i < cpu_clusters[c].cpu_count; i++) {
            int cpu = cpu_clusters[c].cpu_ids[i];
            if (cpu >= 0 && cpu < CPU_SETSIZE) CPU_SET(cpu, out_mask);
        }
    }
    return true;
}

/*
 * 在第一次给新线程写掩码之前，把账本基线设成"它 cpuset 允许的核"。
 * 之后 set_thread_affinity*() 里的基线捕获会因为已有记录而跳过，
 * 于是撤回时回到的是 cpuset 允许集，而不是父线程的窄掩码。
 */
static void seed_newborn_affinity_baseline_locked(pid_t tid) {
    if (tid <= 0) return;
    if (g_affinity_baseline_masks.find(tid) != g_affinity_baseline_masks.end()) return;

    cpu_set_t allowed;
    if (!resolve_thread_allowed_mask_locked(tid, &allowed)) return;
    g_affinity_baseline_masks[tid] = allowed;
    g_affinity_dirty_tids.insert(tid);
}

/*
 * 把线程放回它原本的 cpuset 分组。与掩码恢复一样，只对"确实被我们挪过"的线程动手。
 * 返回 true 表示这一次做了搬迁（无论目标组是否与我们记录的一致）。
 */
static bool restore_thread_cpuset_membership_locked(pid_t tid, const char* reason) {
    if (tid <= 0) return false;
    if (g_cpuset_dirty_tids.find(tid) == g_cpuset_dirty_tids.end()) return false;

    if (kill(tid, 0) != 0 && errno == ESRCH) {
        g_cpuset_dirty_tids.erase(tid);
        g_cpuset_membership_baseline.erase(tid);
        return false;
    }

    auto it = g_cpuset_membership_baseline.find(tid);
    if (it == g_cpuset_membership_baseline.end()) {
        g_cpuset_dirty_tids.erase(tid);
        return false;
    }

    /* 组名形如 "/top-app" 或 "/top-app/sub"，这里取第一段用于写回。 */
    std::string group = it->second;
    size_t start = group.find_first_not_of('/');
    if (start == std::string::npos) {
        g_cpuset_dirty_tids.erase(tid);
        return false;
    }
    size_t end = group.find('/', start);
    group = group.substr(start, end == std::string::npos ? std::string::npos : end - start);
    if (group.empty()) {
        g_cpuset_dirty_tids.erase(tid);
        return false;
    }

    char path[160];
    safe_snprintf(path, sizeof(path), "/dev/cpuset/%s/tasks", group.c_str());
    if (!file_exists(path)) {
        g_cpuset_dirty_tids.erase(tid);
        return false;
    }

    char tid_str[32];
    safe_snprintf(tid_str, sizeof(tid_str), "%d", tid);
    FILE* fp = fopen(path, "w");
    if (!fp) {
        char msg[256];
        safe_snprintf(msg, sizeof(msg),
                      "恢复线程 cpuset 失败 | tid:%d | group:%s | reason:%s | err:%s",
                      (int)tid, group.c_str(), reason ? reason : "unknown", strerror(errno));
        log_affinity_message(msg);
        return false;
    }
    int ok = fprintf(fp, "%s", tid_str);
    int close_ok = fclose(fp);
    if (ok <= 0 || close_ok != 0) {
        char msg[256];
        safe_snprintf(msg, sizeof(msg),
                      "恢复线程 cpuset 失败 | tid:%d | group:%s | reason:%s | err:%s",
                      (int)tid, group.c_str(), reason ? reason : "unknown", strerror(errno));
        log_affinity_message(msg);
        return false;
    }

    g_cpuset_dirty_tids.erase(tid);
    return true;
}

/*
 * 把线程掩码撤回原始值（通常就是全核 0-7）。只对"确实被我们绑过"的线程动手，
 * 避免把官方调度器自己设的掩码一起覆盖掉。
 */
static bool restore_thread_affinity_locked(pid_t tid, const char* reason) {
    if (tid <= 0) return false;
    if (g_affinity_dirty_tids.find(tid) == g_affinity_dirty_tids.end()) return false;

    /* 线程已经退出：救不回来，也不该报错刷屏，直接丢弃账本。 */
    if (kill(tid, 0) != 0 && errno == ESRCH) {
        forget_thread_affinity_locked(tid);
        forget_thread_cpuset_locked(tid);
        return false;
    }

    auto it = g_affinity_baseline_masks.find(tid);
    if (it == g_affinity_baseline_masks.end()) {
        g_affinity_dirty_tids.erase(tid);
        return false;
    }

    if (sched_setaffinity(tid, sizeof(it->second), &it->second) == 0) {
        g_affinity_dirty_tids.erase(tid);
        return true;
    }

    char msg[256];
    safe_snprintf(msg, sizeof(msg),
                  "恢复线程绑核失败 | tid:%d | reason:%s | err:%s",
                  (int)tid,
                  reason ? reason : "unknown",
                  strerror(errno));
    log_affinity_message(msg);
    return false;
}

/*
 * 官方的 pipeline 通道（COSA → HAL → 内核 sa_pipeline）。
 *
 * 内核节点 /proc/oplus_scheduler/sched_assist/pipeline_pids_cpus 接受 "pid cpu pid cpu ..."
 * （最多 6 对，空则 -1），sa_pipeline.c 解析后：cpu == 最大核 → prime task 并触发 pipeline_set_boost。
 * 注意它**只打标记、不绑核**（实测写进去前后 Cpus_allowed_list 不变），
 * 所以绑核始终由我们按配置里的 cpus 字面执行，这个通道只负责补标记。
 * 只有 ColorOS/OPLUS 内核有这个节点；其它机型检测不到就整段跳过。
 */
static const char* kPipelineTasksPath = "/proc/oplus_scheduler/sched_assist/pipeline_pids_cpus";
static std::unordered_map<pid_t, int> g_pipeline_tasks;   /* tid → 核号 */
static bool g_pipeline_node_missing = false;
static int g_pipeline_channel_state = -1;   /* -1=未探测 0=没有这个节点 1=可用 */

/*
 * 关键线程 tid 的只读快照，给动态调频那边的"掉帧现场"观测用。
 * 直接跨线程迭代 g_pipeline_tasks（unordered_map）会有并发读写风险，
 * 这里只在绑核线程里刷新一个定长数组；读方读到旧值最多是少采一次样。
 */
static const int kPipelineTidSnapshotMax = 8;
static pid_t g_pipeline_tid_snapshot[kPipelineTidSnapshotMax] = {};
static int g_pipeline_tid_snapshot_count = 0;

static void refresh_pipeline_tid_snapshot_locked() {
    int n = 0;
    for (const auto& kv : g_pipeline_tasks) {
        if (n >= kPipelineTidSnapshotMax) break;
        g_pipeline_tid_snapshot[n++] = kv.first;
    }
    for (int i = n; i < kPipelineTidSnapshotMax; i++) {
        g_pipeline_tid_snapshot[i] = 0;
    }
    g_pipeline_tid_snapshot_count = n;
}

/* 官方 pipeline 通道是否存在（探测一次并缓存）。 */
static bool pipeline_channel_available() {
    if (g_pipeline_channel_state < 0) {
        g_pipeline_channel_state = (access(kPipelineTasksPath, W_OK) == 0) ? 1 : 0;
        if (g_pipeline_channel_state == 0) {
            log_config_message("没有官方 pipeline 节点（非 ColorOS 机型），只按配置里的 cpus 绑核");
        }
    }
    return g_pipeline_channel_state == 1;
}

static void flush_pipeline_tasks_locked() {
    if (g_pipeline_node_missing) return;
    /*
     * 自愈：tid 可能已经退出，或者被内核回收给了新线程（TID 复用）——
     * 这两种情况下旧登记都是脏的（实测见过"mgpa-report 却登记成 7 号核"）。
     * 只保留"线程还活着、且登记的核仍在它的允许掩码里"的条目。
     *
     * 注意这里**不能**要求掩码只有 1 个核：官方云控给 UnityMain 的绑定是 g_11 = 6-7、
     * 给 UnityGfxDeviceW 是 g_10 = 0-5，登记的核心（7 / 5）只是掩码里的一个点。
     * 早先按"必须是单核"判定，导致改成多核掩码后登记被自己立刻擦掉、
     * /proc/oplus_scheduler/sched_assist/pipeline_pids_cpus 退回全 -1。
     */
    for (auto it = g_pipeline_tasks.begin(); it != g_pipeline_tasks.end();) {
        cpu_set_t current;
        CPU_ZERO(&current);
        bool valid = kill(it->first, 0) == 0 &&
                     sched_getaffinity(it->first, sizeof(current), &current) == 0 &&
                     CPU_ISSET(it->second, &current);
        if (!valid) {
            it = g_pipeline_tasks.erase(it);
        } else {
            ++it;
        }
    }
    char buf[256];
    buf[0] = '\0';
    size_t pos = 0;
    int written = 0;
    for (const auto& kv : g_pipeline_tasks) {
        if (written >= 6) break;   /* 内核最多 6 对 */
        int n = safe_snprintf(buf + pos, sizeof(buf) - pos, "%s%d %d",
                              written == 0 ? "" : " ", (int)kv.first, kv.second);
        if (n <= 0 || (size_t)n >= sizeof(buf) - pos) break;
        pos += (size_t)n;
        written++;
    }
    if (written == 0) {
        safe_strncpy(buf, "-1", sizeof(buf));
    }
    FILE* fp = fopen(kPipelineTasksPath, "w");
    if (!fp) {
        g_pipeline_node_missing = true;   /* 不是所有机型都有这个节点；缺了就静默退回家用规则 */
        g_pipeline_channel_state = 0;
        return;
    }
    fputs(buf, fp);
    fputc('\n', fp);
    fclose(fp);
}

/* cpu < 0 = 取消该线程的 pipeline 登记。 */
static void set_pipeline_task_locked(pid_t tid, int cpu) {
    if (tid <= 0 || g_pipeline_node_missing || !pipeline_channel_available()) return;
    if (cpu < 0) {
        if (g_pipeline_tasks.erase(tid) > 0) {
            flush_pipeline_tasks_locked();
            refresh_pipeline_tid_snapshot_locked();
        }
        return;
    }
    auto it = g_pipeline_tasks.find(tid);
    if (it != g_pipeline_tasks.end() && it->second == cpu) return;
    g_pipeline_tasks[tid] = cpu;
    flush_pipeline_tasks_locked();
    refresh_pipeline_tid_snapshot_locked();
}

static void clear_all_pipeline_tasks_locked() {
    if (g_pipeline_tasks.empty()) return;
    g_pipeline_tasks.clear();
    flush_pipeline_tasks_locked();
    refresh_pipeline_tid_snapshot_locked();
}

/*
 * 执行端自动补的官方 pipeline 标记（ColorOS 专有）。
 *
 * 绑核掩码按 cpus 字面执行，登记进内核的核单独决定：
 *   - 默认（配置只写 cpus）：取掩码里**最高的那个核**
 *     （官方就是把 g_11=6-7 的 UnityMain 登记成 7、把 0-5 组里的关键线程登记成 3/4/5）；
 *   - 配置额外写了 cpu：登记就用它，实现“绑核掩码”与“pipeline 登记”解耦；
 *     cpu 必须落在 cpus 掩码内，否则视为配置错误，回退到最高核并打日志；
 *   - 登记只是打标记/提示（登记核 == 最大核 → prime task + pipeline boost），
 *     注意登记非最大核时内核不会 prime/boost；
 *   - 没有这个节点的机型（非 ColorOS）整段跳过，绑核照旧。
 *
 * pipeline_cpu < 0 表示未配置；否则为配置值，函数内部会校验是否在掩码内。
 */
static void mark_pipeline_task_locked(pid_t tid,
                                      const std::vector<int>& cpus,
                                      int pipeline_cpu,
                                      const ThreadJsonResolvedRule* rule) {
    if (tid <= 0 || cpus.empty()) {
        set_pipeline_task_locked(tid, -1);
        return;
    }
    int top_cpu = cpus[0];
    for (size_t i = 1; i < cpus.size(); i++) {
        if (cpus[i] > top_cpu) top_cpu = cpus[i];
    }
    int target_cpu = top_cpu;
    if (pipeline_cpu >= 0) {
        bool in_mask = false;
        for (size_t i = 0; i < cpus.size(); i++) {
            if (cpus[i] == pipeline_cpu) {
                in_mask = true;
                break;
            }
        }
        if (in_mask) {
            target_cpu = pipeline_cpu;
        } else {
            /* 配置的 cpu 不在 cpus 掩码里，登记了也跑不到 —— 回退并明确报错，避免静默误解。 */
            log_config_message_throttled("pipeline_cpu_out_of_mask", 5000,
                "配置的 cpu 不在 cpus 掩码内，已回退为掩码最高核");
        }
    }
    set_pipeline_task_locked(tid, target_cpu);
}

/*
 * 只有"命中具体规则"的线程才登记（role / custom 规则）。
 * 兜底规则（default / 场景默认，例如 other 的整包兜底）作用于成百条线程，
 * 挨个登记会把节点塞满无意义条目——官方也只登记 bind_list 里点名的那几条关键线程。
 */
/*
 * 只有"命中具体规则"的线程才登记（role / custom 规则）。
 * 兜底规则（default / 场景默认，例如 other 的整包兜底）作用于成百条线程，
 * 挨个登记会把节点塞满无意义条目——官方也只登记 bind_list 里点名的那几条关键线程。
 */
static void mark_pipeline_task_for_rule_locked(pid_t tid,
                                               const ThreadJsonResolvedRule* rule,
                                               const std::vector<int>& cpus) {
    if (!rule ||
        rule->source_kind == THREAD_JSON_RULE_SOURCE_DEFAULT ||
        rule->source_kind == THREAD_JSON_RULE_SOURCE_SCENE_DEFAULT) {
        return;
    }
    /* cpu 未配置时传 -1，保持“取掩码最高核”的原有行为。 */
    int pipeline_cpu = -1;
    if (rule->action && rule->action->has_pipeline_cpu) {
        pipeline_cpu = rule->action->pipeline_cpu;
    }
    mark_pipeline_task_locked(tid, cpus, pipeline_cpu, rule);
}

static void restore_thread_runtime_adjustments_locked(ThreadRuntimeState& state, const char* reason) {
    set_pipeline_task_locked(state.tid, -1);
    if (state.sched_baseline_captured) {
        if (state.applied_sched_policy != state.baseline_sched_policy ||
            state.applied_sched_priority != state.baseline_sched_priority) {
            struct sched_param param = {};
            param.sched_priority = state.baseline_sched_priority;
            if (sched_setscheduler(state.tid, state.baseline_sched_policy, &param) == 0) {
                state.applied_sched_policy = state.baseline_sched_policy;
                state.applied_sched_priority = state.baseline_sched_priority;
            } else {
                char msg[256];
                safe_snprintf(msg, sizeof(msg),
                              "恢复线程调度失败 | tid:%d | comm:%s | reason:%s | err:%s",
                              (int)state.tid,
                              state.comm.c_str(),
                              reason ? reason : "unknown",
                              strerror(errno));
                log_affinity_message(msg);
            }
        }
        if (state.applied_nice != state.baseline_nice) {
            if (setpriority(PRIO_PROCESS, state.tid, state.baseline_nice) == 0) {
                state.applied_nice = state.baseline_nice;
            } else {
                char msg[256];
                safe_snprintf(msg, sizeof(msg),
                              "恢复线程优先级失败 | tid:%d | comm:%s | reason:%s | err:%s",
                              (int)state.tid,
                              state.comm.c_str(),
                              reason ? reason : "unknown",
                              strerror(errno));
                log_affinity_message(msg);
            }
        }
    }

    /*
     * 掩码恢复独立于调度策略：即使这个线程只被绑过核、没有改过 nice/策略，
     * 也必须撤回原始掩码（通常就是全核 0-7）。
     */
    restore_thread_affinity_locked(state.tid, reason);
    restore_thread_cpuset_membership_locked(state.tid, reason);
}

static void reset_thread_runtime_tracking_state_locked(ThreadRuntimeState& state) {
    state.assigned_cluster = -1;
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
    state.has_explicit_affinity_rule = false;
    state.last_static_bind_ms = 0;
    state.static_bind_cooldown_ms = 0;
    state.selection_epoch = 0;
    state.guard_reapply_streak = 0;
    state.guard_yielded = false;
    state.affinity_retry_streak = 0;
    state.affinity_yield_until_ms = 0;
}

static void release_thread_runtime_locked(
    ThreadRuntimeState& state,
    const char* reason,
    bool clear_tracking_state,
    bool clear_latched_primary_tid) {
    if (clear_latched_primary_tid) {
        clear_latched_primary_tid_locked(state.tid, reason);
    }
    set_pipeline_task_locked(state.tid, -1);
    restore_thread_runtime_adjustments_locked(state, reason);
    if (clear_tracking_state) {
        reset_thread_runtime_tracking_state_locked(state);
    }
}

static void restore_all_thread_runtime_adjustments_locked(const char* reason) {
    for (auto& kv : g_threads) {
        restore_thread_runtime_adjustments_locked(kv.second, reason);
    }
    /* 全量恢复：pipeline 登记表也一起清空，别把内核节点留在半登记状态 */
    clear_all_pipeline_tasks_locked();
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
        release_thread_runtime_locked(state, "rule_missing", true, false);
        return;
    }

    const ThreadJsonAction& action = *resolved_rule->action;
    capture_thread_sched_baseline_locked(state);

    int target_policy = state.baseline_sched_policy;
    int target_priority = state.baseline_sched_priority;
    if (action.has_scheduler) {
        target_policy = thread_json_sched_policy_to_native(action.sched_policy);
        target_priority = action.sched_priority;
    }

    bool rt_target = (target_policy == SCHED_RR || target_policy == SCHED_FIFO);
    if (!rt_target && action.has_nice && action.nice != state.applied_nice) {
        if (setpriority(PRIO_PROCESS, state.tid, action.nice) == 0) {
            state.applied_nice = action.nice;
        }
    } else if ((rt_target || !action.has_nice) && state.applied_nice != state.baseline_nice) {
        if (setpriority(PRIO_PROCESS, state.tid, state.baseline_nice) == 0) {
            state.applied_nice = state.baseline_nice;
        }
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

static bool resolve_affinity_template(const char* package, pid_t tid, const char* comm, ThreadClass cls, float cpu_usage_pct, const AllCpuStats& stats, std::vector<int>& cpus, int& primary_cluster) {
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
    g_render_target_tids_installed.clear();
}

static void clear_dynamic_thread_tracking_locked(bool clear_thread_records) {
    g_dynamic_affinity_recheck_pending = false;
    clear_thread_runtime_map_locked();
    clear_target_tid_map_locked();
    if (clear_thread_records) {
        clear_thread_rule_match_caches_locked();
        g_threads.clear();
        clear_all_selection_epochs_locked();
    }
}

static void release_dynamic_thread_runtime_locked(const char* reason, bool clear_thread_records) {
    restore_all_thread_runtime_adjustments_locked(reason);
    clear_dynamic_thread_tracking_locked(clear_thread_records);
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

    clear_invalid_latched_primary_tids_locked();
    std::unordered_set<int> desired_tids;
    bool latched_added = false;
    auto collect_target_tid = [&](pid_t tid) -> bool {
        if (tid <= 0) return true;
        auto it = g_threads.find((int)tid);
        if (it == g_threads.end()) return false;
        const ThreadRuntimeState& state = it->second;
        if (state.pid != g_foreground_pid) return false;
        if (state.package.empty() || strcmp(state.package.c_str(), g_foreground_package) != 0) return false;
        desired_tids.insert((int)state.tid);
        return true;
    };
    if (g_scheduler_session.phase == SESSION_LOCKED &&
        is_scheduler_session_foreground_locked()) {
        if (collect_target_tid(g_scheduler_session.latched_main_tid) &&
            g_scheduler_session.latched_main_tid > 0) {
            latched_added = true;
        }
        if (collect_target_tid(g_scheduler_session.latched_render_tid) &&
            g_scheduler_session.latched_render_tid > 0) {
            latched_added = true;
        }
    } else {
        if (is_scheduler_session_foreground_locked()) {
            if (collect_target_tid(g_scheduler_session.latched_main_tid) &&
                g_scheduler_session.latched_main_tid > 0) {
                latched_added = true;
            }
            if (collect_target_tid(g_scheduler_session.latched_render_tid) &&
                g_scheduler_session.latched_render_tid > 0) {
                latched_added = true;
            }
        }
        if (!latched_added) {
            for (auto& kv : g_threads) {
                ThreadRuntimeState& state = kv.second;
                bool render_like = should_treat_as_priority_game_thread(
                    g_foreground_package, state.comm.c_str(), state.tid, state.cpu_usage_pct);
                if (!render_like) continue;
                if (!is_hot_priority_thread_locked(state.tid, state.comm.c_str(), state.cpu_usage_pct)) continue;
                desired_tids.insert((int)state.tid);
            }
        }
    }

    if (force) {
        clear_target_tid_map_locked();
    }

    for (auto it = g_render_target_tids_installed.begin(); it != g_render_target_tids_installed.end(); ) {
        if (desired_tids.find(*it) != desired_tids.end()) {
            ++it;
            continue;
        }
        uint32_t tid = (uint32_t)(*it);
        bpf_map_delete_elem(g_target_tid_map_fd, &tid);
        it = g_render_target_tids_installed.erase(it);
    }

    for (int tid_int : desired_tids) {
        if (g_render_target_tids_installed.find(tid_int) != g_render_target_tids_installed.end()) {
            continue;
        }
        uint32_t tid = (uint32_t)tid_int;
        uint32_t enabled = 1;
        if (bpf_map_update_elem(g_target_tid_map_fd, &tid, &enabled, BPF_ANY) != 0) {
            log_bpf_error_once("eBPF写入渲染线程TID失败", errno);
            continue;
        }
        g_render_target_tids_installed.insert(tid_int);
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

static void collect_fas_thread_features_locked(const char* package,
                                               FasThreadFeatureSnapshot* out) {
    if (!out) return;

    memset(out, 0, sizeof(*out));
    float worker_top_values[3] = {0.0f, 0.0f, 0.0f};

    for (auto& kv : g_threads) {
        const ThreadRuntimeState& state = kv.second;
        if (package && package[0]) {
            if (state.package.empty() || state.package != package) {
                continue;
            }
        }

        float usage = state.cpu_usage_pct;
        const char* comm = state.comm.c_str();
        if (is_main_thread_name_for_package(package, comm, state.tid)) {
            if (usage > out->game_thread_usage) out->game_thread_usage = usage;
        }
        if (is_render_thread_name(comm)) {
            if (usage > out->render_thread_usage) out->render_thread_usage = usage;
        }
        if (is_rhi_thread_name(comm)) {
            if (usage > out->rhi_thread_usage) out->rhi_thread_usage = usage;
        }
        if (is_taskgraph_thread_name(comm)) {
            if (usage > out->taskgraph_thread_usage) out->taskgraph_thread_usage = usage;
        }

        if (is_worker_thread_name(comm)) {
            if (usage > out->worker_top_usage) out->worker_top_usage = usage;
            if (usage >= 3.0f) out->worker_hot_count++;
            for (int i = 0; i < 3; i++) {
                if (usage > worker_top_values[i]) {
                    for (int j = 2; j > i; j--) {
                        worker_top_values[j] = worker_top_values[j - 1];
                    }
                    worker_top_values[i] = usage;
                    break;
                }
            }
        }
    }

    out->busy_thread_usage = out->game_thread_usage;
    if (out->render_thread_usage > out->busy_thread_usage) out->busy_thread_usage = out->render_thread_usage;
    if (out->rhi_thread_usage > out->busy_thread_usage) out->busy_thread_usage = out->rhi_thread_usage;

    out->worker_top_n_usage = worker_top_values[0] + worker_top_values[1] + worker_top_values[2];
    out->runnable_pressure =
        out->worker_top_n_usage +
        out->taskgraph_thread_usage * 0.7f +
        out->render_thread_usage * 0.35f +
        out->rhi_thread_usage * 0.25f;
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
    std::shared_lock<std::shared_mutex> lock(g_mutex);
    return get_busy_thread_usage_locked();
}

void collect_fas_thread_features(const char* package, FasThreadFeatureSnapshot* out) {
    std::shared_lock<std::shared_mutex> lock(g_mutex);
    collect_fas_thread_features_locked(package, out);
}

bool has_active_download_thread() {
    std::shared_lock<std::shared_mutex> lock(g_mutex);
    return has_active_download_thread_locked();
}

pid_t get_foreground_pid_snapshot() {
    std::shared_lock<std::shared_mutex> lock(g_mutex);
    return g_foreground_pid;
}

static void detach_queuebuffer_uprobe_locked() {
    for (int i = 0; i < kQueuebufferSymbolCount; i++) {
        if (*queuebuffer_links[i]) {
            bpf_link__destroy(*queuebuffer_links[i]);
            *queuebuffer_links[i] = nullptr;
        }
    }
    g_queuebuffer_attached_pid = -1;
    g_queuebuffer_attached_package[0] = '\0';
    reset_queuebuffer_frame_cache(-1, "");
}

static bool attach_queuebuffer_uprobe_locked(pid_t pid, const char* package) {
    if (!g_bpf_obj || !g_bpf_queuebuffer_prog || pid <= 0 || !package || !package[0]) {
        detach_queuebuffer_uprobe_locked();
        return false;
    }

    if (count_queuebuffer_links_locked() > 0 &&
        g_queuebuffer_attached_pid == pid &&
        strcmp(g_queuebuffer_attached_package, package) == 0) {
        return true;
    }

    detach_queuebuffer_uprobe_locked();

    bpf_uprobe_opts opts = {};
    opts.sz = sizeof(opts);
    for (int i = 0; i < kQueuebufferSymbolCount; i++) {
        opts.func_name = queuebuffer_symbol_names[i];
        struct bpf_link* link = bpf_program__attach_uprobe_opts(
            g_bpf_queuebuffer_prog, (int)pid, LIBGUI_SO_PATH, 0, &opts);
        if (libbpf_get_error(link)) {
            link = nullptr;
        }
        *queuebuffer_links[i] = link;
    }

    int attached = count_queuebuffer_links_locked();
    if (attached == 0) {
        char msg[256];
        safe_snprintf(msg, sizeof(msg), "queueBuffer uprobe挂载失败 | app:%s | pid:%d", package, (int)pid);
        log_fas_message(msg);
        return false;
    }

    g_queuebuffer_attached_pid = pid;
    safe_strncpy(g_queuebuffer_attached_package, package, sizeof(g_queuebuffer_attached_package));
    reset_queuebuffer_frame_cache(pid, package);

    char msg[256];
    safe_snprintf(msg, sizeof(msg),
                  "queueBuffer uprobe已挂载 | app:%s | pid:%d | links:%d/%d",
                  package, (int)pid, attached, kQueuebufferSymbolCount);
    log_fas_message(msg);
    return true;
}

static void refresh_queuebuffer_attachment_locked() {
    if (!g_bpf_ready.load() || !g_bpf_queuebuffer_prog) return;
    if (!is_fas_enabled_for_app(g_foreground_package) || g_foreground_pid <= 0) {
        detach_queuebuffer_uprobe_locked();
        return;
    }
    attach_queuebuffer_uprobe_locked(g_foreground_pid, g_foreground_package);
}

static void poll_queuebuffer_ring_locked() {
    if (!g_frame_signal_ring) return;
    int err = ring_buffer__poll(g_frame_signal_ring, 0);
    if (err < 0 && err != -EINTR) {
        log_bpf_error_once("queueBuffer ring_buffer__poll", -err);
    }
}

/*
 * 把"从父线程继承来的窄掩码"解开。
 *
 * 这是 affinity 方案最要命的一处：任何一个角色线程被绑核之后，它 fork 出来的
 * 子线程都会继承那把窄掩码，于是若干个 worker 挤在同一两个核上。这里的做法是
 * 在 fork 事件里立刻把子线程的掩码还原成"全部在线核"，而真正的资源约束继续由
 * cpuset 承担（掩码 ∩ cpuset 里，cpuset 说了算，所以放宽掩码不会越权）。
 *
 * 账本按"全核"记录：将来撤回时不能回到父线程那把窄掩码。
 */
static bool clear_inherited_thread_mask_locked(pid_t tid) {
    if (tid <= 0 || cluster_count <= 0) return false;

    cpu_set_t full;
    if (!resolve_thread_allowed_mask_locked(tid, &full)) return false;
    int full_count = 0;
    for (int cpu = 0; cpu < CPU_SETSIZE; cpu++) {
        if (CPU_ISSET(cpu, &full)) full_count++;
    }
    if (full_count <= 0) return false;

    cpu_set_t current;
    CPU_ZERO(&current);
    if (sched_getaffinity(tid, sizeof(current), &current) != 0) {
        return false;
    }

    bool narrower = false;
    for (int cpu = 0; cpu < CPU_SETSIZE; cpu++) {
        if (CPU_ISSET(cpu, &full) && !CPU_ISSET(cpu, &current)) {
            narrower = true;
            break;
        }
    }
    if (!narrower) {
        /* 没被继承的窄掩码：什么都不用做。 */
        return false;
    }

    if (sched_setaffinity(tid, sizeof(full), &full) != 0) {
        return false;
    }
    g_affinity_baseline_masks[tid] = full;
    g_affinity_dirty_tids.insert(tid);
    return true;
}

/*
 * fork/exec 时刻的精确绑定：新线程一诞生就按它自己的角色规则决定落位，
 * 而不是等下一轮全量枚举之后再纠正（那时它可能已经在错误的核上跑了很久）。
 *
 * 只有命中角色规则、且在规则 limit 之内的线程才会拿到精确掩码；
 * 其余线程只做一件事——解开继承来的掩码，交回内核与 cpuset 处理。
 */
static void apply_rule_based_binding_for_new_thread_locked(
    pid_t pid,
    const char* package,
    pid_t tid,
    const char* comm_hint,
    long long now_ms) {
    if (pid <= 0 || tid <= 0 || !package || !package[0]) return;
    if (!is_custom_thread_enabled_for_app(package)) return;
    if (should_pause_our_game_scheduler()) return;

    auto it = g_threads.find(tid);
    if (it == g_threads.end()) return;
    ThreadRuntimeState& state = it->second;

    /* 先把基线定成 cpuset 允许集：新线程此刻的掩码是继承来的，不能当基线。 */
    seed_newborn_affinity_baseline_locked(tid);

    const char* comm = comm_hint && comm_hint[0] ? comm_hint : state.comm.c_str();
    ThreadJsonResolvedRule resolved_rule = {};
    const ThreadJsonResolvedRule* rule = resolve_thread_json_rule_locked(
        package, tid, comm, state.cpu_usage_pct, state.cls, &resolved_rule);

    if (rule && rule->action) {
        apply_thread_json_action_locked(state, rule);
    }

    /*
     * 自定义线程一律按配置里的 cpus 字面绑核；绑到单核时，
     * 如果这台机器的内核有官方 pipeline 节点（ColorOS），顺手补一个标记
     * （最大核 → prime task + pipeline boost）。JSON 里不写 pipeline，
     * 接口能不能用由执行端判断。撤规则时按 tid 注销登记。
     */
    std::vector<int> explicit_cpus;
    if (rule && rule->action &&
        (rule->action->has_clusters || rule->action->has_cpus)) {
        resolve_thread_json_action_cpus_locked(*rule->action, explicit_cpus);
        normalize_cpu_mask(explicit_cpus);
    }

    if (!explicit_cpus.empty()) {
        if (set_thread_affinity_mask(tid, explicit_cpus)) {
            mark_pipeline_task_for_rule_locked(tid, rule, explicit_cpus);
            state.assigned_cluster = highest_cluster_for_mask(explicit_cpus);
            state.assigned_cpus = explicit_cpus;
            state.last_migrate_ms = now_ms;
            state.has_explicit_affinity_rule =
                rule->source_kind != THREAD_JSON_RULE_SOURCE_DEFAULT &&
                rule->source_kind != THREAD_JSON_RULE_SOURCE_SCENE_DEFAULT;
            g_runtime_stats.thread_event_bind_count.fetch_add(1);
        }
        return;
    }

    if (clear_inherited_thread_mask_locked(tid)) {
        g_runtime_stats.thread_event_unbind_count.fetch_add(1);
    }
}

static int handle_process_event_signal(void* ctx, void* data, size_t size) {
    (void)ctx;
    if (!data || size < sizeof(ProcessEventSignal)) return 0;
    const ProcessEventSignal* signal = reinterpret_cast<const ProcessEventSignal*>(data);
    if (signal->pid <= 0 || signal->tid <= 0) return 0;

    char event_comm[sizeof(signal->comm) + 1] = {0};
    memcpy(event_comm, signal->comm, sizeof(signal->comm));
    event_comm[sizeof(signal->comm)] = '\0';

    g_process_event_seen = true;
    g_last_process_event_ms = get_current_time_ms();

    const bool is_process_leader_event = (signal->tid == signal->pid);

    /*
     * 退出事件先处理。线程退出在系统范围里是高频事件，而"死掉的线程"不需要
     * 再做包名解析和规则匹配，所以这条路必须是零 syscall 的：查表删册就够了。
     */
    if (signal->type == PROCESS_EVENT_EXIT) {
        if (is_process_leader_event) {
            remove_process_from_package_cache_locked(signal->pid);
        }
        auto exit_it = g_threads.find(signal->tid);
        if (exit_it != g_threads.end()) {
            release_thread_runtime_locked(exit_it->second, "thread_exit_event", true, true);
            forget_thread_affinity_locked(exit_it->first);
            forget_thread_cpuset_locked(exit_it->first);
            g_threads.erase(exit_it);
            g_runtime_stats.thread_event_exit_count.fetch_add(1);
        }
        return 0;
    }

    /*
     * 前台进程的事件占绝大多数。此时包名已知，直接复用，
     * 不必为每个 fork/exit 再读一次 /proc —— 事件路径必须是零额外 syscall 的。
     */
    const bool is_foreground_pid =
        g_foreground_pid > 0 &&
        signal->pid == g_foreground_pid &&
        g_foreground_package[0] &&
        strcmp(g_foreground_package, "unknown") != 0;

    char package[MAX_PACKAGE_LEN] = {0};
    if (is_foreground_pid) {
        safe_strncpy(package, g_foreground_package, sizeof(package));
    } else if (!read_process_package_name(signal->pid, package, sizeof(package))) {
        return 0;
    }

    /*
     * 增量维护 pid→package 表：新进程出现时只读它自己的 cmdline 一次，
     * 让"前台 pid 解析"不必再周期性全量重建（那是最大的一笔 /proc 开销）。
     */
    if (signal->type == PROCESS_EVENT_EXEC ||
        (signal->type == PROCESS_EVENT_FORK && is_process_leader_event)) {
        char warm_package[MAX_PACKAGE_LEN] = {0};
        read_process_package_name(signal->pid, warm_package, sizeof(warm_package));
    }

    if (is_foreground_pid &&
        (signal->type == PROCESS_EVENT_FORK || signal->type == PROCESS_EVENT_EXEC)) {
        /*
         * 事件驱动发现：新线程立即入册，不再等下一轮 /proc 全量枚举。
         * 分类/绑核仍由主调度轮按角色规则处理，这里只负责"看得见"。
         */
        ensure_thread_state_for_tid_locked(signal->pid, package, signal->tid, event_comm);
        ThreadRuntimeState& new_state = g_threads[signal->tid];
        if (new_state.comm.empty()) {
            new_state.comm = event_comm;
        }
        new_state.pid = signal->pid;
        new_state.package = package;
        g_runtime_stats.thread_event_fork_count.fetch_add(1);
        g_foreground_pid_refresh_pending = true;

        /* 当场决定它该落在哪些核，并把父线程继承下来的掩码处理掉。 */
        apply_rule_based_binding_for_new_thread_locked(
            signal->pid, package, signal->tid, event_comm, g_last_process_event_ms);
    }

    if (g_foreground_package[0] &&
        strcmp(g_foreground_package, "unknown") != 0 &&
        strcmp(package, g_foreground_package) == 0 &&
        (signal->type == PROCESS_EVENT_EXEC || signal->type == PROCESS_EVENT_FORK)) {
        g_foreground_pid_refresh_pending = true;
    }
    if (g_foreground_package[0] &&
        strcmp(g_foreground_package, "unknown") != 0 &&
        strcmp(package, g_foreground_package) == 0 &&
        signal->type == PROCESS_EVENT_SET_AFFINITY) {
        g_dynamic_affinity_recheck_pending = true;
    }

    const ThreadJsonAppRule* rule = find_persistent_thread_app_rule_for_package_locked(package);
    if (!rule) {
        return 0;
    }

    long long now_ms = get_current_time_ms();
    std::unordered_set<int> seen;
    /*
     * persistent 规则需要整进程重扫。fork 风暴时不能每个事件都扫一遍，
     * 250ms 节流；常规前台进程根本不走这里（上面已按事件精确入册）。
     */
    if (now_ms - g_last_persistent_event_scan_ms >= 250) {
        g_last_persistent_event_scan_ms = now_ms;
        collect_process_threads_for_package_locked(signal->pid, package, true, now_ms, seen);
        apply_explicit_persistent_actions_for_process_locked(signal->pid, package, now_ms);
    }

    if (!seen.empty()) {
        char comm_buf[sizeof(signal->comm) + 1] = {0};
        memcpy(comm_buf, signal->comm, sizeof(signal->comm));
        comm_buf[sizeof(signal->comm)] = '\0';
        
        const char* event_names[] = { "unknown", "fork", "exec", "setaffinity", "exit" };
        const char* event_name =
            (signal->type >= 1 && signal->type <= 4) ? event_names[signal->type] : "unknown";

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
        log_config_message(msg);
        bpf_object__close(g_bpf_obj);
        g_bpf_obj = nullptr;
        g_bpf_failed.store(1);
        return false;
    }

    g_bpf_switch_prog = find_bpf_program_compat("handle_sched_switch", "raw_tracepoint/sched_switch");
    g_bpf_wakeup_prog = find_bpf_program_compat("handle_sched_wakeup", "raw_tracepoint/sched_wakeup");
    g_bpf_queuebuffer_prog = find_bpf_program_compat("handle_queuebuffer", "uprobe");
    g_bpf_process_fork_prog = find_bpf_program_compat("handle_process_fork", "raw_tracepoint/sched_process_fork");
    g_bpf_process_exec_prog = find_bpf_program_compat("handle_process_exec", "raw_tracepoint/sched_process_exec");
    g_bpf_process_affinity_prog = find_bpf_program_compat("handle_sched_setaffinity", "raw_tracepoint/sys_enter");
    /* 可选：新版 BPF 提供退出事件；旧对象没有这个程序时继续跑，只是回退到低频扫描。 */
    g_bpf_process_exit_prog =
        find_bpf_program_compat("handle_process_exit", "raw_tracepoint/sched_process_exit");
    if (!g_bpf_switch_prog || !g_bpf_wakeup_prog || !g_bpf_queuebuffer_prog ||
        !g_bpf_process_fork_prog || !g_bpf_process_exec_prog || !g_bpf_process_affinity_prog) {
        char msg[320];
        safe_snprintf(msg, sizeof(msg),
                      "eBPF初始化失败: 程序缺失 | switch=%d wakeup=%d queuebuffer=%d fork=%d exec=%d affinity=%d",
                      g_bpf_switch_prog ? 1 : 0,
                      g_bpf_wakeup_prog ? 1 : 0,
                      g_bpf_queuebuffer_prog ? 1 : 0,
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
        log_config_message("eBPF初始化失败: attach sched_switch");
        g_bpf_switch_link = nullptr;
        bpf_object__close(g_bpf_obj);
        g_bpf_obj = nullptr;
        g_bpf_failed.store(1);
        return false;
    }

    g_bpf_wakeup_link = bpf_program__attach(g_bpf_wakeup_prog);
    if (libbpf_get_error(g_bpf_wakeup_link)) {
        log_config_message("eBPF初始化失败: attach sched_wakeup");
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
        log_config_message("eBPF初始化失败: attach sched_process_fork");
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
        log_config_message("eBPF初始化失败: attach sched_process_exec");
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
        log_config_message("eBPF初始化失败: attach sys_enter_sched_setaffinity");
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

    /*
     * 退出事件是可选程序：挂上它，线程集合的增删就都由事件驱动；
     * 挂不上（旧 BPF 对象 / 内核不支持该 raw_tp）只降级为低频扫描，不阻塞启动。
     */
    if (g_bpf_process_exit_prog) {
        g_bpf_process_exit_link = bpf_program__attach(g_bpf_process_exit_prog);
        if (libbpf_get_error(g_bpf_process_exit_link)) {
            g_bpf_process_exit_link = nullptr;
            log_config_message("eBPF: sched_process_exit 事件不可用，线程清册回退为低频扫描");
        } else {
            log_config_message("eBPF: 线程退出事件已挂载（事件驱动清册）");
        }
    }
    struct bpf_map* runtime_map = bpf_object__find_map_by_name(g_bpf_obj, "thread_runtime_map");
    struct bpf_map* cpu_last_map = bpf_object__find_map_by_name(g_bpf_obj, "cpu_last_ts_map");
    struct bpf_map* urgent_map = bpf_object__find_map_by_name(g_bpf_obj, "urgent_flag_map");
    struct bpf_map* target_tid_map = bpf_object__find_map_by_name(g_bpf_obj, "target_tid_map");
    struct bpf_map* frame_signal_map = bpf_object__find_map_by_name(g_bpf_obj, "frame_signal_rb");
    struct bpf_map* process_event_map = bpf_object__find_map_by_name(g_bpf_obj, "process_event_rb");
    if (!target_map || !runtime_map || !cpu_last_map || !urgent_map || !target_tid_map ||
        !frame_signal_map || !process_event_map) {
        log_config_message("eBPF初始化失败: 找不到聚合 map");
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
    g_frame_signal_rb_map_fd = bpf_map__fd(frame_signal_map);
    g_process_event_rb_map_fd = bpf_map__fd(process_event_map);
    ring_buffer_opts rb_opts = {};
    rb_opts.sz = sizeof(rb_opts);
    g_frame_signal_ring = ring_buffer__new(
        g_frame_signal_rb_map_fd, handle_queuebuffer_frame_signal, NULL, &rb_opts);
    if (!g_frame_signal_ring) {
        log_config_message("eBPF初始化失败: ring_buffer__new");
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
    g_process_event_ring = ring_buffer__new(
        g_process_event_rb_map_fd, handle_process_event_signal, NULL, &rb_opts);
    if (!g_process_event_ring) {
        log_config_message("eBPF初始化失败: process_event ring_buffer__new");
        ring_buffer__free(g_frame_signal_ring);
        g_frame_signal_ring = nullptr;
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
    g_queuebuffer_attached_pid = -1;
    g_queuebuffer_attached_package[0] = '\0';
    memset(g_boost_until_ms, 0, sizeof(g_boost_until_ms));
    g_bpf_ready.store(1);
    log_message("eBPF内核聚合 + queueBuffer帧采样 + persistent事件刷新已启用");
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
    detach_queuebuffer_uprobe_locked();
    if (g_process_event_ring) {
        ring_buffer__free(g_process_event_ring);
        g_process_event_ring = nullptr;
    }
    if (g_frame_signal_ring) {
        ring_buffer__free(g_frame_signal_ring);
        g_frame_signal_ring = nullptr;
    }
    if (g_bpf_process_affinity_link) {
        bpf_link__destroy(g_bpf_process_affinity_link);
        g_bpf_process_affinity_link = nullptr;
    }
    if (g_bpf_process_exit_link) {
        bpf_link__destroy(g_bpf_process_exit_link);
        g_bpf_process_exit_link = nullptr;
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
    g_bpf_queuebuffer_prog = nullptr;
    g_bpf_process_fork_prog = nullptr;
    g_bpf_process_exec_prog = nullptr;
    g_bpf_process_affinity_prog = nullptr;
    g_target_tgid_map_fd = -1;
    g_thread_runtime_map_fd = -1;
    g_cpu_last_ts_map_fd = -1;
    g_urgent_flag_map_fd = -1;
    g_target_tid_map_fd = -1;
    g_frame_signal_rb_map_fd = -1;
    g_process_event_rb_map_fd = -1;
    g_installed_target_tgid = -1;
    g_bpf_ready.store(0);
}

static pid_t find_foreground_pid_by_package(const char* package) {
    if (!package || package[0] == '\0' || strcmp(package, "unknown") == 0) {
        return -1;
    }
    refresh_process_package_cache_locked(false);

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
    for (auto& kv : g_process_package_cache) {
        const ProcessPackageCacheEntry& item = kv.second;
        pid_t pid = item.pid;
        bool exact_match = (item.raw_cmdline == package);
        bool fallback_match = false;
        if (!exact_match &&
            item.base_package == package &&
            item.raw_cmdline.size() > strlen(package)) {
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
    return best_exact_pid > 0 ? best_exact_pid : best_fallback_pid;
}

static void find_process_pids_by_package(const char* package, std::vector<pid_t>& out_pids) {
    out_pids.clear();
    if (!package || package[0] == '\0' || strcmp(package, "unknown") == 0) {
        return;
    }
    refresh_process_package_cache_locked(false);
    auto it = g_process_pids_by_package_cache.find(package);
    if (it == g_process_pids_by_package_cache.end()) {
        return;
    }
    out_pids = it->second;
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
        char msg[256];
        safe_snprintf(msg, sizeof(msg), "前台PID更新 | app:%s | pid:%d", g_foreground_package, (int)new_pid);
        /* 状态变化类消息：留痕即可，不做逐次刷屏。 */
        log_message_throttled("前台PID更新", 60000, msg);
    }
    g_foreground_pid = new_pid;
}

static void collect_process_threads_for_package_locked(
    pid_t pid,
    const char* package,
    bool use_proc_runtime,
    long long now_ms,
    std::unordered_set<int>& seen) {
    if (pid <= 0 || !package || !package[0]) return;

    /* 验收用：统计 /proc 全量枚举发生了多少次（事件驱动的目标是把这里压下去）。 */
    g_runtime_stats.thread_proc_scan_count.fetch_add(1);

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
        seen.insert((int)tid);
    }
    closedir(dir);
}

static void apply_explicit_persistent_actions_for_process_locked(
    pid_t pid,
    const char* package,
    long long now_ms) {
    if (pid <= 0 || !package || !package[0]) return;
    if (should_pause_our_game_scheduler()) return;
    bool locked_foreground_session =
        g_scheduler_session.phase == SESSION_LOCKED &&
        g_scheduler_session.pid > 0 &&
        pid == g_scheduler_session.pid &&
        pid == g_foreground_pid &&
        strcmp(g_scheduler_session.package, package) == 0 &&
        strcmp(g_foreground_package, package) == 0;
    if (locked_foreground_session) {
        return;
    }

    for (auto& kv : g_threads) {
        ThreadRuntimeState& state = kv.second;
        if (state.pid != pid || state.package != package) {
            continue;
        }

        ThreadJsonResolvedRule resolved_rule = {};
        const ThreadJsonResolvedRule* matched_rule = is_custom_thread_enabled_for_app(package) ?
            resolve_thread_json_rule_locked(
                package, state.tid, state.comm.c_str(), state.cpu_usage_pct, state.cls, &resolved_rule) :
            NULL;
        if (!matched_rule || !matched_rule->action) {
            release_thread_runtime_locked(state, "persistent_rule_not_matched", true, false);
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
            /* 显式规则绑到单核时，ColorOS 上顺手补一个官方 pipeline 标记 */
            mark_pipeline_task_for_rule_locked(state.tid, matched_rule, explicit_cpus);
            state.assigned_cluster = explicit_cluster;
            state.last_migrate_ms = now_ms;
            state.assigned_cpus = explicit_cpus;
        }
    }
}

static void collect_foreground_threads_locked() {
    long long now_ms = get_current_time_ms();

    /*
     * /proc 枚举的定位变了：从"发现线程"降级为"校验 + 兜底"。
     *
     * 线程的增删现在由 BPF 事件（fork/exec/exit）即时驱动，所以这里只需要
     * 低频校验集合一致性。只有事件通道没建立时（BPF 未就绪，或从未收到事件）
     * 才保留高频枚举。自愈全扫同样按事件流健康度放宽。
     */
    const bool event_channel_healthy = g_bpf_ready.load() && g_process_event_seen;
    long long self_heal_interval_ms = kThreadSelfHealRescanMs;
    int scan_interval_ms = 150;
    bool force_self_heal_scan =
        g_last_thread_self_heal_scan_ms <= 0 ||
        now_ms - g_last_thread_self_heal_scan_ms >= self_heal_interval_ms;
    if (g_scheduler_session.phase == SESSION_LOCKED) {
        scan_interval_ms = 1000;
    } else if (g_scheduler_session.phase == SESSION_BACKGROUND_HOLD) {
        scan_interval_ms = 1500;
    } else if (g_bpf_ready.load()) {
        /*
         * 事件通道健康时，枚举只是兜底：fork/exec/exit 已经负责增删，
         * 这里 5s 一次足够纠正偶发丢事件；不健康时保留原来的 400ms。
         */
        scan_interval_ms = event_channel_healthy ? 5000 : 400;
    }
    if (event_channel_healthy) {
        self_heal_interval_ms = 20000;
        force_self_heal_scan =
            g_last_thread_self_heal_scan_ms <= 0 ||
            now_ms - g_last_thread_self_heal_scan_ms >= self_heal_interval_ms;
    }
    if (!force_self_heal_scan &&
        g_last_thread_scan_ms > 0 &&
        now_ms - g_last_thread_scan_ms < scan_interval_ms) {
        return;
    }

    if (force_self_heal_scan) {
        refresh_process_package_cache_locked(true);
        refresh_foreground_pid_locked(true);
        g_last_thread_self_heal_scan_ms = now_ms;
    }

    std::unordered_set<int> seen;
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
        release_dynamic_thread_runtime_locked("no_scan_targets", true);
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
        if (seen.find(it->first) == seen.end()) {
            const bool keep_latched_primary =
                is_latched_primary_tid_locked(it->second.tid) &&
                it->second.pid == g_scheduler_session.pid &&
                is_latched_session_tid_alive_locked(it->second.tid);
            if (keep_latched_primary) {
                ++it;
                continue;
            }
            release_thread_runtime_locked(it->second, "thread_gone", true, true);
            /* 线程已经消失，救不回掩码，账本也一并丢掉，避免无上限增长。 */
            forget_thread_affinity_locked(it->first);
            forget_thread_cpuset_locked(it->first);
            it = g_threads.erase(it);
        } else {
            ++it;
        }
    }
    cleanup_stale_selection_epochs_locked();
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

static const char* session_phase_to_string(SchedulerSessionPhase phase) {
    switch (phase) {
    case SESSION_LEARNING:
        return "LEARNING";
    case SESSION_LOCKED:
        return "LOCKED";
    case SESSION_BACKGROUND_HOLD:
        return "BACKGROUND_HOLD";
    case SESSION_IDLE:
    default:
        return "IDLE";
    }
}

static ThreadClass classify_thread_role_for_lock(pid_t tid, const char* comm, float cpu_usage_pct) {
    if (is_main_thread_name_for_package(g_foreground_package, comm, tid)) {
        return THREAD_CLASS_MAIN;
    }
    if (is_game_engine_priority_thread(g_foreground_package, comm, tid)) {
        return THREAD_CLASS_RENDER;
    }
    if (is_worker_thread_name(comm) || is_taskgraph_thread_name(comm) ||
        is_thread_dash_name(comm) || should_promote_generic_worker(g_foreground_package, comm, cpu_usage_pct)) {
        return THREAD_CLASS_BURST;
    }
    return THREAD_CLASS_BACKGROUND;
}

static void clear_locked_class_assignments_locked() {
    for (int i = 0; i < 4; i++) {
        g_scheduler_session.class_assignments[i].valid = false;
        g_scheduler_session.class_assignments[i].cluster = -1;
        g_scheduler_session.class_assignments[i].cpus.clear();
    }
}

static LockedClassAssignment choose_locked_assignment_for_class_locked(ThreadClass cls) {
    if (cls >= THREAD_CLASS_MAIN && cls <= THREAD_CLASS_BACKGROUND) {
        const LockedClassAssignment& exact = g_scheduler_session.class_assignments[(int)cls];
        if (exact.valid) {
            return exact;
        }
    }
    const LockedClassAssignment& fallback =
        g_scheduler_session.class_assignments[(int)THREAD_CLASS_BACKGROUND];
    if (fallback.valid) {
        return fallback;
    }
    LockedClassAssignment invalid = {};
    invalid.valid = false;
    invalid.cluster = -1;
    return invalid;
}

static void reset_scheduler_session_locked(const char* reason) {
    char old_package[MAX_APP_NAME_LEN];
    safe_strncpy(old_package, g_scheduler_session.package, sizeof(old_package));
    pid_t old_pid = g_scheduler_session.pid;
    SchedulerSessionPhase old_phase = g_scheduler_session.phase;
    g_scheduler_session.phase = SESSION_IDLE;
    g_scheduler_session.pid = -1;
    g_scheduler_session.package[0] = '\0';
    g_scheduler_session.learning_started_ms = 0;
    g_scheduler_session.locked_at_ms = 0;
    g_scheduler_session.background_since_ms = 0;
    g_scheduler_session.last_guard_ms = 0;
    g_scheduler_session.latched_main_tid = 0;
    g_scheduler_session.latched_render_tid = 0;
    g_scheduler_session.latched_main_loss_reason[0] = '\0';
    g_scheduler_session.latched_render_loss_reason[0] = '\0';
    clear_locked_class_assignments_locked();
    if (reason && reason[0]) {
        char msg[320];
        safe_snprintf(msg, sizeof(msg),
                      "调度会话重置 | from:%s | app:%s | pid:%d | reason:%s",
                      session_phase_to_string(old_phase),
                      old_package[0] ? old_package : "none",
                      (int)old_pid,
                      reason);
        log_affinity_message(msg);
        trace_instantf("murong_session", "reset phase=%s app=%s pid=%d reason=%s",
                       session_phase_to_string(old_phase),
                       old_package[0] ? old_package : "none",
                       (int)old_pid,
                       reason);
    }
}

static void begin_learning_session_locked(long long now_ms) {
    reset_scheduler_session_locked("");
    g_scheduler_session.phase = SESSION_LEARNING;
    g_scheduler_session.pid = g_foreground_pid;
    safe_strncpy(g_scheduler_session.package, g_foreground_package, sizeof(g_scheduler_session.package));
    g_scheduler_session.learning_started_ms = now_ms;
    char msg[320];
    safe_snprintf(msg, sizeof(msg),
                  "调度会话开始学习 | app:%s | pid:%d | phase:%s",
                  g_scheduler_session.package,
                  (int)g_scheduler_session.pid,
                  session_phase_to_string(g_scheduler_session.phase));
    log_affinity_message(msg);
    trace_instantf("murong_session", "learning app=%s pid=%d",
                   g_scheduler_session.package,
                   (int)g_scheduler_session.pid);
}

static bool build_expected_mask_for_assignment(
    const LockedClassAssignment& assignment,
    cpu_set_t* out_mask) {
    if (!out_mask || !assignment.valid) return false;
    CPU_ZERO(out_mask);
    if (!assignment.cpus.empty()) {
        for (size_t i = 0; i < assignment.cpus.size(); i++) {
            int cpu = assignment.cpus[i];
            if (cpu >= 0 && cpu < CPU_SETSIZE) {
                CPU_SET(cpu, out_mask);
            }
        }
        return CPU_COUNT(out_mask) > 0;
    }
    if (assignment.cluster < 0 || assignment.cluster >= cluster_count) return false;
    for (int i = 0; i < cpu_clusters[assignment.cluster].cpu_count; i++) {
        CPU_SET(cpu_clusters[assignment.cluster].cpu_ids[i], out_mask);
    }
    return CPU_COUNT(out_mask) > 0;
}

static bool is_thread_assignment_matching_locked(
    pid_t tid,
    const LockedClassAssignment& assignment) {
    cpu_set_t expected_mask;
    if (!build_expected_mask_for_assignment(assignment, &expected_mask)) return false;
    cpu_set_t actual_mask;
    CPU_ZERO(&actual_mask);
    if (sched_getaffinity(tid, sizeof(actual_mask), &actual_mask) != 0) {
        return false;
    }
    return CPU_EQUAL(&expected_mask, &actual_mask);
}

/*
 * 绑核收敛退避。
 *
 * 有些线程受 cpuset 限制（例如被放在 background 组），我们写的目标掩码与它
 * 实际能拿到的掩码永远不相等：写是成功的，但回读永远不匹配，于是动态路径每
 * 500ms 就重写一次，实测能到 900+ 次/秒。这里给"写了但没收敛"的线程退避：
 * 连续 3 次不收敛就停 30s，期间不写、不读掩码，让它安静跑。
 */
static bool thread_affinity_backoff_active_locked(const ThreadRuntimeState& state, long long now_ms) {
    return state.affinity_yield_until_ms > now_ms;
}

static void thread_affinity_note_result_locked(ThreadRuntimeState& state,
                                               bool converged,
                                               long long now_ms) {
    if (converged) {
        state.affinity_retry_streak = 0;
        state.affinity_yield_until_ms = 0;
        return;
    }
    state.affinity_retry_streak++;
    if (state.affinity_retry_streak >= 3) {
        state.affinity_retry_streak = 0;
        state.affinity_yield_until_ms = now_ms + 30000;
        g_runtime_stats.affinity_backoff_count.fetch_add(1);
        char msg[256];
        safe_snprintf(msg, sizeof(msg),
                      "绑核无法收敛，退避 30s | tid:%d | comm:%s（受 cpuset 限制）",
                      (int)state.tid, state.comm.c_str());
        log_affinity_message(msg);
    }
}

static bool should_fast_lock_learning_session_locked(long long now_ms) {
    if (g_scheduler_session.phase != SESSION_LEARNING) return false;
    if (g_scheduler_session.pid <= 0) return false;
    if (strcmp(g_scheduler_session.package, g_foreground_package) != 0) return false;

    long long elapsed_ms = now_ms - g_scheduler_session.learning_started_ms;
    long long fast_lock_min_ms = g_scheduler_session_config.learning_duration_ms;
    if (fast_lock_min_ms > kFastLockMaxLearningMs) {
        fast_lock_min_ms = kFastLockMaxLearningMs;
    }
    if (fast_lock_min_ms < kFastLockMinLearningMs) {
        fast_lock_min_ms = kFastLockMinLearningMs;
    }
    if (elapsed_ms < fast_lock_min_ms) {
        return false;
    }

    struct RoleCandidate {
        bool valid;
        pid_t tid;
        float score;
        long long last_migrate_ms;
        LockedClassAssignment assignment;
    };
    RoleCandidate best[4] = {};
    for (auto& kv : g_threads) {
        const ThreadRuntimeState& state = kv.second;
        if (state.pid != g_scheduler_session.pid) continue;
        if (state.package != g_scheduler_session.package) continue;
        if (state.has_explicit_affinity_rule) continue;
        if (state.assigned_cluster < 0) continue;

        ThreadClass role = classify_thread_role_for_lock(
            state.tid, state.comm.c_str(), state.cpu_usage_pct);
        RoleCandidate& candidate = best[(int)role];
        if (candidate.valid && candidate.score > state.cpu_usage_pct) {
            continue;
        }

        candidate.valid = true;
        candidate.tid = state.tid;
        candidate.score = state.cpu_usage_pct;
        candidate.last_migrate_ms = state.last_migrate_ms;
        candidate.assignment.valid = true;
        candidate.assignment.cluster = state.assigned_cluster;
        candidate.assignment.cpus = state.assigned_cpus;
    }

    int valid_roles = 0;
    bool has_primary_role =
        best[(int)THREAD_CLASS_MAIN].valid || best[(int)THREAD_CLASS_RENDER].valid;
    if (!has_primary_role) {
        return false;
    }

    for (int i = 0; i < 4; i++) {
        const RoleCandidate& candidate = best[i];
        if (!candidate.valid) continue;
        valid_roles++;
        if (candidate.last_migrate_ms > 0 &&
            now_ms - candidate.last_migrate_ms < kFastLockStableWindowMs) {
            return false;
        }
        if (!is_thread_assignment_matching_locked(candidate.tid, candidate.assignment)) {
            return false;
        }
    }

    return valid_roles >= 2;
}

static bool resolve_locked_assignment_for_state_locked(
    const ThreadRuntimeState& state,
    LockedClassAssignment* out_assignment) {
    if (!out_assignment) return false;
    out_assignment->valid = false;
    out_assignment->cluster = -1;
    out_assignment->cpus.clear();
    if (g_scheduler_session.phase != SESSION_LOCKED) return false;
    if (state.pid != g_scheduler_session.pid) return false;
    if (state.package != g_scheduler_session.package) return false;
    clear_invalid_latched_primary_tids_locked();
    bool is_named_main =
        is_main_thread_name_for_package(g_foreground_package, state.comm.c_str(), state.tid);
    bool is_named_render =
        !is_named_main &&
        is_game_engine_priority_thread(g_foreground_package, state.comm.c_str(), state.tid);
    ThreadClass role = classify_thread_role_for_lock(
        state.tid, state.comm.c_str(), state.cpu_usage_pct);
    if (g_scheduler_session.latched_main_tid > 0) {
        if (state.tid == g_scheduler_session.latched_main_tid) {
            role = THREAD_CLASS_MAIN;
        } else if (is_named_main) {
            return false;
        }
    }
    if (g_scheduler_session.latched_render_tid > 0) {
        if (state.tid == g_scheduler_session.latched_render_tid) {
            role = THREAD_CLASS_RENDER;
        } else if (is_named_render) {
            return false;
        }
    }
    LockedClassAssignment assignment = choose_locked_assignment_for_class_locked(role);
    if (!assignment.valid) {
        return false;
    }
    *out_assignment = assignment;
    return true;
}

static bool snapshot_locked_assignments_locked(long long now_ms) {
    struct Candidate {
        bool valid;
        pid_t tid;
        float score;
        int cluster;
        std::vector<int> cpus;
    };
    Candidate best[4] = {};
    for (auto& kv : g_threads) {
        const ThreadRuntimeState& state = kv.second;
        if (state.pid != g_scheduler_session.pid) continue;
        if (state.package != g_scheduler_session.package) continue;
        if (state.assigned_cluster < 0) continue;
        ThreadClass role = classify_thread_role_for_lock(state.tid, state.comm.c_str(), state.cpu_usage_pct);
        Candidate& candidate = best[(int)role];
        float score = state.cpu_usage_pct;
        if (!candidate.valid || score >= candidate.score) {
            candidate.valid = true;
            candidate.tid = state.tid;
            candidate.score = score;
            candidate.cluster = state.assigned_cluster;
            candidate.cpus = state.assigned_cpus;
        }
    }
    clear_invalid_latched_primary_tids_locked();
    auto pin_candidate_from_latched_tid = [&](pid_t tid, ThreadClass role) {
        if (tid <= 0) return;
        auto it = g_threads.find((int)tid);
        if (it == g_threads.end()) return;
        const ThreadRuntimeState& state = it->second;
        if (state.pid != g_scheduler_session.pid) return;
        if (state.package != g_scheduler_session.package) return;
        if (state.assigned_cluster < 0) return;
        Candidate& candidate = best[(int)role];
        candidate.valid = true;
        candidate.tid = state.tid;
        candidate.score = state.cpu_usage_pct;
        candidate.cluster = state.assigned_cluster;
        candidate.cpus = state.assigned_cpus;
    };
    pin_candidate_from_latched_tid(g_scheduler_session.latched_main_tid, THREAD_CLASS_MAIN);
    pin_candidate_from_latched_tid(g_scheduler_session.latched_render_tid, THREAD_CLASS_RENDER);
    bool any_valid = false;
    clear_locked_class_assignments_locked();
    for (int i = 0; i < 4; i++) {
        if (!best[i].valid) continue;
        g_scheduler_session.class_assignments[i].valid = true;
        g_scheduler_session.class_assignments[i].cluster = best[i].cluster;
        g_scheduler_session.class_assignments[i].cpus = best[i].cpus;
        any_valid = true;
    }
    if (!any_valid) return false;
    if (g_scheduler_session.latched_main_tid <= 0 && best[(int)THREAD_CLASS_MAIN].valid) {
        g_scheduler_session.latched_main_tid = best[(int)THREAD_CLASS_MAIN].tid;
        set_latched_primary_loss_reason_locked(THREAD_CLASS_MAIN, "");
    }
    if (g_scheduler_session.latched_render_tid <= 0 && best[(int)THREAD_CLASS_RENDER].valid) {
        g_scheduler_session.latched_render_tid = best[(int)THREAD_CLASS_RENDER].tid;
        set_latched_primary_loss_reason_locked(THREAD_CLASS_RENDER, "");
    }
    g_scheduler_session.phase = SESSION_LOCKED;
    g_scheduler_session.locked_at_ms = now_ms;
    g_scheduler_session.last_guard_ms = 0;
    char msg[512];
    safe_snprintf(msg, sizeof(msg),
                  "调度会话已锁定 | app:%s | pid:%d | main:%d | render:%d | worker:%d | other:%d",
                  g_scheduler_session.package,
                  (int)g_scheduler_session.pid,
                  g_scheduler_session.class_assignments[(int)THREAD_CLASS_MAIN].valid ? 1 : 0,
                  g_scheduler_session.class_assignments[(int)THREAD_CLASS_RENDER].valid ? 1 : 0,
                  g_scheduler_session.class_assignments[(int)THREAD_CLASS_BURST].valid ? 1 : 0,
                  g_scheduler_session.class_assignments[(int)THREAD_CLASS_BACKGROUND].valid ? 1 : 0);
    log_affinity_message(msg);
    trace_instantf("murong_session", "locked app=%s pid=%d", g_scheduler_session.package, (int)g_scheduler_session.pid);
    return true;
}

static void update_scheduler_session_locked(long long now_ms) {
    bool current_is_game = g_foreground_pid > 0 &&
                           g_foreground_package[0] &&
                           strcmp(g_foreground_package, "unknown") != 0 &&
                           is_game_package_name(g_foreground_package);
    if (!current_is_game) {
        return;
    }
    if (active_thread_app_rule_uses_explicit_affinity_locked()) {
        if (g_scheduler_session.phase != SESSION_IDLE) {
            reset_scheduler_session_locked("explicit_thread_affinity");
        }
        return;
    }
    if (g_scheduler_session.phase == SESSION_IDLE) {
        begin_learning_session_locked(now_ms);
        return;
    }
    if (strcmp(g_scheduler_session.package, g_foreground_package) != 0) {
        begin_learning_session_locked(now_ms);
        return;
    }
    if (g_scheduler_session.phase == SESSION_BACKGROUND_HOLD) {
        if (g_scheduler_session.pid == g_foreground_pid) {
            g_scheduler_session.phase = g_scheduler_session.locked_at_ms > 0 ? SESSION_LOCKED : SESSION_LEARNING;
            g_scheduler_session.background_since_ms = 0;
            char msg[256];
            safe_snprintf(msg, sizeof(msg),
                          "调度会话恢复 | app:%s | pid:%d | phase:%s",
                          g_scheduler_session.package,
                          (int)g_scheduler_session.pid,
                          session_phase_to_string(g_scheduler_session.phase));
            log_affinity_message(msg);
            trace_instantf("murong_session", "resume app=%s pid=%d phase=%s",
                           g_scheduler_session.package,
                           (int)g_scheduler_session.pid,
                           session_phase_to_string(g_scheduler_session.phase));
            return;
        }
        begin_learning_session_locked(now_ms);
        return;
    }
    if (g_scheduler_session.pid != g_foreground_pid) {
        begin_learning_session_locked(now_ms);
        return;
    }
    if (g_scheduler_session.phase == SESSION_LOCKED) {
        clear_invalid_latched_primary_tids_locked();
        bool need_main_reselect =
            g_scheduler_session.latched_main_tid <= 0 &&
            has_locked_primary_reselection_candidate_locked(THREAD_CLASS_MAIN);
        bool need_render_reselect =
            g_scheduler_session.latched_render_tid <= 0 &&
            has_locked_primary_reselection_candidate_locked(THREAD_CLASS_RENDER);
        if (need_main_reselect || need_render_reselect) {
            if (need_main_reselect) {
                log_locked_primary_reselection_locked(THREAD_CLASS_MAIN);
            }
            if (need_render_reselect) {
                log_locked_primary_reselection_locked(THREAD_CLASS_RENDER);
            }
            snapshot_locked_assignments_locked(now_ms);
            return;
        }
    }
    if (g_scheduler_session.phase == SESSION_LEARNING &&
        should_fast_lock_learning_session_locked(now_ms)) {
        snapshot_locked_assignments_locked(now_ms);
        return;
    }
    if (g_scheduler_session.phase == SESSION_LEARNING &&
        now_ms - g_scheduler_session.learning_started_ms >= g_scheduler_session_config.learning_duration_ms) {
        /* Phase4: 锁定前确保分配已稳定 — 避免学习刚结束就因瞬态漂移锁错 */
        bool all_stable = true;
        if (now_ms - g_scheduler_session.learning_started_ms < g_scheduler_session_config.learning_duration_ms * 2) {
            for (auto& kv : g_threads) {
                if (kv.second.pid != g_scheduler_session.pid) continue;
                if (kv.second.assigned_cluster < 0) continue;
                if (kv.second.last_migrate_ms > 0 &&
                    now_ms - kv.second.last_migrate_ms < kSelectionChangeThresholdMs) {
                    all_stable = false;
                    break;
                }
            }
        }
        if (all_stable) {
            snapshot_locked_assignments_locked(now_ms);
        }
    }
}

static bool guard_locked_assignments_locked(long long now_ms, bool force_recheck) {
    if (g_scheduler_session.phase != SESSION_LOCKED) return false;
    if (g_scheduler_session.pid <= 0 || g_foreground_pid != g_scheduler_session.pid) return false;
    if (strcmp(g_scheduler_session.package, g_foreground_package) != 0) return false;
    if (!force_recheck &&
        g_scheduler_session.last_guard_ms > 0 &&
        now_ms - g_scheduler_session.last_guard_ms < g_scheduler_session_config.guard_interval_ms) {
        return true;
    }
    g_scheduler_session.last_guard_ms = now_ms;
    for (auto& kv : g_threads) {
        ThreadRuntimeState& state = kv.second;
        if (state.pid != g_scheduler_session.pid) continue;
        if (state.package != g_scheduler_session.package) continue;
        /* Phase1: 跳过已命中显式规则的线程 */
        if (state.has_explicit_affinity_rule) continue;
        /* Phase4: 跳过从未分配过的线程 */
        if (state.assigned_cluster < 0) continue;
        /* 已经让位的线程不再重打：这条线程上有比我们更强/更正确的写入者。 */
        if (state.guard_yielded) {
            g_runtime_stats.guard_yield_count.fetch_add(1);
            continue;
        }
        /* 锁定后只走执行闭环：从学习快照里取稳定目标 */
        LockedClassAssignment assignment = {};
        if (!resolve_locked_assignment_for_state_locked(state, &assignment)) {
            continue;
        }
        /* 只检查实际 affinity 是否被外部篡改 */
        bool actual_match = is_thread_assignment_matching_locked(state.tid, assignment);
        if (actual_match) {
            state.guard_reapply_streak = 0;
            continue;
        }
        /* Phase4: 重绑冷却 — 避免短时间内外部和我们的竞争导致反复重打 */
        if (state.last_migrate_ms > 0 && now_ms - state.last_migrate_ms < kRebindCooldownMs) {
            continue;
        }
        if (!should_pause_our_game_scheduler() && !g_official_tuner_disabled) {
            sync_official_tuner_for_game(g_foreground_package);
        }
        bool applied = !assignment.cpus.empty() ?
            set_thread_affinity_mask(state.tid, assignment.cpus) :
            set_thread_affinity(state.tid, assignment.cluster);
        if (applied) {
            g_runtime_stats.guard_reapply_count.fetch_add(1);
            state.assigned_cluster = assignment.cluster;
            state.assigned_cpus = assignment.cpus;
            state.last_migrate_ms = now_ms;
            /*
             * 让位判据：同一条线程连续被外部改写多次，说明对面有更强的意图
             * （官方调度器、系统组件、或游戏自己）。继续重打只会互相踩，
             * 结果是两边都做不好——实测里那种最差轮次就是这个形态。
             * 到达阈值后就放手，这条线程交回默认调度。
             */
            state.guard_reapply_streak++;
            if (state.guard_reapply_streak >= kGuardYieldStreak) {
                state.guard_yielded = true;
                char yield_msg[320];
                safe_snprintf(yield_msg, sizeof(yield_msg),
                              "锁定守护让位 | app:%s | tid:%d | comm:%s | 连续被外部改写 %d 次，交回默认调度",
                              g_scheduler_session.package,
                              (int)state.tid,
                              state.comm.c_str(),
                              state.guard_reapply_streak);
                g_runtime_stats.guard_yield_count.fetch_add(1);
                log_affinity_message(yield_msg);
                continue;
            }
            char msg[320];
            safe_snprintf(msg, sizeof(msg),
                          "锁定守护重打 | app:%s | pid:%d | tid:%d | tid_assign | cluster:%d | mask_size:%d",
                          g_scheduler_session.package,
                          (int)g_scheduler_session.pid,
                          (int)state.tid,
                          assignment.cluster,
                          (int)assignment.cpus.size());
            log_scheduler_probe_once(msg);
            trace_instantf("murong_guard", "reapply app=%s pid=%d tid=%d cluster=%d mask=%d",
                           g_scheduler_session.package,
                           (int)g_scheduler_session.pid,
                           (int)state.tid,
                           assignment.cluster,
                           (int)assignment.cpus.size());
        }
    }
    return true;
}

static float avg_idle_ratio(int cluster_id, const AllCpuStats& stats) {
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

static float get_freq_headroom(int cluster_id) {
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

static float estimate_thermal_pressure() {
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

float compute_cluster_score(int cluster_id, const ClusterPolicy& policy, const AllCpuStats& stats, float thermal) {
    float idle_ratio = avg_idle_ratio(cluster_id, stats);
    float freq_headroom = get_freq_headroom(cluster_id);
    float rq_pressure = 1.0f - idle_ratio;
    return idle_ratio * policy.w_idle +
           freq_headroom * policy.w_freq -
           rq_pressure * policy.w_rq -
           thermal * policy.w_thermal;
}

int select_target_cluster(ThreadClass cls, const std::vector<float>& cluster_scores) {
    if (cluster_scores.empty()) return 0;

    int start = 0;
    if (cls == THREAD_CLASS_RENDER || cls == THREAD_CLASS_MAIN) {
        start = cluster_scores.size() > 1 ? (int)cluster_scores.size() / 2 : 0;
    } else if (cls == THREAD_CLASS_BURST) {
        start = cluster_scores.size() > 2 ? 1 : 0;
    }

    int best = start;
    float best_score = cluster_scores[start];
    for (int i = start; i < (int)cluster_scores.size(); i++) {
        if (cluster_scores[i] > best_score) {
            best = i;
            best_score = cluster_scores[i];
        }
    }
    if (cls == THREAD_CLASS_BACKGROUND) {
        return 0;
    }
    return best;
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

    /*
     * 记账：第一次把这条线程挪去别的 cpuset 之前，先记下它原本属于哪一组，
     * 释放时才能挪回去。不做这件事的话，被恢复路径搬进 top-app 的线程会永远
     * 留在前台分组里，等于变相扩权。
     */
    record_cpuset_membership_baseline_locked(tid);

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
    /* 第一次动手之前先记下原始掩码，关掉功能时才能撤回全核。 */
    capture_thread_affinity_baseline_locked(tid);
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
        mark_thread_affinity_dirty_locked(tid);
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
    capture_thread_affinity_baseline_locked(tid);
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
        mark_thread_affinity_dirty_locked(tid);
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

