static void reset_fas_config_defaults() {
    g_fas_config.default_target_fps = 60;
    g_fas_config.base_margin_fps = 1.0f;
    g_fas_config.default_kp = 3.0f;
    g_fas_config.app_count = 0;
    memset(g_fas_config.apps, 0, sizeof(g_fas_config.apps));
}

static float get_default_margin_fps_for_fps(int target_fps) {
    if (target_fps <= 0) target_fps = g_fas_config.default_target_fps > 0 ? g_fas_config.default_target_fps : 60;
    float margin = ((float)target_fps / 60.0f) * g_fas_config.base_margin_fps;
    if (margin < 0.5f) margin = 0.5f;
    return margin;
}

static void collect_fas_packages(cJSON* app, std::vector<std::string>* out_packages) {
    if (!app || !out_packages) return;
    out_packages->clear();

    cJSON* packages_item = cJSON_GetObjectItem(app, "packages");
    if (packages_item && cJSON_IsArray(packages_item)) {
        cJSON* item = NULL;
        cJSON_ArrayForEach(item, packages_item) {
            if (!cJSON_IsString(item) || !item->valuestring) continue;
            std::string package_name = trim_copy(item->valuestring);
            if (package_name.empty()) continue;
            out_packages->push_back(package_name);
        }
    }

    cJSON* package_item = cJSON_GetObjectItem(app, "package");
    if (package_item && cJSON_IsString(package_item) && package_item->valuestring) {
        char* packages = strdup(package_item->valuestring);
        if (!packages) return;

        char* saveptr = NULL;
        char* package = strtok_r(packages, ",", &saveptr);
        while (package) {
            std::string package_name = trim_copy(package);
            if (!package_name.empty()) {
                out_packages->push_back(package_name);
            }
            package = strtok_r(NULL, ",", &saveptr);
        }
        free(packages);
    }

    std::sort(out_packages->begin(), out_packages->end());
    out_packages->erase(std::unique(out_packages->begin(), out_packages->end()), out_packages->end());
}

/*
 * FAS 甜点频率（kHz，按簇索引）。
 *
 * 原先住在 cpu.json 的 policy.optimum_freq 里，但它的语义完全是 FAS 的：
 * 卡顿守卫用它抬目标、passthrough 判据用它判断"频率已经掉到甜点以下"。
 * 实测各档位甜点差别很大（powersave 1.78G / performance 2.7G / fast 3.03G），
 * 所以保留档位维度：
 *
 *   "sweetspot_freq": {
 *     "powersave": { "0": 1785600, "1": 1632000 },
 *     "balance":   { "0": 1552000, "1": 1267200 },
 *     "performance": { "0": 2745600, "1": 2880000 },
 *     "fast":      { "0": 3033600, "1": 4070400 }
 *   }
 *
 * 不带档位的一层（{"0": ...}）作为所有档位的默认。
 */
static int g_sweetspot_freq_khz[MAX_CLUSTERS];
/*
 * 深睡地板（kHz，按簇索引），可选。含义：FAS 判到"这一帧没欠预算"时，
 * scaling_min 至少压到哪个频率。
 *
 * 为什么要单独配：65% 只是"甜点比例"，换算出来还要向上取到频率表档位——
 * balance 档 0-5 簇 1552000×65% = 1008800 → 取整成 1113600，
 * 而用户实测认为 0-5 的地板应该就是 883200（频率表里的 883.2MHz）。
 * 这种"具体要多少"的事放在配置里，比靠比例猜更清楚：
 *
 *   "deep_floor_freq": { "0": 883200 }            // 所有档位都用这个
 *   "deep_floor_freq": { "balance": { "0": 883200 } }   // 只对 balance 档
 *
 * 不配就退回"甜点 × 65%"。
 */
static int g_deep_floor_freq_khz[MAX_CLUSTERS];
/* FAS 默认开关（fas.json defaults.enabled）——原先在 sched.txt 的 fas= 与 cpu.json 的 features 里 */
static bool g_fas_enabled_config = true;

static void load_fas_sweetspot_for_mode(const char* mode) {
    memset(g_sweetspot_freq_khz, 0, sizeof(g_sweetspot_freq_khz));

    char* data = NULL;
    size_t data_len = 0;
    if (!read_file_all(FAS_CONFIG_FILE, &data, &data_len) || !data || data_len == 0) {
        if (data) free(data);
        return;
    }

    cJSON* root = cJSON_Parse(data);
    free(data);
    if (!root) return;

    cJSON* table = cJSON_GetObjectItem(root, "sweetspot_freq");
    bool loaded_any = false;
    if (table && cJSON_IsObject(table)) {
        cJSON* selected = table;
        if (mode && mode[0]) {
            cJSON* per_mode = cJSON_GetObjectItem(table, mode);
            if (per_mode && cJSON_IsObject(per_mode)) {
                selected = per_mode;
            }
        }
        cJSON* item = NULL;
        cJSON_ArrayForEach(item, selected) {
            if (!item->string) continue;
            int idx = atoi(item->string);
            if (idx >= 0 && idx < MAX_CLUSTERS && cJSON_IsNumber(item) && item->valueint > 0) {
                g_sweetspot_freq_khz[idx] = item->valueint;
                loaded_any = true;
            }
        }
    }
    cJSON_Delete(root);

    if (loaded_any) {
        char log_msg[256];
        size_t used = 0;
        for (int i = 0; i < MAX_CLUSTERS; i++) {
            if (g_sweetspot_freq_khz[i] <= 0) continue;
            int written = safe_snprintf(log_msg + used, sizeof(log_msg) - used,
                                        "%scluster%d=%d", used == 0 ? "" : " ",
                                        i, g_sweetspot_freq_khz[i]);
            if (written <= 0 || (size_t)written >= sizeof(log_msg) - used) break;
            used += (size_t)written;
        }
        char full_msg[320];
        safe_snprintf(full_msg, sizeof(full_msg), "已加载 FAS 甜点频率 | mode:%s | %s",
                      mode && mode[0] ? mode : "unknown", log_msg);
        log_message(full_msg);
    } else {
        log_message("fas.json 未配置 sweetspot_freq，卡顿守卫将不使用甜点频率");
    }
}

/* 某个 policy 对应的甜点频率；0 表示没配。 */
static int sweetspot_freq_for_policy(int policy_idx) {
    if (policy_idx < 0 || policy_idx >= policy_count) return 0;
    int cluster = cpu_policies[policy_idx].cluster_id;
    if (cluster < 0 || cluster >= MAX_CLUSTERS) return 0;
    return g_sweetspot_freq_khz[cluster];
}

/* 某个 policy 对应的深睡地板频率；0 表示没配（走甜点比例）。 */
static int deep_floor_freq_for_policy(int policy_idx) {
    if (policy_idx < 0 || policy_idx >= policy_count) return 0;
    int cluster = cpu_policies[policy_idx].cluster_id;
    if (cluster < 0 || cluster >= MAX_CLUSTERS) return 0;
    return g_deep_floor_freq_khz[cluster];
}

static void load_fas_deep_floor_for_mode(const char* mode) {
    memset(g_deep_floor_freq_khz, 0, sizeof(g_deep_floor_freq_khz));

    char* data = NULL;
    size_t data_len = 0;
    if (!read_file_all(FAS_CONFIG_FILE, &data, &data_len) || !data || data_len == 0) {
        if (data) free(data);
        return;
    }

    cJSON* root = cJSON_Parse(data);
    free(data);
    if (!root) return;

    cJSON* table = cJSON_GetObjectItem(root, "deep_floor_freq");
    if (table && cJSON_IsObject(table)) {
        cJSON* selected = table;
        if (mode && mode[0]) {
            cJSON* per_mode = cJSON_GetObjectItem(table, mode);
            if (per_mode && cJSON_IsObject(per_mode)) {
                selected = per_mode;
            }
        }
        cJSON* item = NULL;
        cJSON_ArrayForEach(item, selected) {
            if (!item->string) continue;
            int idx = atoi(item->string);
            if (idx >= 0 && idx < MAX_CLUSTERS && cJSON_IsNumber(item) && item->valueint > 0) {
                g_deep_floor_freq_khz[idx] = item->valueint;
            }
        }
    }
    cJSON_Delete(root);

    char log_msg[256];
    size_t used = 0;
    for (int i = 0; i < MAX_CLUSTERS; i++) {
        if (g_deep_floor_freq_khz[i] <= 0) continue;
        int written = safe_snprintf(log_msg + used, sizeof(log_msg) - used,
                                    "%scluster%d=%d", used == 0 ? "" : " ",
                                    i, g_deep_floor_freq_khz[i]);
        if (written <= 0 || (size_t)written >= sizeof(log_msg) - used) break;
        used += (size_t)written;
    }
    if (used > 0) {
        char full_msg[320];
        safe_snprintf(full_msg, sizeof(full_msg), "已加载 FAS 深睡地板 | mode:%s | %s",
                      mode && mode[0] ? mode : "unknown", log_msg);
        log_message(full_msg);
    }
}

void load_fas_config() {
    /* 甜点频率与 FAS 参数同一份文件，跟着一起刷新 */
    load_fas_sweetspot_for_mode(g_current_mode);
    load_fas_deep_floor_for_mode(g_current_mode);
    reset_fas_config_defaults();

    char* data = NULL;
    size_t data_len = 0;
    if (!read_file_all(FAS_CONFIG_FILE, &data, &data_len) || !data || data_len == 0) {
        log_message("未找到 fas.json，使用默认 FAS 配置");
        if (data) free(data);
        return;
    }

    cJSON* root = cJSON_Parse(data);
    if (!root) {
        log_message("fas.json 解析失败，使用默认 FAS 配置");
        free(data);
        return;
    }

    cJSON* defaults = cJSON_GetObjectItem(root, "defaults");
    if (defaults && cJSON_IsObject(defaults)) {
        cJSON* item = cJSON_GetObjectItem(defaults, "default_target_fps");
        if (item && cJSON_IsNumber(item) && item->valueint > 0) g_fas_config.default_target_fps = item->valueint;

        item = cJSON_GetObjectItem(defaults, "base_margin_fps");
        if (item && cJSON_IsNumber(item) && item->valuedouble > 0.0) g_fas_config.base_margin_fps = (float)item->valuedouble;

        item = cJSON_GetObjectItem(defaults, "kp");
        if (item && cJSON_IsNumber(item) && item->valuedouble > 0.0) g_fas_config.default_kp = (float)item->valuedouble;

        item = cJSON_GetObjectItem(defaults, "enabled");
        if (item && (cJSON_IsBool(item) || cJSON_IsNumber(item))) {
            g_fas_enabled_config = item->valueint != 0;
        }


    }

    bool truncated = false;
    cJSON* apps = cJSON_GetObjectItem(root, "apps");
    if (apps && cJSON_IsArray(apps)) {
        cJSON* app = NULL;
        cJSON_ArrayForEach(app, apps) {
            if (g_fas_config.app_count >= MAX_FAS_APPS) {
                truncated = true;
                break;
            }

            cJSON* friendly_item = cJSON_GetObjectItem(app, "friendly");
            std::vector<std::string> package_list;
            collect_fas_packages(app, &package_list);
            if (package_list.empty()) continue;

            cJSON* target_fps = cJSON_GetObjectItem(app, "target_fps");
            cJSON* margin_fps = cJSON_GetObjectItem(app, "margin_fps");
            cJSON* kp = cJSON_GetObjectItem(app, "kp");
            cJSON* fallback = cJSON_GetObjectItem(app, "fallback");
            cJSON* fps_profiles = cJSON_GetObjectItem(app, "fps_profiles");

            for (size_t package_index = 0;
                 package_index < package_list.size() && g_fas_config.app_count < MAX_FAS_APPS;
                 package_index++) {
                const char* package = package_list[package_index].c_str();
                if (fps_profiles && cJSON_IsArray(fps_profiles)) {
                    cJSON* profile = NULL;
                    cJSON_ArrayForEach(profile, fps_profiles) {
                        if (g_fas_config.app_count >= MAX_FAS_APPS) {
                            truncated = true;
                            break;
                        }

                        FasAppConfig* cfg = &g_fas_config.apps[g_fas_config.app_count];
                        safe_strncpy(cfg->package, package, sizeof(cfg->package));
                        if (friendly_item && cJSON_IsString(friendly_item) && friendly_item->valuestring) {
                            safe_strncpy(cfg->friendly, friendly_item->valuestring, sizeof(cfg->friendly));
                        }

                        cJSON* profile_target_fps = cJSON_GetObjectItem(profile, "target_fps");
                        cJSON* profile_margin_fps = cJSON_GetObjectItem(profile, "margin_fps");
                        cJSON* profile_kp = cJSON_GetObjectItem(profile, "kp");
                        cJSON* profile_fallback = cJSON_GetObjectItem(profile, "fallback");
                        cfg->target_fps = (profile_target_fps && cJSON_IsNumber(profile_target_fps) && profile_target_fps->valueint >= 0)
                            ? profile_target_fps->valueint
                            : g_fas_config.default_target_fps;
                        cfg->margin_fps = (profile_margin_fps && cJSON_IsNumber(profile_margin_fps) && profile_margin_fps->valuedouble > 0.0)
                            ? (float)profile_margin_fps->valuedouble
                            : get_default_margin_fps_for_fps(cfg->target_fps);
                        cfg->kp = (profile_kp && cJSON_IsNumber(profile_kp) && profile_kp->valuedouble > 0.0)
                            ? (float)profile_kp->valuedouble
                            : g_fas_config.default_kp;
                        cfg->fallback = (profile_fallback && (cJSON_IsBool(profile_fallback) || cJSON_IsNumber(profile_fallback)))
                            ? profile_fallback->valueint
                            : 0;
                        g_fas_config.app_count++;
                    }
                } else {
                    FasAppConfig* cfg = &g_fas_config.apps[g_fas_config.app_count];
                    safe_strncpy(cfg->package, package, sizeof(cfg->package));
                    if (friendly_item && cJSON_IsString(friendly_item) && friendly_item->valuestring) {
                        safe_strncpy(cfg->friendly, friendly_item->valuestring, sizeof(cfg->friendly));
                    }
                    cfg->target_fps = (target_fps && cJSON_IsNumber(target_fps) && target_fps->valueint >= 0)
                        ? target_fps->valueint
                        : g_fas_config.default_target_fps;
                    cfg->margin_fps = (margin_fps && cJSON_IsNumber(margin_fps) && margin_fps->valuedouble > 0.0)
                        ? (float)margin_fps->valuedouble
                        : get_default_margin_fps_for_fps(cfg->target_fps);
                    cfg->kp = (kp && cJSON_IsNumber(kp) && kp->valuedouble > 0.0)
                        ? (float)kp->valuedouble
                        : g_fas_config.default_kp;
                    cfg->fallback = (fallback && (cJSON_IsBool(fallback) || cJSON_IsNumber(fallback)))
                        ? fallback->valueint
                        : 0;
                    g_fas_config.app_count++;
                }
            }
        }
    }

    cJSON_Delete(root);
    free(data);

    char log_msg[128];
    safe_snprintf(log_msg, sizeof(log_msg), "加载了 %d 个 FAS 应用配置", g_fas_config.app_count);
    log_message(log_msg);
    if (truncated) {
        log_message("fas.json 应用配置超过容量上限，后续包名已被截断");
    }
}

typedef struct {
    uint64_t ktime_ns;
    uint64_t buffer;
} QueueBufferFrameSignal;

typedef struct {
    uint64_t surface_key;
    uint64_t last_ktime_ns;
    uint64_t last_seen_ktime_ns;
    long long last_wall_ms;
    uint64_t signal_count;
    uint64_t accepted_count;
    uint64_t rejected_count;
    uint64_t last_delta_ns;
    /*
     * interval_ewma_ms：该 surface 的提交节奏（相邻两次 queueBuffer 的间隔 EWMA）。
     * 用来判断"这块 surface 现在是不是在真正出帧"，只做参考量，不参与控制。
     */
    double interval_ewma_ms;
    std::vector<double> frametimes_ms;
} QueueBufferSurfaceState;

static std::mutex g_frame_sample_mutex;
static long long g_queuebuffer_last_wall_ms = 0;
static uint64_t g_queuebuffer_signal_count = 0;
static uint64_t g_queuebuffer_accepted_count = 0;
static uint64_t g_queuebuffer_rejected_count = 0;
static uint64_t g_queuebuffer_last_delta_ns = 0;
static char g_queuebuffer_package[MAX_PACKAGE_LEN] = "";
static pid_t g_queuebuffer_pid = -1;
static std::vector<QueueBufferSurfaceState> g_queuebuffer_surface_states;
/* 当前认定的主 surface，用于粘滞选择，避免在多个 surface 之间来回跳 */
static uint64_t g_queuebuffer_primary_key = 0;

static QueueBufferSurfaceState* find_queuebuffer_surface_state_locked(uint64_t surface_key) {
    for (auto& state : g_queuebuffer_surface_states) {
        if (state.surface_key == surface_key) {
            return &state;
        }
    }
    return nullptr;
}

static QueueBufferSurfaceState* find_or_create_queuebuffer_surface_state_locked(uint64_t surface_key) {
    QueueBufferSurfaceState* existing = find_queuebuffer_surface_state_locked(surface_key);
    if (existing) {
        return existing;
    }
    g_queuebuffer_surface_states.push_back({});
    QueueBufferSurfaceState& created = g_queuebuffer_surface_states.back();
    created.surface_key = surface_key;
    return &created;
}

static void prune_queuebuffer_surface_states_locked(uint64_t latest_ktime_ns) {
    constexpr uint64_t kStaleSurfaceNs = 5000000000ULL;
    constexpr size_t kMaxTrackedSurfaces = 24;

    if (g_queuebuffer_surface_states.size() <= kMaxTrackedSurfaces || latest_ktime_ns == 0) {
        return;
    }

    g_queuebuffer_surface_states.erase(
        std::remove_if(
            g_queuebuffer_surface_states.begin(),
            g_queuebuffer_surface_states.end(),
            [latest_ktime_ns](const QueueBufferSurfaceState& state) {
                return state.last_seen_ktime_ns == 0 ||
                    latest_ktime_ns - state.last_seen_ktime_ns > kStaleSurfaceNs;
            }),
        g_queuebuffer_surface_states.end());

    if (g_queuebuffer_surface_states.size() <= kMaxTrackedSurfaces) {
        return;
    }

    std::sort(
        g_queuebuffer_surface_states.begin(),
        g_queuebuffer_surface_states.end(),
        [](const QueueBufferSurfaceState& lhs, const QueueBufferSurfaceState& rhs) {
            return lhs.last_seen_ktime_ns > rhs.last_seen_ktime_ns;
        });
    g_queuebuffer_surface_states.resize(kMaxTrackedSurfaces);
}

/*
 * 主 surface 选择。
 *
 * 游戏进程里 queueBuffer 的调用方不止一个 surface（渲染主 surface、UI/HUD surface、
 * 视频 overlay 等），帧时间只能在"同一个 surface 的相邻两帧"之间算。以前这里比的是
 * frametimes_ms.size()，而该队列在 240 条就封顶，几个 surface 一起封顶后就会按
 * last_seen 来回跳，导致引擎一会儿吃到 120fps 主 surface，一会儿吃到 60Hz 的旁路
 * surface，目标帧率跟着在 60/144 之间乱跳。
 *
 * 现在的判据：
 *   1. 必须新鲜（最近一次提交在 kPrimaryFreshMs 内），否则这块 surface 没在出帧；
 *   2. 必须已经连续提交过 kPrimaryMinFrames 帧，排除刚建出来的旁路 surface；
 *   3. 优先选累计提交最多（活得最久、一直在出帧）的那块，累计数接近时提交更快的胜出；
 *   4. 粘滞：当前主 surface 仍然新鲜且累计数不落后太多时不切换。
 */
static QueueBufferSurfaceState* select_primary_queuebuffer_surface_locked() {
    constexpr long long kPrimaryFreshMs = 1000;
    constexpr uint64_t kPrimaryMinFrames = 12;
    long long now_ms = get_current_time_ms();

    QueueBufferSurfaceState* best = nullptr;
    for (auto& state : g_queuebuffer_surface_states) {
        if (state.accepted_count < kPrimaryMinFrames || state.frametimes_ms.empty()) {
            continue;
        }
        if (state.last_wall_ms <= 0 || now_ms - state.last_wall_ms > kPrimaryFreshMs) {
            continue;
        }
        if (!best) {
            best = &state;
            continue;
        }
        if (state.accepted_count > best->accepted_count) {
            best = &state;
            continue;
        }
        /* 累计数在同一档（差不超过 25%）时，出帧更快的更可能是渲染主 surface */
        if (state.accepted_count + state.accepted_count / 4 >= best->accepted_count &&
            state.interval_ewma_ms > 0.0 && best->interval_ewma_ms > 0.0 &&
            state.interval_ewma_ms < best->interval_ewma_ms) {
            best = &state;
        }
    }

    if (!best) {
        g_queuebuffer_primary_key = 0;
        return nullptr;
    }

    QueueBufferSurfaceState* sticky = find_queuebuffer_surface_state_locked(g_queuebuffer_primary_key);
    if (sticky && sticky != best &&
        sticky->accepted_count >= kPrimaryMinFrames && !sticky->frametimes_ms.empty() &&
        sticky->last_wall_ms > 0 && now_ms - sticky->last_wall_ms <= kPrimaryFreshMs &&
        sticky->accepted_count + sticky->accepted_count / 4 >= best->accepted_count) {
        return sticky;
    }

    g_queuebuffer_primary_key = best->surface_key;
    return best;
}

static void clear_queuebuffer_frametimes_locked() {
    g_queuebuffer_surface_states.clear();
    g_queuebuffer_primary_key = 0;
    g_queuebuffer_last_wall_ms = 0;
    g_queuebuffer_signal_count = 0;
    g_queuebuffer_accepted_count = 0;
    g_queuebuffer_rejected_count = 0;
    g_queuebuffer_last_delta_ns = 0;
}

static void reset_queuebuffer_frame_cache(pid_t pid, const char* package) {
    std::lock_guard<std::mutex> lock(g_frame_sample_mutex);
    clear_queuebuffer_frametimes_locked();
    g_queuebuffer_pid = pid;
    safe_strncpy(g_queuebuffer_package, package ? package : "", sizeof(g_queuebuffer_package));
}

static int handle_queuebuffer_frame_signal(void* ctx, void* data, size_t size) {
    (void)ctx;
    if (!data || size < sizeof(QueueBufferFrameSignal)) return 0;
    const QueueBufferFrameSignal* signal = reinterpret_cast<const QueueBufferFrameSignal*>(data);
    std::lock_guard<std::mutex> lock(g_frame_sample_mutex);
    if (g_queuebuffer_pid <= 0) return 0;
    g_queuebuffer_signal_count++;

    QueueBufferSurfaceState* state = find_or_create_queuebuffer_surface_state_locked(signal->buffer);
    if (!state) return 0;
    state->signal_count++;

    if (state->last_ktime_ns > 0 && signal->ktime_ns > state->last_ktime_ns) {
        uint64_t frame_ns = signal->ktime_ns - state->last_ktime_ns;
        state->last_delta_ns = frame_ns;
        /*
         * 提交节奏：只统计连续提交（间隔 <= 1s）的间隔，长时间空档后重新起步。
         * 这块 surface 是否在真正出帧，比"曾经攒过多少帧"更能说明问题。
         */
        if (frame_ns > 1000000000ULL) {
            state->interval_ewma_ms = 0.0;
        } else if (frame_ns > 0) {
            double delta_ms = (double)frame_ns / 1000000.0;
            state->interval_ewma_ms = state->interval_ewma_ms > 0.0
                ? state->interval_ewma_ms * 0.7 + delta_ms * 0.3
                : delta_ms;
        }
        if (frame_ns >= 1000000ULL && frame_ns <= 200000000ULL) {
            if (state->frametimes_ms.size() >= 240) {
                state->frametimes_ms.erase(state->frametimes_ms.begin());
            }
            state->frametimes_ms.push_back((double)frame_ns / 1000000.0);
            state->last_wall_ms = get_current_time_ms();
            g_queuebuffer_last_wall_ms = state->last_wall_ms;
            g_queuebuffer_accepted_count++;
            state->accepted_count++;
            g_queuebuffer_last_delta_ns = frame_ns;
        } else {
            g_queuebuffer_rejected_count++;
            state->rejected_count++;
        }
    }
    state->last_ktime_ns = signal->ktime_ns;
    state->last_seen_ktime_ns = signal->ktime_ns;
    prune_queuebuffer_surface_states_locked(signal->ktime_ns);
    return 0;
}

static int collect_queuebuffer_frametimes(const char* package,
                                          double* out_ms,
                                          int max_count,
                                          char* source,
                                          size_t source_size) {
    if (!package || !package[0] || !out_ms || max_count <= 0) return 0;
    std::lock_guard<std::mutex> lock(g_frame_sample_mutex);
    if (strcmp(package, g_queuebuffer_package) != 0) return 0;
    QueueBufferSurfaceState* primary = select_primary_queuebuffer_surface_locked();
    if (!primary || primary->frametimes_ms.empty()) return 0;
    long long now_ms = get_current_time_ms();
    if (primary->last_wall_ms <= 0 || now_ms - primary->last_wall_ms > 2000) {
        return 0;
    }

    int total = (int)primary->frametimes_ms.size();
    int count = total < max_count ? total : max_count;
    int start = total - count;
    for (int i = 0; i < count; i++) {
        out_ms[i] = primary->frametimes_ms[start + i];
    }
    if (source && source_size > 0) {
        safe_snprintf(source, source_size, "queuebuffer_surface_%llx",
                      (unsigned long long)primary->surface_key);
    }
    return count;
}

static int collect_recent_frametimes(const char* package, double* out_ms, int max_count, char* source, size_t source_size) {
    if (!package || !package[0] || !out_ms || max_count <= 0) return 0;
    if (source && source_size > 0) source[0] = '\0';
    return collect_queuebuffer_frametimes(package, out_ms, max_count, source, source_size);
}

static int read_sysfs_int_with_fallbacks_for_fas(const char* const* paths,
                                                 size_t count,
                                                 int default_value,
                                                 char* out_source,
                                                 size_t out_source_size) {
    if (!paths) return default_value;
    if (out_source && out_source_size > 0) out_source[0] = '\0';
    char buf[128];
    for (size_t i = 0; i < count; i++) {
        if (!paths[i] || !paths[i][0]) continue;
        if (!read_text_file(paths[i], buf, sizeof(buf))) continue;
        int value = atoi(buf);
        if (value > 0 || strcmp(buf, "0") == 0) {
            if (out_source && out_source_size > 0) {
                safe_strncpy(out_source, paths[i], out_source_size);
            }
            return value;
        }
    }
    return default_value;
}

static int normalize_gpu_busy_pct_for_fas(int raw_busy) {
    if (raw_busy <= 0) return 0;
    if (raw_busy <= 100) return raw_busy;
    if (raw_busy <= 255) return (raw_busy * 100) / 255;
    if (raw_busy <= 1000) return raw_busy / 10;
    return 100;
}

static int normalize_freq_to_khz_for_fas(int raw_freq) {
    if (raw_freq <= 0) return 0;
    if (raw_freq >= 10000000) {
        return raw_freq / 1000; // Hz -> kHz
    }
    if (raw_freq < 10000) {
        return raw_freq * 1000; // MHz -> kHz
    }
    return raw_freq; // already kHz
}

static int read_gpu_busy_pct_from_sysfs_with_source(char* out_source, size_t out_source_size) {
    static const char* kGpuBusyPaths[] = {
        "/sys/class/kgsl/kgsl-3d0/gpu_busy_percentage",
        "/sys/class/devfreq/gpufreq/load",
        "/sys/class/devfreq/mtk-gpufreq/load",
        "/sys/class/devfreq/1c50000.mali/load",
        "/sys/class/devfreq/1c50000.mali/utilization",
        "/sys/class/devfreq/3d00000.gpufreq/load",
        "/sys/class/devfreq/3d00000.gpufreq/utilization"
    };

    int direct_busy = read_sysfs_int_with_fallbacks_for_fas(
        kGpuBusyPaths, sizeof(kGpuBusyPaths) / sizeof(kGpuBusyPaths[0]), -1,
        out_source, out_source_size);
    if (direct_busy >= 0) {
        return normalize_gpu_busy_pct_for_fas(direct_busy);
    }

    static const char* kKgslBusyPaths[] = {
        "/sys/class/kgsl/kgsl-3d0/gpubusy",
        "/sys/class/kgsl/kgsl-3d0/devfreq/gpubusy"
    };
    char buf[128];
    if (out_source && out_source_size > 0) out_source[0] = '\0';
    for (size_t i = 0; i < sizeof(kKgslBusyPaths) / sizeof(kKgslBusyPaths[0]); i++) {
        if (!read_text_file(kKgslBusyPaths[i], buf, sizeof(buf))) continue;
        unsigned long long busy = 0;
        unsigned long long total = 0;
        if (sscanf(buf, "%llu %llu", &busy, &total) == 2 && total > 0) {
            int pct = (int)((busy * 100ULL) / total);
            if (pct < 0) pct = 0;
            if (pct > 100) pct = 100;
            if (out_source && out_source_size > 0) {
                safe_strncpy(out_source, kKgslBusyPaths[i], out_source_size);
            }
            return pct;
        }
    }
    return 0;
}

static int read_gpu_freq_khz_from_sysfs_with_source(char* out_source, size_t out_source_size) {
    static const char* kGpuFreqPaths[] = {
        "/sys/class/kgsl/kgsl-3d0/cur_freq",
        "/sys/class/kgsl/kgsl-3d0/devfreq/cur_freq",
        "/sys/class/devfreq/gpufreq/cur_freq",
        "/sys/class/devfreq/mtk-gpufreq/cur_freq",
        "/sys/class/devfreq/1c50000.mali/cur_freq",
        "/sys/class/devfreq/3d00000.gpufreq/cur_freq"
    };

    int freq = read_sysfs_int_with_fallbacks_for_fas(
        kGpuFreqPaths, sizeof(kGpuFreqPaths) / sizeof(kGpuFreqPaths[0]), 0,
        out_source, out_source_size);
    return normalize_freq_to_khz_for_fas(freq);
}

static int read_ddr_freq_khz_from_sysfs_with_source(char* out_source, size_t out_source_size) {
    static const char* kDdrFreqPaths[] = {
        "/sys/class/devfreq/soc:qcom,cpubw/cur_freq",
        "/sys/class/devfreq/soc:qcom,gpubw/cur_freq",
        "/sys/class/devfreq/soc:qcom,cpu-cpu-ddr-bw/cur_freq",
        "/sys/class/devfreq/soc:qcom,cpu-llcc-ddr-bw/cur_freq",
        "/sys/class/devfreq/mtk-dvfsrc-devfreq/cur_freq",
        // Legacy Mediatek tools often locate dvfsrc under platform paths first.
        "/sys/devices/platform/soc/1c00f000.dvfsrc/mtk-dvfsrc-devfreq/devfreq/mtk-dvfsrc-devfreq/cur_freq",
        "/sys/devices/platform/1c00f000.dvfsrc/mtk-dvfsrc-devfreq/devfreq/mtk-dvfsrc-devfreq/cur_freq",
        "/sys/devices/platform/1c013000.dvfsrc/mtk-dvfsrc-devfreq/devfreq/mtk-dvfsrc-devfreq/cur_freq",
        "/sys/class/devfreq/ddr/cur_freq",
        "/sys/class/devfreq/dmc/cur_freq"
    };

    int freq = read_sysfs_int_with_fallbacks_for_fas(
        kDdrFreqPaths, sizeof(kDdrFreqPaths) / sizeof(kDdrFreqPaths[0]), 0,
        out_source, out_source_size);
    return normalize_freq_to_khz_for_fas(freq);
}

static void collect_fas_predict_thread_features(const char* package,
                                                FasThreadFeatureSnapshot* out) {
    if (!out) return;
    memset(out, 0, sizeof(*out));
    if (!package || !package[0] || strcmp(package, "unknown") == 0) return;
    UnifiedScheduler::collect_fas_thread_features(package, out);
}

static void collect_fas_predict_gpu_features(FasGpuFeatureSnapshot* out) {
    if (!out) return;
    memset(out, 0, sizeof(*out));

    out->gpu_busy_pct = read_gpu_busy_pct_from_sysfs_with_source(
        out->gpu_busy_source, sizeof(out->gpu_busy_source));
    out->gpu_freq_khz = read_gpu_freq_khz_from_sysfs_with_source(
        out->gpu_freq_source, sizeof(out->gpu_freq_source));
    out->ddr_freq_khz = read_ddr_freq_khz_from_sysfs_with_source(
        out->ddr_freq_source, sizeof(out->ddr_freq_source));
    out->gpu_busy_valid = out->gpu_busy_source[0] ? 1 : 0;
    out->gpu_freq_valid = out->gpu_freq_source[0] ? 1 : 0;
    out->ddr_freq_valid = out->ddr_freq_source[0] ? 1 : 0;

    int gpu_pressure = out->gpu_busy_pct;
    if (out->gpu_freq_khz > 0 && out->gpu_freq_khz < 600000) {
        gpu_pressure += 10;
    }
    if (out->ddr_freq_khz > 0 && out->ddr_freq_khz < 1200000) {
        gpu_pressure += 8;
    }
    if (gpu_pressure > 100) gpu_pressure = 100;
    out->gpu_pressure = gpu_pressure;
}

int get_app_current_fps(const char* package) {
    static char cached_package[MAX_PACKAGE_LEN] = "";
    static long long last_query_time = 0;
    static int cached_fps = 0;

    if (!package || package[0] == '\0' || strcmp(package, "unknown") == 0) {
        return 0;
    }

    long long now = get_current_time_ms();
    if (strcmp(cached_package, package) == 0 && now - last_query_time < 2000) {
        return cached_fps;
    }

    double recent_frame_ms[5] = {0};
    char source[96] = {0};
    int sample_count = collect_recent_frametimes(package, recent_frame_ms, 5, source, sizeof(source));

    if (sample_count <= 0) {
        static long long last_no_signal_log_ms = 0;
        if (now - last_no_signal_log_ms >= 2000) {
            last_no_signal_log_ms = now;
            char msg[192];
            safe_snprintf(msg, sizeof(msg), "FAS帧信号缺失 | app:%s | source:queuebuffer_only", package);
            log_fas_message(msg);
        }
    }

    int detected_fps = 0;
    if (sample_count > 0) {
        double total_ms = 0.0;
        for (int i = 0; i < sample_count; i++) {
            total_ms += recent_frame_ms[i];
        }
        double avg_ms = total_ms / (double)sample_count;
        if (avg_ms > 0.0) {
            detected_fps = (int)(1000.0 / avg_ms + 0.5);
        }
    }

    safe_strncpy(cached_package, package, sizeof(cached_package));
    last_query_time = now;
    cached_fps = detected_fps;

    if (is_aux_log_enabled() && detected_fps > 0) {
        char dbg_msg[256];
        safe_snprintf(dbg_msg, sizeof(dbg_msg),
            "[DEBUG] 实时帧率 | 应用:%s | 当前FPS:%d | source:%s",
            package, detected_fps, source[0] ? source : "unknown");
        log_debug_message(dbg_msg);
    }

    return detected_fps;
}
