void load_mode_settings_json(const char* mode) {
    const char* config_path = get_cpu_config_path();
    if (!file_exists(config_path)) {
        char error_msg[256];
        safe_snprintf(error_msg, sizeof(error_msg), "未找到CPU配置文件: %s", config_path);
        log_message(error_msg);
        return;
    }

    FILE* fp = fopen(config_path, "rb");
    if (!fp) {
        char error_msg[256];
        safe_snprintf(error_msg, sizeof(error_msg), "无法打开配置文件: %s (错误: %s)",
            config_path, strerror(errno));
        log_message(error_msg);
        return;
    }

    // Get file size
    fseek(fp, 0, SEEK_END);
    long len = ftell(fp);
    if (len <= 0) {
        log_message("配置文件为空或读取失败");
        cleanup_resources(fp, NULL, NULL);
        return;
    }
    fseek(fp, 0, SEEK_SET);

    // Allocate memory
    char* data = (char*)malloc(len + 1);
    if (!data) {
        log_message("内存分配失败");
        cleanup_resources(fp, NULL, NULL);
        return;
    }

    // Read data
    size_t bytes_read = fread(data, 1, len, fp);
    data[bytes_read] = 0;
    fclose(fp);
    fp = NULL; // Mark as closed
    // Parse JSON
    cJSON* root = cJSON_Parse(data);
    if (!root) {
        const char* error_ptr = cJSON_GetErrorPtr();
        if (error_ptr) {
            char error_msg[256];
            safe_snprintf(error_msg, sizeof(error_msg), "JSON解析失败: %s", error_ptr);
            log_message(error_msg);
        }
        cleanup_resources(NULL, data, NULL);
        return;
    }

    // 1. Parse mode config
    cJSON* mode_obj = cJSON_GetObjectItem(root, mode);
    if (!mode_obj) {
        char error_msg[256];
        safe_snprintf(error_msg, sizeof(error_msg), "在JSON中未找到模式: %s", mode);
        log_message(error_msg);
        log_message("回退到均衡模式");
        mode_obj = cJSON_GetObjectItem(root, "balance");
        if (!mode_obj) {
            cleanup_resources(NULL, data, root);
            log_message("均衡模式也不存在");
            return;
        }
    }

    // 免费版只保留规则线程与基础策略，不启用动态线程学习器。
    g_dyn_params.enabled = 0;
    g_dyn_params.target_load = 85;
    g_dyn_params.load_margin = 5;
    g_dyn_params.debug_log = 0;
    g_dyn_params.low_load_lock_threshold = 0;
    g_dyn_params.load_ceiling_stages.clear();
    g_mode_runtime_features.scheduler_master_enabled = true;
    g_mode_runtime_features.bpf_thread_enabled = true;
    g_mode_runtime_features.scene_category_enabled = true;
    g_mode_runtime_features.base_profile_enabled = true;
    gpu.min_freq = 0;
    gpu.max_freq = 0;
    gpu.high_level = -1;
    gpu.low_level = -1;
    ddr.min_freq = 0;
    ddr.max_freq = 0;
    ddr.min_expr[0] = '\0';
    ddr.max_expr[0] = '\0';

    cJSON* features = cJSON_GetObjectItem(mode_obj, "features");
    if (features && cJSON_IsObject(features)) {
        cJSON* scheduler_master = cJSON_GetObjectItem(features, "scheduler_master");
        if (scheduler_master &&
            (cJSON_IsBool(scheduler_master) || cJSON_IsNumber(scheduler_master))) {
            g_mode_runtime_features.scheduler_master_enabled = scheduler_master->valueint != 0;
        }
        cJSON* bpf_thread = cJSON_GetObjectItem(features, "bpf_thread");
        if (bpf_thread &&
            (cJSON_IsBool(bpf_thread) || cJSON_IsNumber(bpf_thread))) {
            g_mode_runtime_features.bpf_thread_enabled = bpf_thread->valueint != 0;
        }
        cJSON* scene_category = cJSON_GetObjectItem(features, "scene_category");
        if (scene_category &&
            (cJSON_IsBool(scene_category) || cJSON_IsNumber(scene_category))) {
            g_mode_runtime_features.scene_category_enabled = scene_category->valueint != 0;
        }
        cJSON* base_profile = cJSON_GetObjectItem(features, "base_profile");
        if (base_profile &&
            (cJSON_IsBool(base_profile) || cJSON_IsNumber(base_profile))) {
            g_mode_runtime_features.base_profile_enabled = base_profile->valueint != 0;
        }
    }

    for (int i = 0; i < cluster_count; i++) {
        g_cluster_dyn_params[i] = g_dyn_params;
    }

    g_extra_write_count = 0;

    // Parse CPU policies
    cJSON* policies = cJSON_GetObjectItem(mode_obj, "cpu_policies");
    if (policies && cJSON_IsArray(policies)) {
        int policy_index = 0;
        cJSON* policy;
        cJSON_ArrayForEach(policy, policies) {
            if (policy_index >= policy_count) break;

            // Get cluster ID
            cJSON* cluster_id = cJSON_GetObjectItem(policy, "cluster");
            if (cluster_id && cJSON_IsNumber(cluster_id)) {
                int cluster = cluster_id->valueint;
                if (cluster >= 0 && cluster < cluster_count) {
                    // Apply cluster settings to all policies
                    for (int j = 0; j < cpu_clusters[cluster].policy_count; j++) {
                        int policy_num = cpu_clusters[cluster].policies[j];

                        // Find corresponding policy struct
                        for (int k = 0; k < policy_count; k++) {
                            if (cpu_policies[k].policy_num == policy_num) {
                                CpuPolicy* p = &cpu_policies[k];

                                // Set governor
                                cJSON* governor = cJSON_GetObjectItem(policy, "governor");
                                if (governor && cJSON_IsString(governor)) {
                                    safe_strncpy(p->governor, governor->valuestring, sizeof(p->governor));
                                }

                                // Set frequencies
                                cJSON* max_freq = cJSON_GetObjectItem(policy, "max_freq");
                                if (max_freq && cJSON_IsNumber(max_freq)) {
                                    p->max_freq = max_freq->valueint;
                                }

                                cJSON* min_freq = cJSON_GetObjectItem(policy, "min_freq");
                                if (min_freq && cJSON_IsNumber(min_freq)) {
                                    p->min_freq = min_freq->valueint;
                                }

                                // Set governor params
                                cJSON* params = cJSON_GetObjectItem(policy, "params");
                                if (params && cJSON_IsObject(params)) {
                                    p->param_count = 0;
                                    cJSON* param = NULL;
                                    cJSON_ArrayForEach(param, params) {
                                        if (p->param_count >= MAX_GOV_PARAMS) break;

                                        if (cJSON_IsString(param)) {
                                            safe_strncpy(p->param_names[p->param_count], param->string, 32);
                                            safe_strncpy(p->param_values[p->param_count], param->valuestring, 128);
                                            p->param_count++;
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }

            policy_index++;
        }
    }

    // Parse sched boost settings
    cJSON* sched_boost_obj = cJSON_GetObjectItem(mode_obj, "sched_boost");
    if (sched_boost_obj && cJSON_IsObject(sched_boost_obj)) {
        cJSON* top_boost = cJSON_GetObjectItem(sched_boost_obj, "top_boost");
        if (top_boost && cJSON_IsNumber(top_boost)) {
            sched_boost_settings.top_boost = top_boost->valueint;
        } else {
            sched_boost_settings.top_boost = -1;
        }

        cJSON* boost = cJSON_GetObjectItem(sched_boost_obj, "boost");
        if (boost && cJSON_IsNumber(boost)) {
            sched_boost_settings.sched_boost = boost->valueint;
        } else {
            sched_boost_settings.sched_boost = -1;
        }
    } else {
        sched_boost_settings.top_boost = -1;
        sched_boost_settings.sched_boost = -1;
    }

    // Parse scheduler config
    cJSON* sched_config_obj = cJSON_GetObjectItem(mode_obj, "sched_config");
    if (sched_config_obj && cJSON_IsObject(sched_config_obj)) {
        cJSON* downmigrate = cJSON_GetObjectItem(sched_config_obj, "downmigrate");
        if (downmigrate && cJSON_IsNumber(downmigrate)) {
            sched_config_settings.downmigrate = downmigrate->valueint;
        } else {
            sched_config_settings.downmigrate = -1;
        }

        cJSON* upmigrate = cJSON_GetObjectItem(sched_config_obj, "upmigrate");
        if (upmigrate && cJSON_IsNumber(upmigrate)) {
            sched_config_settings.upmigrate = upmigrate->valueint;
        } else {
            sched_config_settings.upmigrate = -1;
        }

        cJSON* group_downmigrate = cJSON_GetObjectItem(sched_config_obj, "group_downmigrate");
        if (group_downmigrate && cJSON_IsNumber(group_downmigrate)) {
            sched_config_settings.group_downmigrate = group_downmigrate->valueint;
        } else {
            sched_config_settings.group_downmigrate = -1;
        }

        cJSON* group_upmigrate = cJSON_GetObjectItem(sched_config_obj, "group_upmigrate");
        if (group_upmigrate && cJSON_IsNumber(group_upmigrate)) {
            sched_config_settings.group_upmigrate = group_upmigrate->valueint;
        } else {
            sched_config_settings.group_upmigrate = -1;
        }
    } else {
        sched_config_settings.downmigrate = -1;
        sched_config_settings.upmigrate = -1;
        sched_config_settings.group_downmigrate = -1;
        sched_config_settings.group_upmigrate = -1;
    }

    // Parse Stune config
    cJSON* stune_obj = cJSON_GetObjectItem(mode_obj, "stune_topapp");
    if (stune_obj && cJSON_IsObject(stune_obj)) {
        cJSON* prefer_idle = cJSON_GetObjectItem(stune_obj, "prefer_idle");
        if (prefer_idle && cJSON_IsNumber(prefer_idle)) {
            stune_settings.prefer_idle = prefer_idle->valueint;
        } else {
            stune_settings.prefer_idle = -1;
        }

        cJSON* boost = cJSON_GetObjectItem(stune_obj, "boost");
        if (boost && cJSON_IsNumber(boost)) {
            stune_settings.boost = boost->valueint;
        } else {
            stune_settings.boost = -1;
        }
    } else {
        stune_settings.prefer_idle = -1;
        stune_settings.boost = -1;
    }

    cJSON* extra_writes = cJSON_GetObjectItem(mode_obj, "extra_writes");
    if (extra_writes && cJSON_IsArray(extra_writes)) {
        cJSON* item = NULL;
        cJSON_ArrayForEach(item, extra_writes) {
            if (g_extra_write_count >= MAX_EXTRA_WRITES) break;
            if (!item || !cJSON_IsObject(item)) continue;
            cJSON* p = cJSON_GetObjectItem(item, "path");
            cJSON* v = cJSON_GetObjectItem(item, "value");
            if (!p || !cJSON_IsString(p) || !p->valuestring) continue;

            safe_strncpy(g_extra_writes[g_extra_write_count].path, p->valuestring,
                sizeof(g_extra_writes[g_extra_write_count].path));

            if (v && cJSON_IsString(v) && v->valuestring) {
                safe_strncpy(g_extra_writes[g_extra_write_count].value, v->valuestring,
                    sizeof(g_extra_writes[g_extra_write_count].value));
            } else if (v && cJSON_IsNumber(v)) {
                safe_snprintf(g_extra_writes[g_extra_write_count].value,
                    sizeof(g_extra_writes[g_extra_write_count].value), "%d", v->valueint);
            } else if (v && cJSON_IsBool(v)) {
                safe_strncpy(g_extra_writes[g_extra_write_count].value,
                    cJSON_IsTrue(v) ? "1" : "0", sizeof(g_extra_writes[g_extra_write_count].value));
            } else {
                g_extra_writes[g_extra_write_count].value[0] = 0;
            }

            g_extra_write_count++;
        }
    }

    // Parse cpuset settings
    cJSON* cpuset_obj = cJSON_GetObjectItem(mode_obj, "cpuset");
    if (cpuset_obj) {
        cJSON* background = cJSON_GetObjectItem(cpuset_obj, "background");
        if (background && cJSON_IsString(background)) {
            safe_strncpy(cpuset.background, background->valuestring, sizeof(cpuset.background));
        }

        cJSON* systembackground = cJSON_GetObjectItem(cpuset_obj, "systembackground");
        if (systembackground && cJSON_IsString(systembackground)) {
            safe_strncpy(cpuset.systembackground, systembackground->valuestring,
                sizeof(cpuset.systembackground));
        }

        cJSON* foreground = cJSON_GetObjectItem(cpuset_obj, "foreground");
        if (foreground && cJSON_IsString(foreground)) {
            safe_strncpy(cpuset.foreground, foreground->valuestring, sizeof(cpuset.foreground));
        }

        cJSON* topapp = cJSON_GetObjectItem(cpuset_obj, "topapp");
        if (topapp && cJSON_IsString(topapp)) {
            safe_strncpy(cpuset.topapp, topapp->valuestring, sizeof(cpuset.topapp));
        }
    }

    cJSON* gpu_obj = cJSON_GetObjectItem(mode_obj, "gpu");
    if (gpu_obj && cJSON_IsObject(gpu_obj)) {
        gpu.min_freq = parse_json_freq_value(cJSON_GetObjectItem(gpu_obj, "min_freq"), true, 0);
        gpu.max_freq = parse_json_freq_value(cJSON_GetObjectItem(gpu_obj, "max_freq"), true, 0);
        cJSON* high_level = cJSON_GetObjectItem(gpu_obj, "high_level");
        if (!high_level) high_level = cJSON_GetObjectItem(gpu_obj, "max_pwrlevel");
        if (high_level && cJSON_IsNumber(high_level)) {
            gpu.high_level = high_level->valueint;
        }
        cJSON* low_level = cJSON_GetObjectItem(gpu_obj, "low_level");
        if (!low_level) low_level = cJSON_GetObjectItem(gpu_obj, "min_pwrlevel");
        if (low_level && cJSON_IsNumber(low_level)) {
            gpu.low_level = low_level->valueint;
        }
    }

    cJSON* ddr_obj = cJSON_GetObjectItem(mode_obj, "ddr");
    if (ddr_obj && cJSON_IsObject(ddr_obj)) {
        cJSON* ddr_min_item = cJSON_GetObjectItem(ddr_obj, "min_freq");
        if (ddr_min_item && cJSON_IsString(ddr_min_item) && ddr_min_item->valuestring) {
            safe_strncpy(ddr.min_expr, ddr_min_item->valuestring, sizeof(ddr.min_expr));
        } else {
            ddr.min_freq = parse_json_freq_value(ddr_min_item, false, 0);
        }
        cJSON* ddr_max_item = cJSON_GetObjectItem(ddr_obj, "max_freq");
        if (ddr_max_item && cJSON_IsString(ddr_max_item) && ddr_max_item->valuestring) {
            safe_strncpy(ddr.max_expr, ddr_max_item->valuestring, sizeof(ddr.max_expr));
        } else {
            ddr.max_freq = parse_json_freq_value(ddr_max_item, false, 0);
        }
    }

    // Parse cpuctl settings
    cJSON* cpuctl_obj = cJSON_GetObjectItem(mode_obj, "cpuctl");
    if (cpuctl_obj) {
        const char* groups[] = { "background", "systembackground", "foreground", "topapp" };
        CpuCtlGroup* group_ptrs[] = {
            &cpuctl.background,
            &cpuctl.systembackground,
            &cpuctl.foreground,
            &cpuctl.topapp
        };

        for (int i = 0; i < 4; i++) {
            cJSON* group_obj = cJSON_GetObjectItem(cpuctl_obj, groups[i]);
            if (group_obj) {
                // uclamp_min (0-100)
                cJSON* uclamp_min = cJSON_GetObjectItem(group_obj, "uclamp_min");
                if (uclamp_min && cJSON_IsNumber(uclamp_min)) {
                    int val = uclamp_min->valueint;
                    // Ensure value is between 0-100
                    if (val < 0) val = 0;
                    if (val > 100) val = 100;
                    group_ptrs[i]->uclamp_min = val;
                }
                else {
                    group_ptrs[i]->uclamp_min = -1; // Mark as unset
                }

                // uclamp_max (0-100)
                cJSON* uclamp_max = cJSON_GetObjectItem(group_obj, "uclamp_max");
                if (uclamp_max && cJSON_IsNumber(uclamp_max)) {
                    int val = uclamp_max->valueint;
                    if (val < 0) val = 0;
                    if (val > 100) val = 100;
                    group_ptrs[i]->uclamp_max = val;
                }
                else {
                    group_ptrs[i]->uclamp_max = -1;
                }

                // latency_sensitive (0/1)
                cJSON* latency_sensitive = cJSON_GetObjectItem(group_obj, "latency_sensitive");
                if (latency_sensitive) {
                    if (cJSON_IsNumber(latency_sensitive)) {
                        group_ptrs[i]->latency_sensitive = latency_sensitive->valueint ? 1 : 0;
                    }
                    else if (cJSON_IsBool(latency_sensitive)) {
                        group_ptrs[i]->latency_sensitive = cJSON_IsTrue(latency_sensitive) ? 1 : 0;
                    }
                    else {
                        group_ptrs[i]->latency_sensitive = -1;
                    }
                }
                else {
                    group_ptrs[i]->latency_sensitive = -1;
                }

                // shares (2-1024)
                cJSON* shares = cJSON_GetObjectItem(group_obj, "shares");
                if (shares && cJSON_IsNumber(shares)) {
                    int val = shares->valueint;
                    if (val < 2) val = 2;
                    if (val > 1024) val = 1024;
                    group_ptrs[i]->shares = val;
                }
                else {
                    group_ptrs[i]->shares = -1;
                }
            }
            else {
                // Group not configured, mark all as unset
                group_ptrs[i]->uclamp_min = -1;
                group_ptrs[i]->uclamp_max = -1;
                group_ptrs[i]->latency_sensitive = -1;
                group_ptrs[i]->shares = -1;
            }
        }
    }

    // Dynamic boost switch has been replaced by g_dyn_params, kept for legacy JSON compatibility
    cJSON* dynamic_boost = cJSON_GetObjectItem(mode_obj, "dynamic_boost");
    if (dynamic_boost && cJSON_IsTrue(dynamic_boost)) {
        g_dyn_params.enabled = 1;
    } else if (dynamic_boost && cJSON_IsFalse(dynamic_boost)) {
        g_dyn_params.enabled = 0;
    }

    if (g_dyn_params.debug_log) {
        char log_msg[256];
        safe_snprintf(log_msg, sizeof(log_msg), "模式 %s 的线程调度配置已启用", mode);
        log_debug_message(log_msg);
        safe_snprintf(log_msg, sizeof(log_msg),
                      "模式 %s 的功能开关 | master:%d | bpf:%d | scene:%d | base:%d",
                      mode,
                      g_mode_runtime_features.scheduler_master_enabled ? 1 : 0,
                      g_mode_runtime_features.bpf_thread_enabled ? 1 : 0,
                      g_mode_runtime_features.scene_category_enabled ? 1 : 0,
                      g_mode_runtime_features.base_profile_enabled ? 1 : 0);
        log_debug_message(log_msg);
    }

    // Unified resource cleanup
    cleanup_resources(NULL, data, root);
}

static bool event_name_matches_config_path(const char* event_name, const char* config_path) {
    if (!event_name || !event_name[0] || !config_path || !config_path[0]) {
        return false;
    }
    const char* base = strrchr(config_path, '/');
    base = base ? (base + 1) : config_path;
    return strcmp(event_name, base) == 0;
}

struct ConfigReloadWatchState {
    bool dirty;
    long long dirty_since_ms;
    long long cooldown_until_ms;
    uint64_t last_hash;
    bool hash_ready;
};

static const long long kConfigReloadDebounceMs = 1500;
static const long long kConfigReloadCooldownMs = 3000;

static ConfigReloadWatchState g_mode_reload_watch = {};
static ConfigReloadWatchState g_scene_categories_reload_watch = {};
static ConfigReloadWatchState g_thread_signature_reload_watch = {};
static ConfigReloadWatchState g_thread_persistent_reload_watch = {};
static ConfigReloadWatchState g_cpu_reload_watch = {};

static bool compute_file_content_hash64(const char* path, uint64_t* out_hash) {
    if (!path || !path[0] || !out_hash || !file_exists(path)) {
        return false;
    }

    char* data = NULL;
    size_t len = 0;
    if (!read_file_all(path, &data, &len) || !data) {
        if (data) free(data);
        return false;
    }

    uint64_t hash = 1469598103934665603ULL;
    for (size_t i = 0; i < len; i++) {
        hash ^= (unsigned char)data[i];
        hash *= 1099511628211ULL;
    }
    free(data);
    *out_hash = hash;
    return true;
}

static void prime_config_reload_watch_state(ConfigReloadWatchState* state, const char* path) {
    if (!state) return;
    state->dirty = false;
    state->dirty_since_ms = 0;
    state->cooldown_until_ms = 0;

    uint64_t hash = 0;
    if (compute_file_content_hash64(path, &hash)) {
        state->last_hash = hash;
        state->hash_ready = true;
    } else {
        state->last_hash = 0;
        state->hash_ready = false;
    }
}

static void mark_config_reload_pending(ConfigReloadWatchState* state) {
    if (!state) return;
    long long now_ms = get_current_time_ms();
    state->dirty = true;
    state->dirty_since_ms = now_ms;
}

static bool should_reload_pending_config(ConfigReloadWatchState* state, const char* path) {
    if (!state || !state->dirty || !path || !path[0]) {
        return false;
    }

    long long now_ms = get_current_time_ms();
    if (state->dirty_since_ms <= 0) {
        state->dirty_since_ms = now_ms;
        return false;
    }
    if (now_ms < state->cooldown_until_ms) {
        return false;
    }
    if (now_ms - state->dirty_since_ms < kConfigReloadDebounceMs) {
        return false;
    }

    uint64_t hash = 0;
    if (!compute_file_content_hash64(path, &hash)) {
        return false;
    }

    state->dirty = false;
    state->dirty_since_ms = 0;
    if (state->hash_ready && state->last_hash == hash) {
        return false;
    }

    state->last_hash = hash;
    state->hash_ready = true;
    state->cooldown_until_ms = now_ms + kConfigReloadCooldownMs;
    return true;
}

void prime_config_reload_watch_hashes() {
    prime_config_reload_watch_state(&g_mode_reload_watch, MODE_FILE);
    prime_config_reload_watch_state(&g_scene_categories_reload_watch, SCENE_CATEGORIES_FILE);
    prime_config_reload_watch_state(&g_thread_signature_reload_watch, get_thread_signature_config_path());
    char persistent_path[256];
    build_persistent_thread_override_path(persistent_path, sizeof(persistent_path));
    prime_config_reload_watch_state(&g_thread_persistent_reload_watch, persistent_path);
    prime_config_reload_watch_state(&g_cpu_reload_watch, get_cpu_config_path());
}

void process_pending_config_reloads() {
    if (should_reload_pending_config(&g_mode_reload_watch, MODE_FILE)) {
        log_message("模式文件内容已变更，正在热重载");
        char mode[64] = "balance";
        FILE* mode_file = fopen(MODE_FILE, "r");
        if (mode_file) {
            char line[256];
            if (fgets(line, sizeof(line), mode_file)) {
                line[strcspn(line, "\n")] = 0;
                safe_strncpy(mode, line, sizeof(mode));
            }
            fclose(mode_file);
        }
        safe_strncpy(g_current_mode, mode, sizeof(g_current_mode));
        load_mode_settings_json(mode);
        UnifiedScheduler::initialize_cluster_policies();
    }

    if (should_reload_pending_config(&g_scene_categories_reload_watch, SCENE_CATEGORIES_FILE)) {
        log_message("场景分类配置内容已变更，正在热重载");
        load_scene_categories_config();
    }

    const char* thread_config_path = get_thread_signature_config_path();
    if (should_reload_pending_config(&g_thread_signature_reload_watch, thread_config_path)) {
        log_message("线程专项配置内容已变更，正在热重载");
        load_thread_signature_config();
        UnifiedScheduler::reload_thread_config_now();
    }

    char persistent_path[256];
    build_persistent_thread_override_path(persistent_path, sizeof(persistent_path));
    if (should_reload_pending_config(&g_thread_persistent_reload_watch, persistent_path)) {
        log_message("设备持久规则覆盖内容已变更，正在热重载");
        load_thread_signature_config();
        UnifiedScheduler::reload_thread_config_now();
    }

    const char* cpu_config_path = get_cpu_config_path();
    if (should_reload_pending_config(&g_cpu_reload_watch, cpu_config_path)) {
        log_message("CPU 配置内容已变更，正在热重载");
        load_mode_settings_json(g_current_mode);
        UnifiedScheduler::initialize_cluster_policies();
    }
}

// Handle inotify events
void handle_inotify_events(int inotify_fd, int wd_general_config_dir, int wd_cpu_config_dir) {
    char buffer[INOTIFY_BUF_LEN];
    ssize_t length = read(inotify_fd, buffer, INOTIFY_BUF_LEN);
    if (length < 0) {
        return; // No events
    }

    for (char* ptr = buffer; ptr < buffer + length; ) {
        struct inotify_event* event = (struct inotify_event*)ptr;
        ptr += INOTIFY_EVENT_SIZE + event->len;

        // Ignore temporary files
        if (event->len > 0 && strstr(event->name, ".tmp") != NULL) {
            continue;
        }

        if (event->mask & (IN_MODIFY | IN_CLOSE_WRITE | IN_MOVED_TO | IN_CREATE)) {
            if (event->wd == wd_general_config_dir && event->len > 0) {
                if (strcmp(event->name, "mode.txt") == 0) {
                    mark_config_reload_pending(&g_mode_reload_watch);
                } else if (strcmp(event->name, "categories.json") == 0) {
                    mark_config_reload_pending(&g_scene_categories_reload_watch);
                }
            } else if (event->wd == wd_cpu_config_dir && event->len > 0) {
                if (event_name_matches_config_path(event->name, get_thread_signature_config_path())) {
                    mark_config_reload_pending(&g_thread_signature_reload_watch);
                } else {
                    char persistent_path[256];
                    build_persistent_thread_override_path(persistent_path, sizeof(persistent_path));
                    if (event_name_matches_config_path(event->name, persistent_path)) {
                        mark_config_reload_pending(&g_thread_persistent_reload_watch);
                    }
                }
                if (event_name_matches_config_path(event->name, get_cpu_config_path())) {
                    mark_config_reload_pending(&g_cpu_reload_watch);
                }
            }
        }
    }
}
