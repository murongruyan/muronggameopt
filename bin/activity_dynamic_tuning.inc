// activity_dynamic_tuning.inc — 动态调频执行器
#include <sys/time.h>
#include "activity_common.inc"

/*
 * sysfs 的数值文件读出来带结尾换行（"384000\n"），直接塞进日志会把一条记录
 * 拆成两行，看起来像少了一半。这里就地裁掉尾部空白，只为日志/诊断服务。
 */
static void trim_trailing_whitespace(char* text) {
    if (!text) return;
    size_t len = strlen(text);
    while (len > 0 && (text[len - 1] == '\n' || text[len - 1] == '\r' ||
                       text[len - 1] == ' ' || text[len - 1] == '\t')) {
        text[--len] = '\0';
    }
}

static int round_freq_up_to_table(int policy_idx, int target_freq) {
    if (policy_idx < 0 || policy_idx >= policy_count || g_freq_tables[policy_idx].count <= 0) return target_freq;
    int closest = g_freq_tables[policy_idx].freqs[0], min_diff = 2147483647, max_f = g_freq_tables[policy_idx].freqs[0];
    bool found = false;
    for (int f = 0; f < g_freq_tables[policy_idx].count; f++) {
        int v = g_freq_tables[policy_idx].freqs[f];
        if (v > max_f) max_f = v;
        if (v >= target_freq) { int d = v - target_freq; if (d < min_diff) { min_diff = d; closest = v; found = true; } }
    }
    return found ? closest : max_f;
}

/*
 * round_freq_down_to_table / get_policy_lowest_available_freq / get_policy_safe_floor_freq /
 * refresh_scene_policy_state 已抽到 activity_freq_utils.inc —— 纯线程版也要用它们，
 * 而本文件在纯线程版被条件排除，故不能继续放在这里。
 */


// ── 渲染档位执行器：把动作档位给出的频率区间写到节点 ──
static long long g_render_band_raise_ms[MAX_CLUSTERS] = {0};
static long long g_render_band_release_ms[MAX_CLUSTERS] = {0};

/*
 * 平台让位：scaling_min/max_freq 这对节点上不止我们一个写入者
 * （ColorOS perfservice / thermal / 内核 msm_performance 都会写）。
 * 检测到"不是我们写的"变化时，先让位一小段时间，别和平台抢；
 * 让位窗口过去后再把我们要的区间写回去。
 */
static const long long kPlatformYieldMs = 400;
/* 我们最后一次【期望写入】的值（不是读回值） */
static int g_band_seen_min[MAX_CLUSTERS] = {0};
static int g_band_seen_max[MAX_CLUSTERS] = {0};
/* 我们最后一次写入后【实际读回】的值；平台钳制时用它来判断有没有被外部改动 */
static int g_band_actual_min[MAX_CLUSTERS] = {0};
static int g_band_actual_max[MAX_CLUSTERS] = {0};
static long long g_band_yield_until_ms[MAX_CLUSTERS] = {0};
/* 日用诊断日志（daily_write）的限流时间戳，每簇一份。 */
static long long g_daily_last_log_ms[MAX_CLUSTERS] = {0};

/*
 * 当前是不是"游戏场景"。
 * 游戏里帧优先，平台怎么改频率我们都抢回来（不让位）；日用才让位。
 * 注意不能只看"走 FAS 路径还是负载表路径"——实测有的机型/场景 FAS 拿不到帧信号
 * （queueBuffer 采不到，frame 全是 0），这时游戏跑的是负载表路径，但它仍然是游戏。
 */
static bool current_scene_prefers_frames(const char* app) {
    if (app && app[0] && is_game_package_name(app)) return true;
    if (scene_category_is_hok(g_scene_policy.foreground_category) ||
        scene_category_is_genshin(g_scene_policy.foreground_category) ||
        scene_category_is_generic_game(g_scene_policy.foreground_category)) {
        return true;
    }
    return false;
}

/*
 * 写频率区间，并处理"平台也在写同一对节点"这件事。
 *
 * allow_yield 的语义（按用户的设定）：
 *   游戏（有帧信号，FAS 路径）：**不让位**。帧优先，平台写多少我们都立刻抢回来；
 *   日用（没有帧信号，负载表路径）：可以让位，先让平台 400ms，别互相踩。
 * 之前不分场景一律让位，实测游戏里那 400ms CPU 就真按平台写的低频率跑，
 * 用户的 CSV 里 4 次掉帧有 3 次和 band_yield 同一秒。
 */
static void enforce_band_with_yield(int ci, int pi, int want_min, int want_max, int fmin, int fmax,
                                    long long now, bool allow_yield) {
    if (ci < 0 || ci >= MAX_CLUSTERS) return;
    int hw_min = cpu_policies[pi].hw_min_freq;
    int hw_max = cpu_policies[pi].max_freq;
    if (want_min < fmin) want_min = fmin;
    if (want_min < hw_min) want_min = hw_min;
    if (want_max > fmax) want_max = fmax;
    if (want_max > hw_max) want_max = hw_max;
    if (want_max < want_min) want_max = want_min;

    char path_min[128], path_max[128];
    safe_snprintf(path_min, sizeof(path_min),
                  "/sys/devices/system/cpu/cpufreq/policy%d/scaling_min_freq",
                  cpu_policies[pi].policy_num);
    safe_snprintf(path_max, sizeof(path_max),
                  "/sys/devices/system/cpu/cpufreq/policy%d/scaling_max_freq",
                  cpu_policies[pi].policy_num);
    int seen_min = UnifiedScheduler::read_int_from_file(path_min, g_band_seen_min[ci]);
    int seen_max = UnifiedScheduler::read_int_from_file(path_max, g_band_seen_max[ci]);

    /*
     * 用"上次写完实际读回的值"作基准判断外部改动，而不是用"我们期望写的值"。
     * 否则平台钳制时（期望 2745600、实际 1017600）每拍都会误判成平台改动。
     */
    bool external_change = (g_band_actual_min[ci] != 0 && seen_min != g_band_actual_min[ci]) ||
                           (g_band_actual_max[ci] != 0 && seen_max != g_band_actual_max[ci]);
    g_band_actual_min[ci] = seen_min;
    g_band_actual_max[ci] = seen_max;
    if (external_change) {
        static long long last_yield_log_ms = 0;
        if (!allow_yield) {
            /* 游戏里不让位：记一笔日志（限流）后直接往下走，把我们的区间抢回去 */
            if (now - last_yield_log_ms >= 5000) {
                last_yield_log_ms = now;
                char msg[192];
                safe_snprintf(msg, sizeof(msg),
                              "band_override | c%d 平台改动 %d-%d → 游戏内不让位，直接写回 %d-%d",
                              ci, seen_min, seen_max, want_min, want_max);
                log_message(msg);
            }
        } else {
            g_band_yield_until_ms[ci] = now + kPlatformYieldMs;
            if (now - last_yield_log_ms >= 5000) {
                last_yield_log_ms = now;
                char msg[192];
                safe_snprintf(msg, sizeof(msg),
                              "band_yield | c%d 平台改动 %d-%d → 让位 %lldms",
                              ci, seen_min, seen_max, kPlatformYieldMs);
                log_message(msg);
            }
            return;
        }
    }
    if (allow_yield && now < g_band_yield_until_ms[ci]) return;
    if (seen_min == want_min && seen_max == want_max) return;

    /*
     * 写完后【只把"我们期望的值"记为上次写入值】，实际读回值单独记在 g_band_actual_*。
     *
     * 历史 bug（2026-10-05 定位）：这里原来把 read_int_from_file() 读回的“实际值”存进
     * g_band_seen_*。当平台把写入钳制住（例如把 max 从 2745600 压到 1017600）时，
     * seen_max 会被固化成被钳制后的值，而 want_max 随后也被同样钳制 —— 两者相等，
     * 于是每次都在上面的提前返回处退出，**再也不重写**。表现为整局上限死在 1017600，
     * band_override 只出现十几次后彻底消失。
     */
    UnifiedScheduler::update_cluster_freq(ci, want_min, want_max);
    int actual_min = UnifiedScheduler::read_int_from_file(path_min, -1);
    int actual_max = UnifiedScheduler::read_int_from_file(path_max, -1);
    g_band_seen_min[ci] = want_min;
    g_band_seen_max[ci] = want_max;
    g_band_actual_min[ci] = actual_min;
    g_band_actual_max[ci] = actual_max;
    /* 写不进去时限流记录，避免每拍刷屏（按簇各自节流，否则小核先记就把大核的吞掉了） */
    if (actual_min != want_min || actual_max != want_max) {
        static long long last_clamp_log_ms[MAX_CLUSTERS] = {0};
        if (ci >= 0 && ci < MAX_CLUSTERS && now - last_clamp_log_ms[ci] >= 5000) {
            last_clamp_log_ms[ci] = now;
            char dbg[256];
            safe_snprintf(dbg, sizeof(dbg),
                          "写入被钳制 | c%d 期望 %d-%d | 实际 %d-%d",
                          ci, want_min, want_max, actual_min, actual_max);
            log_message(dbg);
        }
    }
}

/*
 * 动作档位 → 频率区间 (min, max)。
 *
 * 升频立即生效；降频按步进释放，避免帧债刚消失就把频率一下塌到地板，
 * 也避免"降频 → 立刻卡 → 再升频"的来回抖动。
 */
static void apply_render_band(int ci, int pi, int band_min, int band_max,
                              int fmin, int fmax, int* last, long long now,
                              int clamp_floor = -1) {
    if (ci < 0 || ci >= MAX_CLUSTERS) return;
    int hw_min = cpu_policies[pi].hw_min_freq;
    int hw_max = cpu_policies[pi].max_freq;
    /*
     * 下限的地板：默认是档位下限（fmin）。游戏侧传硬件最低档进来——
     * 官方对游戏就是这么干的（scaling_min 写频率表第 0 档），
     * 让空闲瞬间能真的掉到最低频，而不是被档位下限顶住。
     */
    int floor_limit = (clamp_floor >= 0) ? clamp_floor : fmin;
    if (floor_limit > fmin) floor_limit = fmin;
    if (band_max > fmax) band_max = fmax;
    if (band_max > hw_max) band_max = hw_max;
    if (band_min < floor_limit) band_min = floor_limit;
    if (band_min < hw_min) band_min = hw_min;
    if (band_min > band_max) band_min = band_max;

    int want_max = round_freq_down_to_table(pi, band_max);
    int want_min = round_freq_up_to_table(pi, band_min);
    if (want_min > want_max) want_min = want_max;

    int current = *last > 0 ? *last : want_max;
    int applied = want_max;
    if (want_max < current) {
        /*
         * 降频步长与间隔来自 dyn.json 的 control（默认 25% 范围 / 60ms）。
         * 死区已经挡住了噪声抖动，这里不用再慢慢磨——磨得越久，
         * 中间那些"释放路上的频率"就越是白花的预算。
         */
        if (now - g_render_band_raise_ms[ci] < 120) return;   /* 刚升过频，先稳住 */
        if (now - g_render_band_release_ms[ci] < g_dyn_control.release_hold_ms) return;
        int range = std::max(1, fmax - fmin);
        int step = std::max(range * g_dyn_control.release_step_permille / 1000, 64000);
        int next = current - step;
        if (next < want_max) next = want_max;
        applied = round_freq_down_to_table(pi, next);
        if (applied > current) applied = current;
        if (applied < want_max) applied = want_max;
        g_render_band_release_ms[ci] = now;
    } else if (want_max > current) {
        g_render_band_raise_ms[ci] = now;
    }
    if (applied < want_min) applied = want_min;

    /* FAS 路径只服务游戏，游戏内不让位 */
    enforce_band_with_yield(ci, pi, want_min, applied, fmin, fmax, now, false);
    *last = applied;
    g_virtual_freq[ci] = applied;
}

/*
 * 日用动态调频的负载信号：这一簇的平均占用率（%）。
 * 数据来自 /proc/stat 的每核累计值（内核已聚合），一次读、算增量，不扫线程。
 * 自带上一拍快照：线程调度那条路径在非游戏时会提前返回，不能依赖它的 g_prev_stats。
 */
static AllCpuStats g_daily_prev_stats = {};
static bool g_daily_prev_valid = false;

static int calc_cluster_load_percent_for_daily(int cluster_id, const AllCpuStats& stats, int load_mode) {
    if (cluster_id < 0 || cluster_id >= cluster_count || !stats.valid) return -1;
    if (!g_daily_prev_valid) return -1;
    float busy[32];
    int count = 0;
    for (int c = 0; c < cpu_clusters[cluster_id].cpu_count && c < 32; c++) {
        int cid = cpu_clusters[cluster_id].cpu_ids[c];
        if (cid < 0) continue;
        uint64_t idle_now = stats.idle[cid];
        uint64_t total_now = stats.total[cid];
        uint64_t idle_prev = g_daily_prev_stats.idle[cid];
        uint64_t total_prev = g_daily_prev_stats.total[cid];
        if (total_now <= total_prev) continue;
        uint64_t idle_delta = idle_now >= idle_prev ? (idle_now - idle_prev) : 0;
        uint64_t total_delta = total_now - total_prev;
        float core_busy = 1.0f - (float)idle_delta / (float)total_delta;
        if (core_busy < 0.0f) core_busy = 0.0f;
        if (core_busy > 1.0f) core_busy = 1.0f;
        busy[count] = core_busy;
        count++;
    }
    if (count <= 0) return -1;

    /*
     * 负载口径：
     *   avg   平均占用——平稳，但"1 核跑满、其余空闲"会被平均掉；
     *   top2  前二核平均——单核瓶颈不会被掩盖，也不被单核瞬时抖动带跑（默认）；
     *   peak  峰值——最敏感，也最容易被抖动带跑。
     */
    if (load_mode == 2) {
        float peak = 0.0f;
        for (int k = 0; k < count; k++) if (busy[k] > peak) peak = busy[k];
        return (int)(peak * 100.0f + 0.5f);
    }
    if (load_mode == 1 && count > 1) {
        float first = 0.0f, second = 0.0f;
        for (int k = 0; k < count; k++) {
            if (busy[k] > first) {
                second = first;
                first = busy[k];
            } else if (busy[k] > second) {
                second = busy[k];
            }
        }
        return (int)((first + second) * 50.0f + 0.5f);
    }
    float sum = 0.0f;
    for (int k = 0; k < count; k++) sum += busy[k];
    return (int)(sum * 100.0f / (float)count + 0.5f);
}

static void update_daily_stats_snapshot(const AllCpuStats& stats) {
    if (!stats.valid) return;
    g_daily_prev_stats = stats;
    g_daily_prev_valid = true;
}

/*
 * control_trace：按 FAS 理念重构前的观测层（只记录，不参与控制）。
 *
 * 记录的是"这一帧还差多少时间"的派生量：
 *   budget_us = 1e6 / target_fps        （渲染时间预算，144fps → 6944us）
 *   frame_us  = 最近一帧的实测帧时间（FAS 引擎缓存）
 *   debt_us   = frame_us - budget_us     （正数 = 这一帧已经晚了）
 * 同时记录动作落点（是否贴在 fmin / fmax）与顶格次数，用来判断"钱花在哪"。
 *
 * 每 5 秒汇总一行，避免逐帧刷日志；目的是先拿到真实分布，再决定算法怎么改。
 */
#define CONTROL_TRACE_WINDOW_MS 5000

struct ControlTraceWindow {
    long long start_ms;
    int samples;
    int fas_on_samples;
    int budget_us;
    int frame_min_us;
    int frame_max_us;
    long long frame_sum_us;
    int debt_min_us;
    int debt_max_us;
    long long debt_sum_us;
    int tier_ok;      /* debt <= 0          : 预算内 */
    int tier_low;     /* 0 < debt <= 10%    : 轻微超预算 */
    int tier_mid;     /* 10% < debt <= 30%  : 明显超预算 */
    int tier_high;    /* debt > 30%         : 严重超预算 */
    int floor_hits;   /* 实际频率贴在该簇 min */
    int ceil_hits;    /* 实际频率贴在该簇 max */
    int ceil_events;  /* 顶格事件次数：动作档位要求顶 fmax */
    int applied_min_khz;
    int applied_max_khz;
    /* 目标帧率是否稳定：窗口里见过哪些档位、跳了几次 */
    int target_fps_last;
    int target_fps_min;
    int target_fps_max;
    int target_flips;
    /* 动作档位计数：深睡/甜点/放开档位上限/顶格 */
    int action_ds;
    int action_sweet;
    int action_ceil;
    int action_max;
    /* 帧信号来自哪块 surface：主 surface 是否会中途换人 */
    uint64_t surface_key;
    int surface_flips;
    /* 这个窗口里最差的一帧，以及当时 FAS 的档位/实际写下去的区间 */
    int worst_frame_us;
    int worst_frame_action;
    int worst_frame_applied_khz;
    int worst_frame_sample_cur[MAX_CLUSTERS];
    /*
     * 关键线程（pipeline 标记的那些）的运行队列等待增量。
     * 掉帧到底是"算力不够"还是"被别的线程挤下线"，就靠这个数分辨：
     * 帧时间 >25ms 而 runq 只有几百 us → 游戏自己内部卡（GC/资源/网络），调度救不了；
     * runq 也是 20~40ms → 抢占问题，我们能在绑核/优先级上继续做。
     */
    long long rd_max_ns;
    int rd_tid;
};

static ControlTraceWindow g_control_trace = {};

/* 掉帧现场：各簇当时实际频率 + DDR 当时频率，只在窗口内刷新最差帧时读一次。 */
static void control_trace_capture_freqs(ControlTraceWindow& w) {
    for (int c = 0; c < MAX_CLUSTERS; c++) {
        w.worst_frame_sample_cur[c] = 0;
    }
    int cluster = 0;
    for (int p = 0; p < policy_count && cluster < MAX_CLUSTERS; p++) {
        char path[96];
        safe_snprintf(path, sizeof(path),
                      "/sys/devices/system/cpu/cpufreq/policy%d/scaling_cur_freq",
                      cpu_policies[p].policy_num);
        int freq = read_int_file_simple(path, 0);
        int cid = cpu_policies[p].cluster_id;
        if (cid < 0 || cid >= MAX_CLUSTERS) cid = cluster;
        w.worst_frame_sample_cur[cid] = freq;
        cluster++;
    }
}

static int control_trace_read_ddr_khz(void) {
    return read_int_file_simple("/sys/devices/system/cpu/bus_dcvs/DDR/cur_freq", 0);
}

#define CONTROL_TRACE_RD_SLOTS 8
struct PipelineRdSlot {
    pid_t tid;
    long long last_delay_ns;
};
static PipelineRdSlot g_pipeline_rd_slots[CONTROL_TRACE_RD_SLOTS] = {};
static long long g_pipeline_rd_last_sample_ms = 0;

/* /proc/<tid>/schedstat 第二列 = 这次调度实体在运行队列里等待的累计 ns。 */
static long long read_task_run_delay_ns(pid_t tid) {
    if (tid <= 0) return -1;
    char path[64];
    safe_snprintf(path, sizeof(path), "/proc/%d/schedstat", (int)tid);
    FILE* fp = fopen(path, "r");
    if (!fp) return -1;
    char buf[192] = {0};
    bool ok = fgets(buf, sizeof(buf), fp) != NULL;
    fclose(fp);
    if (!ok) return -1;
    long long running_ns = 0;
    long long delay_ns = 0;
    unsigned long long slices = 0;
    if (sscanf(buf, "%lld %lld %llu", &running_ns, &delay_ns, &slices) != 3) return -1;
    return delay_ns;
}

/*
 * 每 ~200ms 采一次关键线程的运行队列等待，记窗口内的最大增量。
 * 只在有 pipeline 标记（=有游戏在跑、规则命中了关键线程）时才读，最多 8 个线程。
 */
static void control_trace_sample_run_delay(long long now_ms) {
    ControlTraceWindow& w = g_control_trace;
    if (w.start_ms == 0) return;
    if (now_ms - g_pipeline_rd_last_sample_ms < 200) return;
    g_pipeline_rd_last_sample_ms = now_ms;
    int count = UnifiedScheduler::g_pipeline_tid_snapshot_count;
    if (count <= 0) return;
    if (count > CONTROL_TRACE_RD_SLOTS) count = CONTROL_TRACE_RD_SLOTS;

    for (int slot = 0; slot < count; slot++) {
        pid_t tid = UnifiedScheduler::g_pipeline_tid_snapshot[slot];
        if (tid <= 0) continue;
        long long delay = read_task_run_delay_ns(tid);
        if (delay < 0) {
            g_pipeline_rd_slots[slot].tid = tid;
            g_pipeline_rd_slots[slot].last_delay_ns = -1;
            continue;
        }
        if (g_pipeline_rd_slots[slot].tid == tid && g_pipeline_rd_slots[slot].last_delay_ns >= 0) {
            long long delta = delay - g_pipeline_rd_slots[slot].last_delay_ns;
            if (delta > w.rd_max_ns) {
                w.rd_max_ns = delta;
                w.rd_tid = (int)tid;
            }
        }
        g_pipeline_rd_slots[slot].tid = tid;
        g_pipeline_rd_slots[slot].last_delay_ns = delay;
    }
}

static void control_trace_note(int budget_us, int frame_us, int last_frame_us, int debt_us, int action,
                              int applied_khz, int fmin, int fmax, bool fas_on) {
    ControlTraceWindow& w = g_control_trace;
    if (w.start_ms == 0) return;

    w.samples++;
    if (fas_on) w.fas_on_samples++;
    switch (action) {
        case FAS_ACTION_DEEP_SLEEP: w.action_ds++; break;
        case FAS_ACTION_SWEETSPOT: w.action_sweet++; break;
        case FAS_ACTION_SWEETSPOT_CEILING: w.action_ceil++; break;
        case FAS_ACTION_MAX_FREQ: w.action_max++; break;
        default: break;
    }
    if (budget_us > 0) {
        w.budget_us = budget_us;
        int tfps = (int)(1000000 / budget_us + 0.5);
        if (w.target_fps_last == 0) {
            w.target_fps_min = tfps;
            w.target_fps_max = tfps;
        } else if (tfps != w.target_fps_last) {
            w.target_flips++;
            if (tfps < w.target_fps_min) w.target_fps_min = tfps;
            if (tfps > w.target_fps_max) w.target_fps_max = tfps;
        }
        w.target_fps_last = tfps;
    }

    {
        uint64_t key = g_queuebuffer_primary_key;
        if (key != 0) {
            if (w.surface_key == 0) {
                w.surface_key = key;
            } else if (key != w.surface_key) {
                w.surface_flips++;
                w.surface_key = key;
            }
        }
    }

    if (frame_us > 0) {
        if (w.frame_min_us == 0 || frame_us < w.frame_min_us) w.frame_min_us = frame_us;
        if (frame_us > w.frame_max_us) w.frame_max_us = frame_us;
        w.frame_sum_us += frame_us;
    }
    /*
     * 掉帧现场用"最近一帧"（g_cached_fas_frame_us），不是短窗口平均——
     * 单个 40ms 的坑在 144fps 的平均里只有 7.2ms，完全看不出来。
     */
    if (last_frame_us > w.worst_frame_us) {
        w.worst_frame_us = last_frame_us;
        w.worst_frame_action = action;
        w.worst_frame_applied_khz = applied_khz;
        control_trace_capture_freqs(w);
    }
    if (w.debt_min_us == 0 || debt_us < w.debt_min_us) w.debt_min_us = debt_us;
    if (debt_us > w.debt_max_us) w.debt_max_us = debt_us;
    w.debt_sum_us += debt_us;

    int ten_pct = budget_us / 10;
    int thirty_pct = (budget_us * 3) / 10;
    if (debt_us <= 0) w.tier_ok++;
    else if (debt_us <= ten_pct) w.tier_low++;
    else if (debt_us <= thirty_pct) w.tier_mid++;
    else w.tier_high++;

    if (applied_khz > 0) {
        if (w.applied_min_khz == 0 || applied_khz < w.applied_min_khz) w.applied_min_khz = applied_khz;
        if (applied_khz > w.applied_max_khz) w.applied_max_khz = applied_khz;
        if (fmin > 0 && applied_khz <= fmin) w.floor_hits++;
        if (fmax > 0 && applied_khz >= fmax) w.ceil_hits++;
    }
}

static void control_trace_maybe_emit(long long now_ms, const char* app) {
    ControlTraceWindow& w = g_control_trace;
    if (w.start_ms == 0) {
        w.start_ms = now_ms;
        return;
    }
    if (w.samples == 0 || now_ms - w.start_ms < CONTROL_TRACE_WINDOW_MS) return;

    int avg_frame = w.frame_sum_us > 0 ? (int)(w.frame_sum_us / w.samples) : 0;
    int avg_debt = (int)(w.debt_sum_us / w.samples);
    auto pct = [](int part, int total) -> int { return total > 0 ? (part * 100) / total : 0; };

    char log_msg[768];
    safe_snprintf(
        log_msg, sizeof(log_msg),
        "control_trace | app:%s | fas:%d%% | budget:%d | frame(avg/min/max):%d/%d/%d | "
        "debt(avg/min/max):%d/%d/%d | tiers ok:%d%% low:%d%% mid:%d%% high:%d%% | "
        "applied:%d-%d floor:%d%% ceil:%d%% | tfps:%d(%d-%d) flips:%d | "
        "actions ds/sweet/ceil/max:%d/%d/%d/%d maxev:%d | "
        "worst:%dus@act%d band:%d cur:%d/%d ddr:%d | runq:%lldus(tid:%d) | "
        "surface:0x%llx sflips:%d",
        app && app[0] ? app : "unknown",
        pct(w.fas_on_samples, w.samples),
        w.budget_us,
        avg_frame, w.frame_min_us, w.frame_max_us,
        avg_debt, w.debt_min_us, w.debt_max_us,
        pct(w.tier_ok, w.samples), pct(w.tier_low, w.samples),
        pct(w.tier_mid, w.samples), pct(w.tier_high, w.samples),
        w.applied_min_khz, w.applied_max_khz,
        pct(w.floor_hits, w.samples), pct(w.ceil_hits, w.samples),
        w.target_fps_last, w.target_fps_min, w.target_fps_max, w.target_flips,
        w.action_ds, w.action_sweet, w.action_ceil, w.action_max,
        w.ceil_events,
        w.worst_frame_us, w.worst_frame_action, w.worst_frame_applied_khz,
        w.worst_frame_sample_cur[0], w.worst_frame_sample_cur[1],
        control_trace_read_ddr_khz(),
        w.rd_max_ns / 1000, w.rd_tid,
        (unsigned long long)w.surface_key, w.surface_flips);
    log_message(log_msg);

    /*
     * 真掉帧（超预算 1 倍以上）单独记一行，方便和用户导出的 CSV 按时间对齐。
     * 限流：同一秒最多一条，避免一个坑打出几十行。
     */
    static long long last_hitch_log_ms = 0;
    if (w.worst_frame_us >= w.budget_us * 2 && w.budget_us > 0 && now_ms - last_hitch_log_ms >= 1000) {
        last_hitch_log_ms = now_ms;
        char hitch_msg[512];
        safe_snprintf(hitch_msg, sizeof(hitch_msg),
                      "hitch | app:%s | frame:%dus budget:%dus act:%d band:%d cur:%d/%d ddr:%d | "
                      "runq:%lldus(tid:%d) | debt(avg/max):%d/%d tierHigh:%d%%",
                      app && app[0] ? app : "unknown",
                      w.worst_frame_us, w.budget_us, w.worst_frame_action, w.worst_frame_applied_khz,
                      w.worst_frame_sample_cur[0], w.worst_frame_sample_cur[1],
                      control_trace_read_ddr_khz(),
                      w.rd_max_ns / 1000, w.rd_tid,
                      avg_debt, w.debt_max_us, pct(w.tier_high, w.samples));
        log_message(hitch_msg);
    }

    int kept_budget = w.budget_us;
    g_control_trace = {};
    g_control_trace.start_ms = now_ms;
    g_control_trace.budget_us = kept_budget;
}

void handle_dynamic_tuning(const char* current_app) {
    long long now = get_current_time_ms();
    static long long last_t = 0;
    static long long last_detail_log_ms = 0;
    static int last_f[MAX_CLUSTERS] = {0};
    /*
     * 决策是帧事件驱动的：每 30ms 一次机会，有帧信号就走帧债动作表，
     * 没有就走日用负载表；两边的信号都是别人算好的，这里不扫 /proc 做决策。
     */
    long long min_interval_ms = 30;
    if (now - last_t < min_interval_ms) return;
    last_t = now;
    g_runtime_stats.dynamic_tuning_ticks.fetch_add(1);

    /*
     * control_trace：只观测。
     * budget 与 frame 都取引擎发布的值——引擎的动作档位就是按这两个数判的，
     * 观测口径必须和决策口径一致，否则改了算法就看不出来效果。
     */
    int trace_budget_us = g_cached_fas_budget_us.load();
    int trace_frame_us = g_cached_fas_avg_frame_us.load();
    /* 最近一帧（不是短窗口平均）：掉帧现场只有这个口径看得出来 */
    int trace_last_frame_us = g_cached_fas_frame_us.load();
    if (trace_budget_us <= 0) {
        int trace_target_fps = g_cached_fas_target_fps.load();
        if (trace_target_fps <= 0) trace_target_fps = g_fas_config.default_target_fps;
        trace_budget_us = trace_target_fps > 0 ? (int)(1000000 / trace_target_fps) : 0;
    }
    int trace_debt_us = (trace_budget_us > 0 && trace_frame_us > 0)
        ? (trace_frame_us - trace_budget_us) : 0;
    /* 关键线程的运行队列等待：先采样再汇总，样本才落进本窗口 */
    control_trace_sample_run_delay(now);
    control_trace_maybe_emit(now, current_app);

    refresh_scene_policy_state(current_app);
    if (should_pause_our_game_scheduler()) {
        g_cached_fas_guard_signal.store(0); reset_fas_prediction_cache();
        UnifiedScheduler::reset_cluster_freq_overrides();
        if (g_dynamic_tuning_active) {
            g_dynamic_tuning_active = 0;
            if (!is_self_bypass_by_official_tuner_active() &&
                !is_scheduler_skip_process_active()) {
                apply_settings();
            }
        }
        g_cached_fas_guard_signal.store(0); return;
    }
    bool fas_on = is_fas_enabled_for_app(current_app);
    bool dyn_on = is_dynamic_tuning_enabled_for_app(current_app);
    if (!dyn_on && !fas_on) {
        g_cached_fas_guard_signal.store(0); reset_fas_prediction_cache();
        UnifiedScheduler::reset_cluster_freq_overrides();
        if (g_dynamic_tuning_active) { g_dynamic_tuning_active = 0; apply_settings(); }
        return;
    }
    int fas_buf = g_cached_fas_buffer_usable.load();
    int fas_action = g_cached_fas_action_level.load();
    int fas_debt_us = g_cached_fas_debt_us.load();
    int fas_severity_us = g_cached_fas_severity_us.load();
    int fas_streak = g_cached_fas_over_budget_streak.load();
    int fas_current_fps = g_cached_fas_current_fps.load();
    int fas_target_fps = g_cached_fas_target_fps.load();
    bool should_log_detail = (now - last_detail_log_ms >= 1000);
    char detail_msg[1024] = {0};
    size_t detail_pos = 0;
    if (should_log_detail) {
        int written = safe_snprintf(
            detail_msg, sizeof(detail_msg),
            "dyn_detail | app:%s | dyn:%d | fas:%d | fas_buf:%d | fas_action:%d | fas_debt:%dus",
            current_app && current_app[0] ? current_app : "unknown",
            dyn_on ? 1 : 0,
            fas_on ? 1 : 0,
            fas_buf,
            fas_action,
            fas_debt_us);
        if (written > 0) detail_pos = (size_t)written;
    }

    if (!g_dynamic_tuning_active) {
        apply_settings(); g_cached_fas_guard_signal.store(0);
        for (int i = 0; i < cluster_count; i++) {
            int pi = -1, pn = cpu_clusters[i].policies[0];
            for (int k = 0; k < policy_count; k++) { if (cpu_policies[k].policy_num == pn) { pi = k; break; } }
            g_virtual_freq[i] = (pi >= 0) ? cpu_policies[pi].min_freq : 300000;
            last_f[i] = 0;
        }
        g_dynamic_tuning_active = 1;
    }

    /* 日用动态调频要用的 CPU 占用（/proc/stat 一次读） */
    AllCpuStats stats = {};
    bool cpu_stats_ready = read_all_cpu_stats(&stats) && stats.valid;

    for (int i = 0; i < cluster_count; i++) {
        int pi = -1, pn = cpu_clusters[i].policies[0];
        for (int k = 0; k < policy_count; k++) { if (cpu_policies[k].policy_num == pn) { pi = k; break; } }
        if (pi == -1) continue;

        int fmin = cpu_policies[pi].min_freq, fmax = cpu_policies[pi].max_freq;
        /*
         * 游戏里下限用硬件最低档：档位 min_freq（例如 balance 的 c1=1017600）在游戏里
         * 会变成硬地板，实测同帧率下比官方多烧 ~0.6W（官方游戏配置 scaling_min 就是第 0 档）。
         * 日用不受影响，日常保底照旧。
         *
         * 例外：本档位开了 respect_min_freq_in_game 时【保留】档位 min_freq 当硬地板。
         * 高刷（165Hz）双大核场景下地板太低会让 CPU 占用率飙升、帧时间抖动加大，
         * 此时抬高地板反而更稳——是否接受那 ~0.6W 的代价由配置决定，默认仍走省电行为。
         */
        if (is_game_package_name(current_app) && !cpu_policies[pi].respect_min_freq_in_game) {
            int hw_low = get_policy_lowest_available_freq(pi);
            if (hw_low > 0 && hw_low < fmin) fmin = hw_low;
        }
        int mo = UnifiedScheduler::get_cluster_min_override(i), xo = UnifiedScheduler::get_cluster_max_override(i);
        if (mo > fmin) fmin = mo; if (xo > 0 && xo < fmax) fmax = xo;
        if (fmax < fmin) fmax = fmin;
        if (g_scene_policy.screen_off) { fmin = get_policy_lowest_available_freq(pi); fmax = fmin; }
        else { int sf = get_scene_floor_freq_for_cluster(pi, i, current_app); if (sf > fmin) fmin = sf; }
        if (fmax < fmin) fmax = fmin;
        /*
         * 频率边界的归属仲裁（dyn.json 的 corridor_owner）：
         *   dyn  → 用 dyn.json 里这个簇的边界（没配就跟随档位）
         *   mode → 只按档位配置的 min/max 走，运行时不套 dyn.json 的边界
         *   fas  → 同上；FAS 是唯一决策者
         * 边界按簇取，能表达"大核给高上限、小核压更低"这类日常调法。
         */
        const bool corridor_owned_by_dyn = (g_corridor_owner == CORRIDOR_OWNER_DYN);
        if (corridor_owned_by_dyn) {
            if (i >= 0 && i < MAX_CLUSTERS) {
                if (g_dyn_corridor_min_khz[i] > 0 && fmin < g_dyn_corridor_min_khz[i]) {
                    fmin = g_dyn_corridor_min_khz[i];
                }
                if (g_dyn_corridor_max_khz[i] > 0 && fmax > g_dyn_corridor_max_khz[i]) {
                    fmax = g_dyn_corridor_max_khz[i];
                }
            }
        }
        if (fmax < fmin) fmax = fmin;

        /*
         * FAS 路径：动作档位 → 频率区间。
         *
         * 引擎只输出"这一帧欠多少时间"，这里按档位查表。**FAS 只管地板，天花板交还给档位/走廊上限**：
         *   深睡(<=0 债)           → [深睡地板, fmax]
         *   甜点(<=10% 债)         → [甜点,     fmax]
         *   放开(<=30% 债)         → [甜点与上限的中点, fmax]
         *   顶格(>30% 债/连续两帧) → [fmax, fmax]
         *
         * 为什么天花板不再压到甜点（2026-09-22 实测）：
         * 我们的深睡区间原来是 [地板, 甜点]，对 policy6 就是 [883200, 1267200]，
         * 而平台自己要往 1382400 抬 —— 日志里就是
         *   policy6/scaling_max_freq -> 期望 [1267200] 实际 [1382400]
         *   policy6/scaling_min_freq -> 期望 [883200]  实际 [1267200]
         * 也就是说**我们比平台还保守**：CPU7 占用 69%（峰值 90%+）时反而被我们的上限压住爬不上去，
         * 小掉帧变多。地板仍然有用（避免帧来了才从最低频往上爬），天花板压住只会损帧。
         *
         * 也不再把 min 和 max 写成同一个值（旧实现三个档位都是 min=max）：
         * 钉死等于把 governor 关掉，帧一超预算没有余量可借；实测游戏里被钉在 883MHz 时
         * 8 核平均占用 82%、功耗 7.15W，而官方（只写区间、运行时保底给最低档）只有 4.77W。
         * 只有"顶格"保留满频：那是连续严重超预算的兜底，出现时间极短。
         */
        if (fas_on && fas_buf > 0 && fas_action >= FAS_ACTION_DEEP_SLEEP) {
            int sweet = sweetspot_freq_for_policy(pi);
            /* 甜点必须落在本簇可用区间里，否则区间会退化 */
            if (sweet > 0) {
                if (sweet > fmax) sweet = fmax;
                if (sweet < fmin) sweet = fmin;
            }
            int sweet_floor = (sweet > 0 && sweet > fmin) ? sweet : fmin;
            /* 甜点档上限：甜点再往上放一半余量（老行为） */
            int sweet_ceil = sweet_floor + (fmax - sweet_floor) / 2;
            if (sweet_ceil < sweet_floor) sweet_ceil = sweet_floor;
            /*
             * 深睡地板：优先用 fas.json 里显式配的 deep_floor_freq（按档位/簇，kHz），
             * 没配才退回"甜点 × 65%"。
             *
             * 为什么要这个开关：65% 是"甜点比例"，换算出来要向上取到频率表档位，
             * balance 档 0-5 簇算出来是 1008.8 → 取整成 1113.6；
             * 用户实测认为 0-5 的地板应该就是 883.2（= 档位表里的 883200），
             * 这种"具体要多少"的事应该写在配置里，而不是靠比例猜。
             */
            int deep_floor = sweet > 0 ? (int)((long long)sweet * 65 / 100) : 0;
            int configured_floor = deep_floor_freq_for_policy(pi);
            if (configured_floor > 0) deep_floor = configured_floor;
            int fas_low = fmin;
            if (deep_floor > fas_low) fas_low = deep_floor;
            if (fas_low > fmax) fas_low = fmax;
            /* 甜点到上限的中间台阶（"放开档位"用） */
            int fas_mid = sweet_floor + (fmax - sweet_floor) / 2;
            if (fas_mid < sweet_floor) fas_mid = sweet_floor;
            if (fas_mid > fmax) fas_mid = fmax;
            int band_min = fas_low;
            /*
             * 上限按档位给（与 4.65W 那局一致），不做"整局放开"：
             * 放开上限那局（5.45W）帧最稳（min137/jitter0.4%）但功耗高 0.8W；
             * 用户要的是"功耗低于官方"这条基线，所以上限照旧压住。
             */
            int band_max = sweet > 0 ? sweet : fmin;
            switch (fas_action) {
                case FAS_ACTION_SWEETSPOT:
                    band_min = sweet_floor;
                    band_max = sweet_ceil;
                    break;
                case FAS_ACTION_SWEETSPOT_CEILING:
                    band_min = fas_mid;
                    band_max = fmax;
                    break;
                case FAS_ACTION_MAX_FREQ:
                    band_min = fmax;
                    band_max = fmax;
                    g_control_trace.ceil_events++;
                    break;
                default:
                    band_min = fas_low;
                    band_max = sweet > 0 ? sweet : fmin;
                    break;
            }
            apply_render_band(i, pi, band_min, band_max, fmin, fmax, &last_f[i], now, fas_low);
            control_trace_note(trace_budget_us, trace_frame_us, trace_last_frame_us, trace_debt_us,
                               fas_action, last_f[i], fmin, fmax, true);
            if (should_log_detail && detail_pos < sizeof(detail_msg)) {
                int written = safe_snprintf(
                    detail_msg + detail_pos,
                    sizeof(detail_msg) - detail_pos,
                    " | c%d:fas act:%d debt:%dus sev:%dus streak:%d sweet:%d band:%d-%d fps:%d/%d final:%d",
                    i,
                    fas_action,
                    fas_debt_us,
                    fas_severity_us,
                    fas_streak,
                    sweet,
                    band_min,
                    band_max,
                    fas_current_fps,
                    fas_target_fps,
                    last_f[i]);
                if (written > 0 && (size_t)written < sizeof(detail_msg) - detail_pos) {
                    detail_pos += (size_t)written;
                }
            }
            continue;
        }

        /*
         * 日用（没有帧信号）：按簇负载查分段表，输出 [地板, 上限] 这个区间。
         *
         * 上限决定 governor 最高跑到哪（省电/控温），地板决定它不用从最低频慢慢爬（跟手）；
         * 低负载时上限也留余量，突发在区间内由 governor 自己吃，不会"锁死等下一拍"。
         * 分段表、负载口径、迟滞、软化、收紧间隔都来自 dyn.json 的 daily。
         */
        DynDailyParams daily = g_cluster_dyn_daily[i];
        if (!daily.enabled) daily = g_dyn_daily;
        int daily_busy = -1;
        int daily_floor = fmin;
        int daily_target = fmax;
        int daily_stage = -1;
        if (cpu_stats_ready) {
            daily_busy = calc_cluster_load_percent_for_daily(i, stats, daily.load_mode);
        }
        if (daily_busy < 0) daily_busy = 0;

        if (daily.enabled && !daily.stages.empty()) {
            /* 分辨率段表：段点之间线性插值；段点附近用迟滞保持上一段，避免来回跳 */
            int stage_index = (int)daily.stages.size() - 1;
            for (size_t si = 0; si < daily.stages.size(); si++) {
                if (daily_busy <= daily.stages[si].load_upper) {
                    stage_index = (int)si;
                    break;
                }
            }
            {
                static int daily_stage_idx[MAX_CLUSTERS] = {0};
                int prev_stage = daily_stage_idx[i];
                if (prev_stage >= 0 && prev_stage < (int)daily.stages.size() &&
                    daily.hysteresis > 0 && stage_index != prev_stage) {
                    int prev_load = daily.stages[prev_stage].load_upper;
                    if (abs(daily_busy - prev_load) <= daily.hysteresis) {
                        stage_index = prev_stage;
                    }
                }
                daily_stage_idx[i] = stage_index;
            }
            daily_stage = stage_index;

            int span = fmax > fmin ? fmax - fmin : 0;
            int floor_permille = daily.stages[stage_index].floor_permille;
            int ceiling_permille = daily.stages[stage_index].ceiling_permille;
            if (stage_index > 0) {
                const DynDailyStage& lower = daily.stages[stage_index - 1];
                const DynDailyStage& upper = daily.stages[stage_index];
                int band = upper.load_upper - lower.load_upper;
                if (band > 0) {
                    int t = (daily_busy - lower.load_upper) * 1000 / band;
                    if (t < 0) t = 0;
                    if (t > 1000) t = 1000;
                    floor_permille = lower.floor_permille +
                        (upper.floor_permille - lower.floor_permille) * t / 1000;
                    ceiling_permille = lower.ceiling_permille +
                        (upper.ceiling_permille - lower.ceiling_permille) * t / 1000;
                }
            }
            daily_floor = fmin + span * floor_permille / 1000;
            daily_target = fmin + span * ceiling_permille / 1000;
            if (daily.soften_permille < 1000 && daily_target > fmin) {
                daily_target = fmin +
                    (daily_target - fmin) * daily.soften_permille / 1000;
            }
        }
        if (daily_target < daily_floor) daily_target = daily_floor;
        if (daily_target > fmax) daily_target = fmax;
        if (daily_floor > fmax) daily_floor = fmax;
        if (daily_floor < fmin) daily_floor = fmin;

        /* 时间向：放宽（升）立即生效；收紧（降）至少隔 fall_hold_ms，负载一抖不会被立刻压下去 */
        {
            static long long daily_last_fall_ms[MAX_CLUSTERS] = {0};
            static int daily_last_floor[MAX_CLUSTERS] = {0};
            static int daily_last_ceil[MAX_CLUSTERS] = {0};
            int cur_floor = daily_last_floor[i] > 0 ? daily_last_floor[i] : daily_floor;
            int cur_ceil = daily_last_ceil[i] > 0 ? daily_last_ceil[i] : daily_target;
            bool widening = (daily_floor > cur_floor) || (daily_target > cur_ceil);
            bool narrowing = (daily_floor < cur_floor) || (daily_target < cur_ceil);
            if (widening || !narrowing ||
                now - daily_last_fall_ms[i] >= daily.fall_hold_ms) {
                daily_last_floor[i] = daily_floor;
                daily_last_ceil[i] = daily_target;
                if (narrowing) daily_last_fall_ms[i] = now;
            } else {
                daily_floor = cur_floor;
                daily_target = cur_ceil;
            }
        }

        /*
         * 每拍核对并纠正；平台的 boost/thermal 写入先让位 400ms（enforce_band_with_yield）。
         */
        {
            /*
             * 游戏场景不让位（哪怕是走负载表这条路），日用才让位让平台 400ms。
             */
            enforce_band_with_yield(i, pi, daily_floor, daily_target, fmin, fmax, now,
                                    !current_scene_prefers_frames(current_app));
            last_f[i] = daily_target;
            /*
             * 日用诊断：区间写了什么、实际是多少、当前簇负载与命中的段。
             * 限流按簇各一份，否则两个簇抢同一个 5 秒名额，日志里只剩一个簇。
             */
            {
                if (now - g_daily_last_log_ms[i] >= 5000) {
                    g_daily_last_log_ms[i] = now;
                    char path_min[128], path_max[128];
                    safe_snprintf(path_min, sizeof(path_min),
                                  "/sys/devices/system/cpu/cpufreq/policy%d/scaling_min_freq",
                                  cpu_policies[pi].policy_num);
                    safe_snprintf(path_max, sizeof(path_max),
                                  "/sys/devices/system/cpu/cpufreq/policy%d/scaling_max_freq",
                                  cpu_policies[pi].policy_num);
                    char buf_min[32] = {0}, buf_max[32] = {0};
                    read_text_file(path_min, buf_min, sizeof(buf_min));
                    read_text_file(path_max, buf_max, sizeof(buf_max));
                    trim_trailing_whitespace(buf_min);
                    trim_trailing_whitespace(buf_max);
                    char msg[320];
                    safe_snprintf(msg, sizeof(msg),
                                  "daily_write | c%d 负载 %d%% 段%d | 区间 %d-%d | 实际 %s-%s",
                                  i, daily_busy, daily_stage, daily_floor, daily_target,
                                  buf_min, buf_max);
                    log_message(msg);
                }
            }
        }
        g_virtual_freq[i] = daily_floor;
        control_trace_note(trace_budget_us, trace_frame_us, trace_last_frame_us, trace_debt_us,
                           FAS_ACTION_INVALID,
                           daily_target, fmin, fmax, fas_on);
        if (should_log_detail && detail_pos < sizeof(detail_msg)) {
            int written = safe_snprintf(
                detail_msg + detail_pos,
                sizeof(detail_msg) - detail_pos,
                " | c%d:static band:%d-%d",
                i, fmin, fmax);
            if (written > 0 && (size_t)written < sizeof(detail_msg) - detail_pos) {
                detail_pos += (size_t)written;
            }
        }
    }
    if (should_log_detail) {
        last_detail_log_ms = now;
        log_debug_message(detail_msg);
    }
    /* 日用负载基准：记下这一拍的 /proc/stat，供下一拍算增量 */
    if (cpu_stats_ready) {
        update_daily_stats_snapshot(stats);
    }
}
