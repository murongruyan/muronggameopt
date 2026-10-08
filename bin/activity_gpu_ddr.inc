static std::vector<int> g_gpu_available_freqs;
static std::vector<int> g_gpu_pwrlevel_freqs;
static std::vector<int> g_ddr_available_freqs;
static std::vector<std::string> g_ddr_min_freq_paths;
static std::vector<std::string> g_ddr_max_freq_paths;
static char g_gpu_cur_freq_path[256] = "";
static char g_gpu_min_pwrlevel_path[256] = "";
static char g_gpu_max_pwrlevel_path[256] = "";
static char g_gpu_default_pwrlevel_path[256] = "";
static char g_ddr_cur_freq_path[256] = "";
static char g_ddr_boost_freq_path[256] = "";
static char g_ddr_fixed_opp_path[256] = "";
static int g_gpu_hw_min_freq = 0;
static int g_gpu_hw_max_freq = 0;
static int g_gpu_default_freq = 0;
static int g_gpu_target_min_pwrlevel = -1;
static int g_gpu_target_max_pwrlevel = -1;
static int g_gpu_applied_min_freq = 0;
static int g_gpu_applied_max_freq = 0;
static bool g_gpu_monitor_enabled = false;
static int g_ddr_hw_min_freq = 0;
static int g_ddr_hw_max_freq = 0;
static int g_ddr_applied_min_freq = 0;
static int g_ddr_applied_max_freq = 0;
static bool g_ddr_monitor_enabled = false;

/*
 * 内核/平台自己接管的 GPU/DDR 节点。
 *
 * memlat(bwmon) 与 kgsl dcvs 各管一半：写进去马上被改回去（日志里的
 * "写入未生效(被内核改写)" 就是它们）。写失败本身不可怕，可怕的是**写入那一瞬间是生效的**——
 * 我们每次写都会把平台的 DDR latfloor 提升/GPU 上限短暂清掉，等内核改回来，
 * 正好落在这段时间里的帧就会变成 25~40ms 的深坑。
 * 所以连续写不进去就彻底停手，不再碰这些节点。
 */
static bool g_gpu_pwrlevel_write_disabled = false;
static bool g_ddr_node_write_disabled = false;
static int g_gpu_write_fail_streak = 0;
static int g_ddr_write_fail_streak = 0;
static const int kKernelOwnedWriteStreakLimit = 5;

static bool read_text_file_local(const char* path, char* buf, size_t buf_size);

static int read_int_file_simple(const char* path, int fallback) {
    if (!path || !path[0]) return fallback;
    char buf[128] = {0};
    if (!read_text_file_local(path, buf, sizeof(buf))) {
        return fallback;
    }
    return atoi(buf);
}

static bool load_frequency_list_from_file(const char* path, std::vector<int>& out) {
    out.clear();
    if (!path || !path[0]) return false;
    char* data = NULL;
    size_t len = 0;
    if (!read_file_all(path, &data, &len) || !data) {
        if (data) free(data);
        return false;
    }

    char* saveptr = NULL;
    char* token = strtok_r(data, " \n\r\t", &saveptr);
    while (token) {
        int value = atoi(token);
        if (value > 0) {
            out.push_back(value);
        }
        token = strtok_r(NULL, " \n\r\t", &saveptr);
    }
    free(data);
    return !out.empty();
}

static bool read_text_file_local(const char* path, char* buf, size_t buf_size) {
    if (!path || !buf || buf_size == 0) {
        return false;
    }
    FILE* fp = fopen(path, "r");
    if (!fp) {
        return false;
    }
    size_t n = fread(buf, 1, buf_size - 1, fp);
    fclose(fp);
    if (n == 0) {
        return false;
    }
    buf[n] = '\0';
    return true;
}

static void collect_named_files_recursive(const char* dir_path,
                                          const char* file_name,
                                          int depth,
                                          std::vector<std::string>& out) {
    if (!dir_path || !dir_path[0] || !file_name || !file_name[0] || depth < 0) {
        return;
    }
    DIR* dir = opendir(dir_path);
    if (!dir) return;

    struct dirent* entry = NULL;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        char path[512];
        safe_snprintf(path, sizeof(path), "%s/%s", dir_path, entry->d_name);
        struct stat st;
        if (stat(path, &st) != 0) {
            continue;
        }
        if (S_ISDIR(st.st_mode) && depth > 0) {
            collect_named_files_recursive(path, file_name, depth - 1, out);
            continue;
        }
        if (S_ISREG(st.st_mode) && strcmp(entry->d_name, file_name) == 0) {
            out.push_back(path);
        }
    }

    closedir(dir);
}

static void append_unique_path_if_exists(const char* path, std::vector<std::string>& out) {
    if (!path || !path[0] || !file_exists(path)) {
        return;
    }
    for (size_t i = 0; i < out.size(); i++) {
        if (out[i] == path) {
            return;
        }
    }
    out.push_back(path);
}

static void detect_mtk_ddr_paths() {
    const char* dvfsrc_dirs[] = {
        "/sys/devices/platform/soc/1c00f000.dvfsrc",
        "/sys/devices/platform/1c00f000.dvfsrc",
        "/sys/devices/platform/1c013000.dvfsrc",
        "/sys/class/devfreq/mtk-dvfsrc-devfreq"
    };

    for (size_t i = 0; i < sizeof(dvfsrc_dirs) / sizeof(dvfsrc_dirs[0]); i++) {
        const char* dir = dvfsrc_dirs[i];
        if (!file_exists(dir)) {
            continue;
        }

        char devfreq_base[256] = {0};
        char direct_base[256] = {0};
        safe_snprintf(devfreq_base, sizeof(devfreq_base), "%s/mtk-dvfsrc-devfreq/devfreq/mtk-dvfsrc-devfreq", dir);
        safe_snprintf(direct_base, sizeof(direct_base), "%s", dir);

        const char* bases[] = { devfreq_base, direct_base };
        for (size_t j = 0; j < sizeof(bases) / sizeof(bases[0]); j++) {
            char available_path[256] = {0};
            char min_path[256] = {0};
            char max_path[256] = {0};
            char cur_path[256] = {0};
            safe_snprintf(available_path, sizeof(available_path), "%s/available_frequencies", bases[j]);
            safe_snprintf(min_path, sizeof(min_path), "%s/min_freq", bases[j]);
            safe_snprintf(max_path, sizeof(max_path), "%s/max_freq", bases[j]);
            safe_snprintf(cur_path, sizeof(cur_path), "%s/cur_freq", bases[j]);

            if (g_ddr_available_freqs.empty() && file_exists(available_path)) {
                load_frequency_list_from_file(available_path, g_ddr_available_freqs);
            }
            append_unique_path_if_exists(min_path, g_ddr_min_freq_paths);
            append_unique_path_if_exists(max_path, g_ddr_max_freq_paths);
            if (!g_ddr_cur_freq_path[0] && file_exists(cur_path)) {
                safe_strncpy(g_ddr_cur_freq_path, cur_path, sizeof(g_ddr_cur_freq_path));
            }
        }

        char helper_path[256] = {0};
        const char* dir_name = strrchr(dir, '/');
        dir_name = dir_name ? dir_name + 1 : dir;
        safe_snprintf(helper_path, sizeof(helper_path), "%s/%s:dvfsrc-helper/dvfsrc_force_vcore_dvfs_opp", dir, dir_name);
        if (file_exists(helper_path)) {
            safe_strncpy(g_ddr_fixed_opp_path, helper_path, sizeof(g_ddr_fixed_opp_path));
        }

        if (!g_ddr_min_freq_paths.empty() || !g_ddr_max_freq_paths.empty() || !g_ddr_available_freqs.empty()) {
            break;
        }
    }
}

static int snap_freq_floor(const std::vector<int>& freqs, int requested, int fallback) {
    if (requested <= 0 || freqs.empty()) {
        return fallback;
    }
    for (size_t i = 0; i < freqs.size(); i++) {
        if (freqs[i] >= requested) {
            return freqs[i];
        }
    }
    return freqs.back();
}

static int snap_freq_ceiling(const std::vector<int>& freqs, int requested, int fallback) {
    if (requested <= 0 || freqs.empty()) {
        return fallback;
    }
    for (size_t i = freqs.size(); i-- > 0;) {
        if (freqs[i] <= requested) {
            return freqs[i];
        }
    }
    return freqs.front();
}

static int snap_freq_nearest(const std::vector<int>& freqs, int requested, int fallback) {
    if (requested <= 0 || freqs.empty()) {
        return fallback;
    }
    int best = freqs.front();
    long long best_diff = llabs((long long)best - requested);
    for (size_t i = 1; i < freqs.size(); i++) {
        long long diff = llabs((long long)freqs[i] - requested);
        if (diff < best_diff) {
            best = freqs[i];
            best_diff = diff;
        }
    }
    return best;
}

static int round_double_to_int(double value) {
    return (int)(value >= 0.0 ? value + 0.5 : value - 0.5);
}

static int interpolate_int_range(int min_value, int max_value, double ratio) {
    if (max_value <= min_value) {
        return min_value;
    }
    if (ratio < 0.0) ratio = 0.0;
    if (ratio > 1.0) ratio = 1.0;
    double value = (double)min_value + ((double)(max_value - min_value) * ratio);
    return round_double_to_int(value);
}

static double clamp_ratio01(double ratio) {
    if (ratio < 0.0) return 0.0;
    if (ratio > 1.0) return 1.0;
    return ratio;
}

/*
 * GPU/DDR 的 FAS 联动比例：由动作档位直接给出。
 * 以前是拿"目标频率在频率表里的位置"折算比例，那是被 CPU 频率间接决定的；
 * 现在档位本身就是"这一帧欠多少时间"，GPU/DDR 跟着帧债走更直接。
 *   深睡 0.0 / 甜点 0.35 / 放开档位上限 0.7 / 顶格 1.0
 */
static bool get_fas_control_ratio(double* out_ratio) {
    if (!out_ratio) return false;
    if (g_cached_fas_buffer_usable.load() <= 0) {
        return false;
    }
    int action = g_cached_fas_action_level.load();
    if (action < FAS_ACTION_DEEP_SLEEP) {
        return false;
    }
    if (action > FAS_ACTION_MAX_FREQ) action = FAS_ACTION_MAX_FREQ;

    static const double kActionRatio[4] = {0.0, 0.35, 0.7, 1.0};
    *out_ratio = clamp_ratio01(kActionRatio[action]);
    return true;
}

static void apply_fas_range_window(int base_min,
                                   int base_max,
                                   const std::vector<int>& available_freqs,
                                   bool use_nearest_snap,
                                   int* io_min,
                                   int* io_max) {
    if (!io_min || !io_max || base_max <= base_min) {
        return;
    }

    double fas_ratio = 0.0;
    if (!get_fas_control_ratio(&fas_ratio)) {
        return;
    }

    const bool guard_active = g_cached_fas_guard_active.load() > 0;
    double min_ratio = guard_active
        ? (0.70 + fas_ratio * 0.30)
        : (fas_ratio * 0.55);
    double max_ratio = guard_active
        ? std::max(fas_ratio, 0.92)
        : fas_ratio;

    min_ratio = clamp_ratio01(min_ratio);
    max_ratio = clamp_ratio01(max_ratio);
    if (max_ratio < min_ratio) {
        max_ratio = min_ratio;
    }

    int target_min = interpolate_int_range(base_min, base_max, min_ratio);
    int target_max = interpolate_int_range(base_min, base_max, max_ratio);

    if (!available_freqs.empty()) {
        if (use_nearest_snap) {
            target_min = snap_freq_nearest(available_freqs, target_min, target_min);
            target_max = snap_freq_nearest(available_freqs, target_max, target_max);
        } else {
            target_min = snap_freq_floor(available_freqs, target_min, target_min);
            target_max = snap_freq_ceiling(available_freqs, target_max, target_max);
        }
    }

    if (target_min < base_min) target_min = base_min;
    if (target_max > base_max) target_max = base_max;
    if (target_min > target_max) target_min = target_max;

    *io_min = target_min;
    *io_max = target_max;
}

/*
 * GPU/DDR 的 FAS 联动窗口：**只抬地板，绝不压天花板**。
 *
 * 实测（SM8850 王者荣耀）：原来 GPU/DDR 和 CPU 共用同一套"区间整体缩放"的算法，
 * 于是深睡档（ratio=0）被翻译成 [base_min, base_min]，也就是
 *   DDR 上限压到 547MHz、GPU 上限压到最低 pwrlevel(17)
 * 内核不认这两个值（memlat/bwmon 与 kgsl dcvs 自己管），每 250ms 改回去一次，
 * 日志里反复出现：
 *   /sys/devices/system/cpu/bus_dcvs/DDR/soc:qcom,memlat:ddr:gold/max_freq -> 期望 [547000] 实际 [4780000]
 *   /sys/class/kgsl/kgsl-3d0/max_pwrlevel -> 期望 [17] 实际 [1]
 * 危害不在"写不进去"，而在"写进去的那一瞬间是生效的"：DDR 的 latfloor 提升与 GPU
 * 频率上限会被短暂清掉，内核的工作队列改回来之前的这段时间，落进去的帧就是 25~40ms
 * 的深坑（这也是同一局里掉帧时刻 DDR 正好贴在档位上、而其他统计量都看不出来的原因）。
 *
 * 语义对齐官方：官方对 GPU 只写"地板"（云控 gpuctrl=578000000 是 GPU 下限），
 * 从不压上限。所以这里
 *   ratio 0（深睡/有预算）→ 地板=base_min，天花板=base_max（完全放开，交给平台自己调）
 *   ratio 1（顶格）      → 地板=base_max，天花板=base_max
 *   卡顿守卫             → 地板抬到 70%~100%（守卫本来就是"帧已经不稳了"），天花板照旧放开
 */
static void apply_fas_floor_window(int base_min,
                                   int base_max,
                                   const std::vector<int>& available_freqs,
                                   bool use_nearest_snap,
                                   int* io_min,
                                   int* io_max) {
    if (!io_min || !io_max || base_max <= base_min) {
        return;
    }

    *io_min = base_min;
    *io_max = base_max;

    double fas_ratio = 0.0;
    if (!get_fas_control_ratio(&fas_ratio)) {
        return;
    }

    const bool guard_active = g_cached_fas_guard_active.load() > 0;
    double floor_ratio = guard_active ? (0.70 + fas_ratio * 0.30) : fas_ratio;
    floor_ratio = clamp_ratio01(floor_ratio);

    int target_min = interpolate_int_range(base_min, base_max, floor_ratio);
    if (!available_freqs.empty()) {
        target_min = use_nearest_snap
            ? snap_freq_nearest(available_freqs, target_min, target_min)
            : snap_freq_floor(available_freqs, target_min, target_min);
    }
    if (target_min < base_min) target_min = base_min;
    if (target_min > base_max) target_min = base_max;
    if (target_min > *io_min) *io_min = target_min;
    /* 天花板始终是 base_max：GPU/DDR 上 FAS 只有"抬地板"这一种动作 */
}

static int resolve_special_freq_value(const char* raw, bool is_gpu, int fallback) {
    if (!raw || !raw[0]) return fallback;
    std::string token = trim_copy(scheduler_to_lower_copy(raw));
    if (token.empty()) return fallback;

    char* end = NULL;
    long parsed = strtol(token.c_str(), &end, 10);
    if (end && *end == '\0') {
        return (int)parsed;
    }

    if (is_gpu) {
        if (token == "gpu_min") return g_gpu_hw_min_freq;
        if (token == "gpu_max") return g_gpu_hw_max_freq;
        if (token == "gpu_default") return g_gpu_default_freq > 0 ? g_gpu_default_freq : g_gpu_hw_max_freq;
    } else {
        if (token == "ddr_min") return g_ddr_hw_min_freq;
        if (token == "ddr_max") return g_ddr_hw_max_freq;
    }

    return fallback;
}

static int parse_json_freq_value(cJSON* item, bool is_gpu, int fallback) {
    if (!item) return fallback;
    if (cJSON_IsNumber(item)) {
        return item->valueint;
    }
    if (cJSON_IsString(item) && item->valuestring) {
        return resolve_special_freq_value(item->valuestring, is_gpu, fallback);
    }
    return fallback;
}

static bool evaluate_scaled_freq_expression(const char* raw,
                                            bool is_gpu,
                                            int fallback,
                                            int* out_value,
                                            bool* out_used_expression) {
    if (out_used_expression) *out_used_expression = false;
    if (!raw || !raw[0] || !out_value) {
        return false;
    }

    std::string token = trim_copy(scheduler_to_lower_copy(raw));
    if (token.empty()) {
        return false;
    }

    char* end = NULL;
    long parsed = strtol(token.c_str(), &end, 10);
    if (end && *end == '\0') {
        *out_value = (int)parsed;
        return true;
    }

    size_t mul_pos = token.find('*');
    if (mul_pos != std::string::npos) {
        std::string base_token = trim_copy(token.substr(0, mul_pos));
        std::string factor_token = trim_copy(token.substr(mul_pos + 1));
        if (!base_token.empty() && !factor_token.empty()) {
            int base_value = resolve_special_freq_value(base_token.c_str(), is_gpu, fallback);
            char* factor_end = NULL;
            double factor = strtod(factor_token.c_str(), &factor_end);
            if (factor_end && *factor_end == '\0' && base_value > 0) {
                *out_value = round_double_to_int((double)base_value * factor);
                if (out_used_expression) *out_used_expression = true;
                return true;
            }
        }
    }

    int special_value = resolve_special_freq_value(token.c_str(), is_gpu, fallback);
    if (special_value > 0) {
        *out_value = special_value;
        return true;
    }
    return false;
}

static int clamp_gpu_level(int level) {
    if (g_gpu_pwrlevel_freqs.empty()) return -1;
    if (level < 0) return -1;
    int max_level = (int)g_gpu_pwrlevel_freqs.size() - 1;
    if (level > max_level) return max_level;
    return level;
}

static int gpu_level_to_freq(int level, int fallback) {
    level = clamp_gpu_level(level);
    if (level < 0) return fallback;
    int freq = g_gpu_pwrlevel_freqs[(size_t)level];
    return freq > 0 ? freq : fallback;
}

static int find_gpu_pwrlevel_for_max_freq(int max_freq) {
    if (g_gpu_pwrlevel_freqs.empty() || max_freq <= 0) {
        return -1;
    }
    int best_pl = -1;
    int best_freq = 0;
    for (size_t i = 0; i < g_gpu_pwrlevel_freqs.size(); i++) {
        int freq = g_gpu_pwrlevel_freqs[i];
        if (freq <= 0 || freq > max_freq) continue;
        if (best_pl < 0 || freq > best_freq) {
            best_pl = (int)i;
            best_freq = freq;
        }
    }
    if (best_pl >= 0) return best_pl;
    for (size_t i = 0; i < g_gpu_pwrlevel_freqs.size(); i++) {
        if (g_gpu_pwrlevel_freqs[i] > 0) return (int)i;
    }
    return -1;
}

static int find_gpu_pwrlevel_for_min_freq(int min_freq) {
    if (g_gpu_pwrlevel_freqs.empty() || min_freq <= 0) {
        return -1;
    }
    int best_pl = -1;
    int best_freq = 0;
    for (size_t i = 0; i < g_gpu_pwrlevel_freqs.size(); i++) {
        int freq = g_gpu_pwrlevel_freqs[i];
        if (freq <= 0 || freq < min_freq) continue;
        if (best_pl < 0 || freq < best_freq) {
            best_pl = (int)i;
            best_freq = freq;
        }
    }
    if (best_pl >= 0) return best_pl;
    for (size_t i = 0; i < g_gpu_pwrlevel_freqs.size(); i++) {
        if (g_gpu_pwrlevel_freqs[i] > 0) best_pl = (int)i;
    }
    return best_pl;
}

static void initialize_gpu_ddr_control_state() {
    g_gpu_available_freqs.clear();
    g_gpu_pwrlevel_freqs.clear();
    g_ddr_available_freqs.clear();
    g_ddr_min_freq_paths.clear();
    g_ddr_max_freq_paths.clear();
    g_gpu_cur_freq_path[0] = '\0';
    g_ddr_cur_freq_path[0] = '\0';
    g_ddr_fixed_opp_path[0] = '\0';
    g_gpu_hw_min_freq = 0;
    g_gpu_hw_max_freq = 0;
    g_gpu_default_freq = 0;
    g_gpu_target_min_pwrlevel = -1;
    g_gpu_target_max_pwrlevel = -1;
    g_gpu_applied_min_freq = 0;
    g_gpu_applied_max_freq = 0;
    g_gpu_monitor_enabled = false;
    g_ddr_hw_min_freq = 0;
    g_ddr_hw_max_freq = 0;
    g_ddr_applied_min_freq = 0;
    g_ddr_applied_max_freq = 0;
    g_ddr_monitor_enabled = false;

    const char* gpu_available_candidates[] = {
        "/sys/class/kgsl/kgsl-3d0/gpu_available_frequencies",
        "/sys/class/kgsl/kgsl-3d0/available_frequencies"
    };
    const char* gpu_cur_candidates[] = {
        "/sys/class/kgsl/kgsl-3d0/gpuclk",
        "/sys/class/kgsl/kgsl-3d0/clock_mhz"
    };
    safe_strncpy(g_gpu_min_pwrlevel_path, "/sys/class/kgsl/kgsl-3d0/min_pwrlevel", sizeof(g_gpu_min_pwrlevel_path));
    safe_strncpy(g_gpu_max_pwrlevel_path, "/sys/class/kgsl/kgsl-3d0/max_pwrlevel", sizeof(g_gpu_max_pwrlevel_path));
    safe_strncpy(g_gpu_default_pwrlevel_path, "/sys/class/kgsl/kgsl-3d0/default_pwrlevel", sizeof(g_gpu_default_pwrlevel_path));
    safe_strncpy(g_ddr_cur_freq_path, "/sys/devices/system/cpu/bus_dcvs/DDR/cur_freq", sizeof(g_ddr_cur_freq_path));
    safe_strncpy(g_ddr_boost_freq_path, "/sys/devices/system/cpu/bus_dcvs/DDR/boost_freq", sizeof(g_ddr_boost_freq_path));

    std::vector<int> gpu_raw_freqs;
    for (size_t i = 0; i < sizeof(gpu_available_candidates) / sizeof(gpu_available_candidates[0]); i++) {
        if (load_frequency_list_from_file(gpu_available_candidates[i], gpu_raw_freqs)) {
            break;
        }
    }
    if (!gpu_raw_freqs.empty()) {
        int pwrlevel_count = read_int_file_simple("/sys/class/kgsl/kgsl-3d0/num_pwrlevels", (int)gpu_raw_freqs.size());
        if (pwrlevel_count <= 0 || pwrlevel_count < (int)gpu_raw_freqs.size()) {
            pwrlevel_count = (int)gpu_raw_freqs.size();
        }
        g_gpu_pwrlevel_freqs.assign((size_t)pwrlevel_count, 0);
        bool descending = gpu_raw_freqs.size() < 2 || gpu_raw_freqs[0] > gpu_raw_freqs[1];
        for (size_t i = 0; i < gpu_raw_freqs.size(); i++) {
            size_t pl = descending ? i : (gpu_raw_freqs.size() - 1 - i);
            if (pl < g_gpu_pwrlevel_freqs.size()) {
                g_gpu_pwrlevel_freqs[pl] = gpu_raw_freqs[i];
            }
        }
        g_gpu_available_freqs = gpu_raw_freqs;
        std::sort(g_gpu_available_freqs.begin(), g_gpu_available_freqs.end());
        g_gpu_available_freqs.erase(
            std::unique(g_gpu_available_freqs.begin(), g_gpu_available_freqs.end()),
            g_gpu_available_freqs.end());
        g_gpu_hw_min_freq = g_gpu_available_freqs.front();
        g_gpu_hw_max_freq = g_gpu_available_freqs.back();

        int default_pl = read_int_file_simple(g_gpu_default_pwrlevel_path, -1);
        if (default_pl >= 0 && default_pl < (int)g_gpu_pwrlevel_freqs.size()) {
            g_gpu_default_freq = g_gpu_pwrlevel_freqs[(size_t)default_pl];
        }
    }
    for (size_t i = 0; i < sizeof(gpu_cur_candidates) / sizeof(gpu_cur_candidates[0]); i++) {
        if (file_exists(gpu_cur_candidates[i])) {
            safe_strncpy(g_gpu_cur_freq_path, gpu_cur_candidates[i], sizeof(g_gpu_cur_freq_path));
            break;
        }
    }

    load_frequency_list_from_file("/sys/devices/system/cpu/bus_dcvs/DDR/available_frequencies", g_ddr_available_freqs);
    if (!g_ddr_available_freqs.empty()) {
        std::sort(g_ddr_available_freqs.begin(), g_ddr_available_freqs.end());
        g_ddr_available_freqs.erase(
            std::unique(g_ddr_available_freqs.begin(), g_ddr_available_freqs.end()),
            g_ddr_available_freqs.end());
    }
    g_ddr_hw_min_freq = read_int_file_simple("/sys/devices/system/cpu/bus_dcvs/DDR/hw_min_freq",
                                             g_ddr_available_freqs.empty() ? 0 : g_ddr_available_freqs.front());
    g_ddr_hw_max_freq = read_int_file_simple("/sys/devices/system/cpu/bus_dcvs/DDR/hw_max_freq",
                                             g_ddr_available_freqs.empty() ? 0 : g_ddr_available_freqs.back());
    collect_named_files_recursive("/sys/devices/system/cpu/bus_dcvs/DDR", "min_freq", 2, g_ddr_min_freq_paths);
    collect_named_files_recursive("/sys/devices/system/cpu/bus_dcvs/DDR", "max_freq", 2, g_ddr_max_freq_paths);
    if ((current_platform == PLATFORM_MEDIATEK || (g_ddr_min_freq_paths.empty() && g_ddr_max_freq_paths.empty())) &&
        (g_ddr_available_freqs.empty() || g_ddr_min_freq_paths.empty())) {
        detect_mtk_ddr_paths();
    }
    std::sort(g_ddr_min_freq_paths.begin(), g_ddr_min_freq_paths.end());
    std::sort(g_ddr_max_freq_paths.begin(), g_ddr_max_freq_paths.end());
    if (g_ddr_hw_min_freq <= 0 && !g_ddr_available_freqs.empty()) {
        g_ddr_hw_min_freq = g_ddr_available_freqs.front();
    }
    if (g_ddr_hw_max_freq <= 0 && !g_ddr_available_freqs.empty()) {
        g_ddr_hw_max_freq = g_ddr_available_freqs.back();
    }
}

static void write_int_value_with_perm(const char* path, int value, mode_t pre_perm, mode_t post_perm) {
    if (!path || !path[0]) return;
    char value_str[32];
    safe_snprintf(value_str, sizeof(value_str), "%d", value);
    lock_val_perm(path, value_str, pre_perm, post_perm);
}

static void write_int_value_default(const char* path, int value) {
    if (!path || !path[0]) return;
    char value_str[32];
    safe_snprintf(value_str, sizeof(value_str), "%d", value);
    lock_val(path, value_str);
}

static bool resolve_gpu_frequency_range(int* out_min_freq, int* out_max_freq) {
    if (!out_min_freq || !out_max_freq || g_gpu_hw_min_freq <= 0 || g_gpu_hw_max_freq <= 0) {
        return false;
    }

    int min_freq = gpu.min_freq;
    int max_freq = gpu.max_freq;
    int high_level = clamp_gpu_level(gpu.high_level);
    int low_level = clamp_gpu_level(gpu.low_level);
    if (high_level >= 0) {
        max_freq = gpu_level_to_freq(high_level, max_freq);
    }
    if (low_level >= 0) {
        min_freq = gpu_level_to_freq(low_level, min_freq);
    }
    if (min_freq <= 0) min_freq = g_gpu_hw_min_freq;
    if (max_freq <= 0) max_freq = g_gpu_hw_max_freq;

    min_freq = std::max(min_freq, g_gpu_hw_min_freq);
    max_freq = std::min(max_freq, g_gpu_hw_max_freq);
    if (!g_gpu_available_freqs.empty()) {
        min_freq = snap_freq_floor(g_gpu_available_freqs, min_freq, g_gpu_hw_min_freq);
        max_freq = snap_freq_ceiling(g_gpu_available_freqs, max_freq, g_gpu_hw_max_freq);
    }
    if (min_freq > max_freq) {
        min_freq = max_freq;
    }

    /*
     * 这里**不再套 FAS 的联动窗口**（2026-09-22 修正）。
     *
     * 原先 GPU/DDR 跟着帧债抬地板，实测两个后果：
     *   1) DDR 的 memlat min_freq 是内核 governor 真会照做的下限，被抬上去之后
     *      整局 DDR 钉在最高档（PerfDog 记的 DDR 从 6.4Gbps 变成 9.6Gbps），功耗 +1.7W；
     *   2) GPU 的地板抬到 pwrlevel 0 等于把 GPU 钉在 1200MHz。
     * 官方对自己 GPU 只写一个固定下限（云控 gpuctrl），DDR 由平台自己的 memlat/bwmon 管；
     * 我们的职责是 CPU 侧的帧感知，GPU/DDR 只在**用户在档位 JSON 里显式配了**时才动。
     */

    *out_min_freq = min_freq;
    *out_max_freq = max_freq;
    return true;
}

static bool resolve_ddr_frequency_range(int* out_min_freq, int* out_max_freq) {
    if (!out_min_freq || !out_max_freq || g_ddr_hw_min_freq <= 0 || g_ddr_hw_max_freq <= 0) {
        return false;
    }

    int min_freq = ddr.min_freq;
    int max_freq = ddr.max_freq;
    bool min_from_expr = false;
    bool max_from_expr = false;
    int expr_value = 0;
    if (ddr.min_expr[0] &&
        evaluate_scaled_freq_expression(ddr.min_expr, false, g_ddr_hw_min_freq, &expr_value, &min_from_expr)) {
        min_freq = expr_value;
    }
    expr_value = 0;
    if (ddr.max_expr[0] &&
        evaluate_scaled_freq_expression(ddr.max_expr, false, g_ddr_hw_max_freq, &expr_value, &max_from_expr)) {
        max_freq = expr_value;
    }
    if (min_freq <= 0) min_freq = g_ddr_hw_min_freq;
    if (max_freq <= 0) max_freq = g_ddr_hw_max_freq;

    min_freq = std::max(min_freq, g_ddr_hw_min_freq);
    max_freq = std::min(max_freq, g_ddr_hw_max_freq);
    if (!g_ddr_available_freqs.empty()) {
        min_freq = min_from_expr
            ? snap_freq_nearest(g_ddr_available_freqs, min_freq, g_ddr_hw_min_freq)
            : snap_freq_floor(g_ddr_available_freqs, min_freq, g_ddr_hw_min_freq);
        max_freq = max_from_expr
            ? snap_freq_nearest(g_ddr_available_freqs, max_freq, g_ddr_hw_max_freq)
            : snap_freq_ceiling(g_ddr_available_freqs, max_freq, g_ddr_hw_max_freq);
    }
    if (min_freq > max_freq) {
        min_freq = max_freq;
    }

    /* 同 GPU：DDR 的 FAS 联动已撤掉，只有档位 JSON 显式配了边界才写（见上面那段说明） */

    *out_min_freq = min_freq;
    *out_max_freq = max_freq;
    return true;
}

/*
 * GPU / DDR 的地板策略：**只加地板，且只撤销我们自己加的那一层**。
 *
 * 平台（COSA 云控 / 热控）本来就在这些节点上设下限——官方给王者荣耀写的 gpuctrl=578000000
 * 就是 GPU 下限，memlat 也在给自己设 DDR 下限。旧实现每次都把"当前想要的值"整段写回去，
 * 于是：
 *   - 深睡档（FAS ratio=0）被翻译成上限=下限，等于把平台的 DDR 上限/GPU 上限清掉；
 *   - 取消加速时又把平台设的地板推平。
 * 这里改成：
 *   - 第一次动手前记下节点原值（g_gpu_base_min_pwrlevel / g_ddr_base_min_freq）；
 *   - 只在"我们要的比现在更紧"时才写（GPU 的 pwrlevel 数值越小越高频）；
 *   - 不需要加速了就把原值写回去，不把平台的设置留在被我们改过的状态；
 *   - 天花板除用户显式配置外一概不碰。
 */
static int g_gpu_base_min_pwrlevel = -1;   /* -1 = 还没读到过原值 */
static bool g_gpu_floor_ours = false;
static int g_ddr_base_min_freq = 0;        /* 0 = 还没读到过原值 */
static bool g_ddr_floor_ours = false;
static int g_ddr_last_boost_freq = -1;
/* 我们自己写进去的那一层，只有节点还等于它时才撤回（别把平台同时改的值推平） */
static int g_gpu_ours_level = -1;
static int g_ddr_ours_freq = 0;

static void apply_gpu_floor_level(int want_level) {
    if (!g_gpu_min_pwrlevel_path[0] || want_level < 0) return;
    int current = read_int_file_simple(g_gpu_min_pwrlevel_path, -1);
    if (current < 0) return;
    if (g_gpu_base_min_pwrlevel < 0 && !g_gpu_floor_ours) {
        g_gpu_base_min_pwrlevel = current;   /* 第一次见到的原值 */
    }
    int base = g_gpu_base_min_pwrlevel >= 0 ? g_gpu_base_min_pwrlevel : current;
    int reference = g_gpu_floor_ours ? base : current;

    if (want_level < reference) {
        write_int_value_with_perm(g_gpu_min_pwrlevel_path, want_level, 0644, 0644);
        g_gpu_ours_level = want_level;
        g_gpu_floor_ours = true;
    } else if (g_gpu_floor_ours && want_level >= base) {
        /*
         * 加速结束：只撤回"我们抬上去的那一层"。
         * 只有节点还等于我们写进去的值时才写回原值；如果期间平台自己又改过
         * （例如它在游戏里把地板抬到 1100MHz），就什么都不做，别把平台的设置推平。
         */
        if (g_gpu_ours_level >= 0 && current == g_gpu_ours_level && current != base) {
            write_int_value_with_perm(g_gpu_min_pwrlevel_path, base, 0644, 0644);
        }
        g_gpu_floor_ours = false;
        g_gpu_ours_level = -1;
    }
}

static void apply_ddr_floor_freq(int want_min_freq) {
    if (g_ddr_min_freq_paths.empty() || want_min_freq <= 0) return;
    int current = read_int_file_simple(g_ddr_min_freq_paths.front().c_str(), -1);
    if (current < 0) return;
    if (g_ddr_base_min_freq <= 0 && !g_ddr_floor_ours) {
        g_ddr_base_min_freq = current;
    }
    int base = g_ddr_base_min_freq > 0 ? g_ddr_base_min_freq : current;
    int reference = g_ddr_floor_ours ? base : current;

    if (want_min_freq > reference) {
        for (size_t i = 0; i < g_ddr_min_freq_paths.size(); i++) {
            write_int_value_default(g_ddr_min_freq_paths[i].c_str(), want_min_freq);
        }
        g_ddr_ours_freq = want_min_freq;
        g_ddr_floor_ours = true;
    } else if (g_ddr_floor_ours && want_min_freq <= base) {
        if (g_ddr_ours_freq > 0 && current == g_ddr_ours_freq && current != base) {
            for (size_t i = 0; i < g_ddr_min_freq_paths.size(); i++) {
                write_int_value_default(g_ddr_min_freq_paths[i].c_str(), base);
            }
        }
        g_ddr_floor_ours = false;
        g_ddr_ours_freq = 0;
    }
}

void apply_gpu_settings() {
    int min_freq = 0;
    int max_freq = 0;
    if (!resolve_gpu_frequency_range(&min_freq, &max_freq)) {
        g_gpu_monitor_enabled = false;
        return;
    }

    /*
     * GPU 只写"地板"（min_pwrlevel），不动平台/内核设的上限（max_pwrlevel）。
     *
     * 依据两条：官方云控对自己 GPU 也只写下限（gpuctrl=578000000）；
     * 实测写上限会被 kgsl dcvs 改回去（旧日志：期望 17 实际 1），
     * 而写它的副作用是把平台设的上限清掉——和一个只该加地板的调度器无关。
     * 只有用户在档位 JSON 里显式配了 GPU 上限（high_level / max_freq）时才收紧上限。
     */
    bool manage_ceiling = (clamp_gpu_level(gpu.high_level) >= 0) ||
                          (gpu.max_freq > 0 && gpu.max_freq < g_gpu_hw_max_freq);
    int high_level = manage_ceiling ? clamp_gpu_level(gpu.high_level) : -1;
    int low_level = clamp_gpu_level(gpu.low_level);
    if (manage_ceiling && high_level < 0) {
        high_level = find_gpu_pwrlevel_for_max_freq(max_freq);
    }
    if (low_level < 0) {
        low_level = find_gpu_pwrlevel_for_min_freq(min_freq);
    }
    if (low_level < 0 || (manage_ceiling && (high_level < 0 || high_level > low_level))) {
        g_gpu_monitor_enabled = false;
        return;
    }

    if (!g_gpu_pwrlevel_write_disabled) {
        apply_gpu_floor_level(low_level);
        if (manage_ceiling) {
            write_int_value_with_perm(g_gpu_max_pwrlevel_path, high_level, 0644, 0644);
        }
    }

    g_gpu_target_min_pwrlevel = low_level;
    /* -1 = 上限不归我们管（不写、也不拿它当"我们写过的值"去比对） */
    g_gpu_target_max_pwrlevel = manage_ceiling ? high_level : -1;
    g_gpu_applied_min_freq = min_freq;
    g_gpu_applied_max_freq = manage_ceiling ? max_freq : g_gpu_hw_max_freq;
    double fas_ratio = 0.0;
    bool fas_active = get_fas_control_ratio(&fas_ratio);
    g_gpu_monitor_enabled = fas_active || (min_freq > g_gpu_hw_min_freq || max_freq < g_gpu_hw_max_freq);
}

void apply_ddr_settings() {
    int min_freq = 0;
    int max_freq = 0;
    if (!resolve_ddr_frequency_range(&min_freq, &max_freq)) {
        g_ddr_monitor_enabled = false;
        return;
    }

    if (!g_ddr_node_write_disabled) {
        /* 只加地板，且只撤销我们自己加的那一层；天花板留着让平台自己管 */
        apply_ddr_floor_freq(min_freq);
    }
    /*
     * boost_freq 是"请求一次 DDR 提频"的投票口。这里只在**用户显式配了 DDR 下限**时
     * 才顺手投一票（那是他要求保持的最低带宽），FAS 不再自己往里写——
     * 2026-09-22 实测：FAS 有帧债就往这里写，DDR 被钉在最高档，整局功耗 +1.7W。
     */
    if (g_ddr_boost_freq_path[0] && min_freq > g_ddr_hw_min_freq) {
        int boost_freq = std::min(max_freq, std::max(min_freq, g_ddr_hw_min_freq));
        if (boost_freq != g_ddr_last_boost_freq) {
            write_int_value_default(g_ddr_boost_freq_path, boost_freq);
            g_ddr_last_boost_freq = boost_freq;
        }
    }

    g_ddr_applied_min_freq = min_freq;
    /* 上限我们不写（平台/memlat 自己管），所以不拿它当"我们写过的值" */
    g_ddr_applied_max_freq = g_ddr_hw_max_freq;
    double fas_ratio = 0.0;
    bool fas_active = get_fas_control_ratio(&fas_ratio);
    g_ddr_monitor_enabled = fas_active || (min_freq > g_ddr_hw_min_freq || max_freq < g_ddr_hw_max_freq);
}

static void enforce_gpu_ddr_limits(bool force) {
    if (should_pause_our_game_scheduler()) {
        return;
    }
    static long long last_check_ms = 0;
    long long now_ms = get_current_time_ms();
    if (!force && now_ms - last_check_ms < 250) {
        return;
    }
    last_check_ms = now_ms;

    double gpu_fas_ratio = 0.0;
    bool gpu_fas_active = get_fas_control_ratio(&gpu_fas_ratio);
    if (!g_gpu_pwrlevel_write_disabled && (g_gpu_monitor_enabled || gpu_fas_active)) {
        bool need_reapply = force;
        int desired_gpu_min = 0;
        int desired_gpu_max = 0;
        if (resolve_gpu_frequency_range(&desired_gpu_min, &desired_gpu_max)) {
            if (desired_gpu_min != g_gpu_applied_min_freq || desired_gpu_max != g_gpu_applied_max_freq) {
                need_reapply = true;
            }
        }
        /*
         * 只有"我们抬过地板、而节点比目标还松"才算被改回去，需要再写一次。
         * 节点比我们的目标更紧（平台自己又抬了）是正常情况，不用管。
         */
        if (g_gpu_floor_ours) {
            int current_min_pl = read_int_file_simple(g_gpu_min_pwrlevel_path, -1);
            if (current_min_pl >= 0 && current_min_pl > g_gpu_target_min_pwrlevel) {
                need_reapply = true;
            }
        }
        if (g_gpu_target_max_pwrlevel >= 0) {
            int current_max_pl = read_int_file_simple(g_gpu_max_pwrlevel_path, g_gpu_target_max_pwrlevel);
            if (current_max_pl != g_gpu_target_max_pwrlevel) {
                need_reapply = true;
            }
        }
        int current_gpu_freq = read_int_file_simple(g_gpu_cur_freq_path, 0);
        if (current_gpu_freq > 0 && current_gpu_freq < 10000) {
            current_gpu_freq *= 1000000;
        }
        if (current_gpu_freq > 0 &&
            (current_gpu_freq < g_gpu_applied_min_freq || current_gpu_freq > g_gpu_applied_max_freq)) {
            need_reapply = true;
        }
        if (need_reapply) {
            apply_gpu_settings();
            /*
             * 写完立刻回读：还是被改回去，就说明这个节点归内核/平台管。
             * 连续 kKernelOwnedWriteStreakLimit 次都这样就不再写了 ——
             * 继续写只会在"写入瞬间生效"的窗口里把平台的上限/底噪清掉。
             */
            if (!g_gpu_pwrlevel_write_disabled && g_gpu_floor_ours) {
                int now_min_pl = read_int_file_simple(g_gpu_min_pwrlevel_path, -1);
                bool stuck = (now_min_pl >= 0 && now_min_pl > g_gpu_target_min_pwrlevel);
                int now_max_pl = now_min_pl;
                if (g_gpu_target_max_pwrlevel >= 0) {
                    now_max_pl = read_int_file_simple(g_gpu_max_pwrlevel_path, g_gpu_target_max_pwrlevel);
                    if (now_max_pl != g_gpu_target_max_pwrlevel) stuck = true;
                }
                if (stuck) {
                    if (++g_gpu_write_fail_streak >= kKernelOwnedWriteStreakLimit) {
                        g_gpu_pwrlevel_write_disabled = true;
                        char msg[256];
                        safe_snprintf(msg, sizeof(msg),
                                      "GPU pwrlevel 由内核/平台接管（写入被改写 max:%d->%d min:%d->%d），已停止写入",
                                      g_gpu_target_max_pwrlevel, now_max_pl,
                                      g_gpu_target_min_pwrlevel, now_min_pl);
                        log_message(msg);
                    }
                } else {
                    g_gpu_write_fail_streak = 0;
                }
            }
        }
    }

    double ddr_fas_ratio = 0.0;
    bool ddr_fas_active = get_fas_control_ratio(&ddr_fas_ratio);
    if (!g_ddr_node_write_disabled && (g_ddr_monitor_enabled || ddr_fas_active)) {
        bool need_reapply = force;
        int desired_ddr_min = 0;
        int desired_ddr_max = 0;
        if (resolve_ddr_frequency_range(&desired_ddr_min, &desired_ddr_max)) {
            if (desired_ddr_min != g_ddr_applied_min_freq || desired_ddr_max != g_ddr_applied_max_freq) {
                need_reapply = true;
            }
        }
        /* 同 GPU：只有我们自己抬的地板被推平才重写；天花板不归我们管 */
        if (g_ddr_floor_ours && !g_ddr_min_freq_paths.empty()) {
            int current_ddr_min = read_int_file_simple(g_ddr_min_freq_paths.front().c_str(), -1);
            if (current_ddr_min >= 0 && current_ddr_min < g_ddr_applied_min_freq) {
                need_reapply = true;
            }
        }
        int current_ddr_freq = read_int_file_simple(g_ddr_cur_freq_path, 0);
        if (current_ddr_freq > 0 &&
            (current_ddr_freq < g_ddr_applied_min_freq || current_ddr_freq > g_ddr_applied_max_freq)) {
            need_reapply = true;
        }
        if (need_reapply) {
            apply_ddr_settings();
            /* 同 GPU：连续写不进去说明这些节点（memlat/bwmon）归内核管，停手。 */
            if (!g_ddr_node_write_disabled && g_ddr_floor_ours) {
                bool stuck = false;
                int now_max = g_ddr_applied_max_freq;
                int now_min = g_ddr_applied_min_freq;
                if (!g_ddr_min_freq_paths.empty()) {
                    now_min = read_int_file_simple(g_ddr_min_freq_paths.front().c_str(), -1);
                    if (now_min >= 0 && now_min < g_ddr_applied_min_freq) stuck = true;
                }
                if (stuck) {
                    if (++g_ddr_write_fail_streak >= kKernelOwnedWriteStreakLimit) {
                        g_ddr_node_write_disabled = true;
                        char msg[256];
                        safe_snprintf(msg, sizeof(msg),
                                      "DDR 频率节点由内核 memlat/bwmon 接管（写入被改写 max:%d->%d min:%d->%d），已停止写入",
                                      g_ddr_applied_max_freq, now_max,
                                      g_ddr_applied_min_freq, now_min);
                        log_message(msg);
                    }
                } else {
                    g_ddr_write_fail_streak = 0;
                }
            }
        }
    }
}

void apply_ddr_settings_qualcomm() {
    apply_ddr_settings();
}

void apply_ddr_settings_mediatek() {
    apply_ddr_settings();
}

void try_mediatek_alternative_ddr_paths() {
}
