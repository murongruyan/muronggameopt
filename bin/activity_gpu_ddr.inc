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

    *out_min_freq = min_freq;
    *out_max_freq = max_freq;
    return true;
}

void apply_gpu_settings() {
    int min_freq = 0;
    int max_freq = 0;
    if (!resolve_gpu_frequency_range(&min_freq, &max_freq)) {
        g_gpu_monitor_enabled = false;
        return;
    }

    int high_level = clamp_gpu_level(gpu.high_level);
    int low_level = clamp_gpu_level(gpu.low_level);
    if (high_level < 0) {
        high_level = find_gpu_pwrlevel_for_max_freq(max_freq);
    }
    if (low_level < 0) {
        low_level = find_gpu_pwrlevel_for_min_freq(min_freq);
    }
    if (high_level < 0 || low_level < 0 || high_level > low_level) {
        g_gpu_monitor_enabled = false;
        return;
    }

    write_int_value_with_perm(g_gpu_max_pwrlevel_path, high_level, 0644, 0644);
    write_int_value_with_perm(g_gpu_min_pwrlevel_path, low_level, 0644, 0644);

    g_gpu_target_max_pwrlevel = high_level;
    g_gpu_target_min_pwrlevel = low_level;
    g_gpu_applied_min_freq = min_freq;
    g_gpu_applied_max_freq = max_freq;
    g_gpu_monitor_enabled = (min_freq > g_gpu_hw_min_freq || max_freq < g_gpu_hw_max_freq);
}

void apply_ddr_settings() {
    int min_freq = 0;
    int max_freq = 0;
    if (!resolve_ddr_frequency_range(&min_freq, &max_freq)) {
        g_ddr_monitor_enabled = false;
        return;
    }

    for (size_t i = 0; i < g_ddr_max_freq_paths.size(); i++) {
        write_int_value_default(g_ddr_max_freq_paths[i].c_str(), max_freq);
    }
    for (size_t i = 0; i < g_ddr_min_freq_paths.size(); i++) {
        write_int_value_default(g_ddr_min_freq_paths[i].c_str(), min_freq);
    }
    if (g_ddr_boost_freq_path[0]) {
        int boost_freq = std::min(max_freq, std::max(min_freq, g_ddr_hw_min_freq));
        write_int_value_default(g_ddr_boost_freq_path, boost_freq);
    }

    g_ddr_applied_min_freq = min_freq;
    g_ddr_applied_max_freq = max_freq;
    g_ddr_monitor_enabled = (min_freq > g_ddr_hw_min_freq || max_freq < g_ddr_hw_max_freq);
}

static void enforce_gpu_ddr_limits(bool force) {
    static long long last_check_ms = 0;
    long long now_ms = get_current_time_ms();
    if (!force && now_ms - last_check_ms < 250) {
        return;
    }
    last_check_ms = now_ms;

    if (g_gpu_monitor_enabled) {
        bool need_reapply = force;
        int current_min_pl = read_int_file_simple(g_gpu_min_pwrlevel_path, g_gpu_target_min_pwrlevel);
        int current_max_pl = read_int_file_simple(g_gpu_max_pwrlevel_path, g_gpu_target_max_pwrlevel);
        if (current_min_pl != g_gpu_target_min_pwrlevel || current_max_pl != g_gpu_target_max_pwrlevel) {
            need_reapply = true;
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
        }
    }

    if (g_ddr_monitor_enabled) {
        bool need_reapply = force;
        if (!g_ddr_min_freq_paths.empty()) {
            int current_ddr_min = read_int_file_simple(g_ddr_min_freq_paths.front().c_str(), g_ddr_applied_min_freq);
            if (current_ddr_min != g_ddr_applied_min_freq) {
                need_reapply = true;
            }
        }
        if (!g_ddr_max_freq_paths.empty()) {
            int current_ddr_max = read_int_file_simple(g_ddr_max_freq_paths.front().c_str(), g_ddr_applied_max_freq);
            if (current_ddr_max != g_ddr_applied_max_freq) {
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
