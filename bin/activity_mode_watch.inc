static int read_policy_freq_default(int policy_num, const char* node_name, int fallback) {
    if (policy_num < 0 || !node_name || !node_name[0]) {
        return fallback;
    }
    char path[256];
    safe_snprintf(path, sizeof(path),
                  "/sys/devices/system/cpu/cpufreq/policy%d/%s",
                  policy_num,
                  node_name);
    char buf[64] = {0};
    if (!read_text_file(path, buf, sizeof(buf))) {
        return fallback;
    }
    int value = atoi(buf);
    return value > 0 ? value : fallback;
}

static void reset_cpuctl_group(CpuCtlGroup* group) {
    if (!group) return;
    group->uclamp_min = -1;
    group->uclamp_max = -1;
    group->latency_sensitive = -1;
    group->shares = -1;
}

static void reset_mode_runtime_state() {
    for (int i = 0; i < policy_count; i++) {
        int policy_num = cpu_policies[i].policy_num;
        int cluster_id = cpu_policies[i].cluster_id;
        int hw_min_freq = cpu_policies[i].hw_min_freq;
        int min_freq_fallback = cpu_policies[i].min_freq > 0 ? cpu_policies[i].min_freq : hw_min_freq;
        int max_freq_fallback = cpu_policies[i].max_freq > 0 ? cpu_policies[i].max_freq : min_freq_fallback;
        /* memset 会清掉配置态字段，这里先存后恢复（否则每次重载模式开关都会被重置）。 */
        int respect_min_freq_in_game = cpu_policies[i].respect_min_freq_in_game;

        memset(&cpu_policies[i], 0, sizeof(cpu_policies[i]));
        cpu_policies[i].policy_num = policy_num;
        cpu_policies[i].cluster_id = cluster_id;
        cpu_policies[i].hw_min_freq = hw_min_freq;
        cpu_policies[i].respect_min_freq_in_game = respect_min_freq_in_game;
        cpu_policies[i].min_freq = read_policy_freq_default(policy_num, "scaling_min_freq", min_freq_fallback);
        cpu_policies[i].max_freq = read_policy_freq_default(policy_num, "scaling_max_freq", max_freq_fallback);
        if (cpu_policies[i].max_freq > 0 && cpu_policies[i].min_freq > cpu_policies[i].max_freq) {
            cpu_policies[i].min_freq = cpu_policies[i].max_freq;
        }
        if (cpu_policies[i].min_freq <= 0 && hw_min_freq > 0) {
            cpu_policies[i].min_freq = hw_min_freq;
        }
        if (cpu_policies[i].max_freq <= 0) {
            cpu_policies[i].max_freq = cpu_policies[i].min_freq;
        }
    }

    memset(&cpuset, 0, sizeof(cpuset));
    reset_cpuctl_group(&cpuctl.background);
    reset_cpuctl_group(&cpuctl.systembackground);
    reset_cpuctl_group(&cpuctl.foreground);
    reset_cpuctl_group(&cpuctl.topapp);
    gpu.min_freq = 0;
    gpu.max_freq = 0;
    gpu.high_level = -1;
    gpu.low_level = -1;
    ddr.min_freq = 0;
    ddr.max_freq = 0;
    ddr.min_expr[0] = '\0';
    ddr.max_expr[0] = '\0';

    sched_boost_settings.top_boost = -1;
    sched_boost_settings.sched_boost = -1;
    sched_config_settings.downmigrate = -1;
    sched_config_settings.upmigrate = -1;
    sched_config_settings.group_downmigrate = -1;
    sched_config_settings.group_upmigrate = -1;
    stune_settings.prefer_idle = -1;
    stune_settings.boost = -1;
    g_extra_write_count = 0;
}

/*
 * sched.txt 的布尔值解析（task-41）：
 *   只接受 on / 1 / true / yes / t（大小写不敏感），其余任何值一律为 false。
 * 旧实现只看首字符，导致 "off" 因为以 o 开头被当成 true；现在按 token 完整比较。
 */
static bool runtime_feature_value_is_true(const char* start, const char* end) {
    while (start < end && (*start == ' ' || *start == '\t')) start++;
    while (end > start && (end[-1] == ' ' || end[-1] == '\t')) end--;
    const size_t len = (size_t)(end - start);
    if (len == 0) return false;
    if (len == 1) {
        const char c = (char)tolower((unsigned char)start[0]);
        /* 任务约定：只接受 on / 1 / true / yes / t；单个 y 不在白名单内。 */
        return c == '1' || c == 't';
    }
    if (len == 2) {
        return strncasecmp(start, "on", 2) == 0;
    }
    if (len == 3) {
        return strncasecmp(start, "yes", 3) == 0;
    }
    if (len == 4 && strncasecmp(start, "true", 4) == 0) return true;
    return false;
}

static bool parse_runtime_feature_flag_from_text(const char* text, const char* key, bool* out_value) {
    if (!text || !key || !key[0] || !out_value) return false;
    size_t key_len = strlen(key);
    const char* cursor = text;
    while (*cursor) {
        while (*cursor == '\r' || *cursor == '\n') {
            cursor++;
        }
        if (!*cursor) break;
        const char* line_end = cursor;
        while (*line_end && *line_end != '\r' && *line_end != '\n') {
            line_end++;
        }
        while (cursor < line_end && (*cursor == ' ' || *cursor == '\t')) {
            cursor++;
        }
        if ((size_t)(line_end - cursor) > key_len &&
            strncmp(cursor, key, key_len) == 0) {
            const char* separator = cursor + key_len;
            while (separator < line_end && (*separator == ' ' || *separator == '\t')) {
                separator++;
            }
            if (separator < line_end && (*separator == '=' || *separator == ':')) {
                const char* value = separator + 1;
                *out_value = runtime_feature_value_is_true(value, line_end);
                return true;
            }
        }
        cursor = line_end;
    }
    return false;
}

static bool parse_runtime_feature_flag_alias_from_text(
    const char* text,
    const char* short_key,
    const char* long_key,
    bool* out_value
) {
    return parse_runtime_feature_flag_from_text(text, short_key, out_value) ||
           parse_runtime_feature_flag_from_text(text, long_key, out_value);
}

static bool apply_runtime_feature_overrides_from_shared_config(const char* mode) {
    (void)mode;
    /*
     * task-41：外部开关（dyn.json 的 enabled / fas.json 的 defaults.enabled）不依赖
     * config/sched.txt，必须先无条件同步。旧实现把这两行放在「sched.txt 存在」分支之后，
     * sched.txt 缺失（新装模块未打开 App / 用户删除）时 FAS 会停在默认 true，
     * 用户在 fas.json 里关闭 FAS 不生效（runtime_loop #4）。
     */
    g_mode_runtime_features.dynamic_tuning_enabled = g_dyn_params.enabled;
    g_mode_runtime_features.fas_enabled = g_fas_enabled_config;
    invalidate_runtime_feature_switch_cache();

    const char* config_path = get_runtime_feature_config_path();
    if (!file_exists(config_path)) {
        return false;
    }

    char* data = NULL;
    size_t len = 0;
    if (!read_file_all(config_path, &data, &len) || !data || len == 0) {
        if (data) free(data);
        return false;
    }
    /*
     * sched.txt 承载全局"总闸"开关：
     *   main / scheduler_master            调度总控
     *   cth  / custom_thread               自定义线程
     *   dth  / dynamic_thread_scheduler    动态线程（历史别名 bpf / bpf_thread）
     *   cat  / scene_category              场景分类
     *   base / base_profile                模式基础
     * 动态调频 / FAS 仍读各自配置（dyn.json 的 enabled / fas.json 的 defaults.enabled）。
     * 每应用与场景分类规则只能"收窄"，不能把这里关掉的功能重新打开
     * （见 resolve_runtime_feature_switches_for_app 的 clamp）。
     * 缺省（sched.txt 没有对应行）时保持上一轮的值，首次加载默认全开。
     */
    bool value = false;
    if (parse_runtime_feature_flag_alias_from_text(data, "main", "scheduler_master", &value)) {
        g_mode_runtime_features.scheduler_master_enabled = value;
    }
    if (parse_runtime_feature_flag_alias_from_text(data, "cth", "custom_thread", &value)) {
        g_mode_runtime_features.custom_thread_enabled = value;
    }
    if (parse_runtime_feature_flag_alias_from_text(data, "dth", "dynamic_thread_scheduler", &value) ||
        parse_runtime_feature_flag_alias_from_text(data, "bpf", "bpf_thread", &value)) {
        g_mode_runtime_features.dynamic_thread_scheduler_enabled = value;
    }
    if (parse_runtime_feature_flag_alias_from_text(data, "cat", "scene_category", &value)) {
        g_mode_runtime_features.scene_category_enabled = value;
    }
    if (parse_runtime_feature_flag_alias_from_text(data, "base", "base_profile", &value)) {
        g_mode_runtime_features.base_profile_enabled = value;
    }
    invalidate_runtime_feature_switch_cache();
    free(data);
    return true;
}

void load_mode_settings_json(const char* mode) {
    /*
     * task-41：fas.json 的 defaults.enabled 不依赖 cpu.json / sched.txt。
     * 在最早的返回点之前同步一次，保证「sched.txt 缺失」「cpu.json 暂缺」等
     * 提前返回路径上，用户关闭 FAS 也立即生效。
     */
    g_mode_runtime_features.fas_enabled = g_fas_enabled_config;

    const char* config_path = get_cpu_config_path();
    if (!file_exists(config_path)) {
        return;
    }

    FILE* fp = fopen(config_path, "rb");
    if (!fp) {
        char error_msg[256];
        safe_snprintf(error_msg, sizeof(error_msg), "无法打开配置文件: %s (错误: %s)",
            config_path, strerror(errno));
        log_config_message(error_msg);
        return;
    }

    // Get file size
    fseek(fp, 0, SEEK_END);
    long len = ftell(fp);
    if (len <= 0) {
        log_config_message("配置文件为空或读取失败");
        cleanup_resources(fp, NULL, NULL);
        return;
    }
    fseek(fp, 0, SEEK_SET);

    // Allocate memory
    char* data = (char*)malloc(len + 1);
    if (!data) {
        log_config_message("内存分配失败");
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
            log_config_message(error_msg);
        }
        cleanup_resources(NULL, data, NULL);
        return;
    }

    // 1. Parse mode config
    cJSON* mode_obj = cJSON_GetObjectItem(root, mode);
    if (!mode_obj) {
        char error_msg[256];
        safe_snprintf(error_msg, sizeof(error_msg), "在JSON中未找到模式: %s", mode);
        log_config_message(error_msg);
        log_config_message("回退到均衡模式");
        mode_obj = cJSON_GetObjectItem(root, "balance");
        if (!mode_obj) {
            cleanup_resources(NULL, data, root);
            log_config_message("均衡模式也不存在");
            return;
        }
    }

    // Reset dynamic tuning params to defaults
    g_mode_runtime_features.scheduler_master_enabled = true;
    g_mode_runtime_features.dynamic_tuning_enabled = true;
    g_mode_runtime_features.fas_enabled = true;
    g_mode_runtime_features.custom_thread_enabled = true;
    g_mode_runtime_features.dynamic_thread_scheduler_enabled = true;
    g_mode_runtime_features.scene_category_enabled = true;
    g_mode_runtime_features.base_profile_enabled = true;
    g_scheduler_session_config.learning_duration_ms = 120 * 1000;
    g_scheduler_session_config.guard_interval_ms = 1000;
    reset_mode_runtime_state();

    /*
     * 动态调频参数来自独立的 bin/cpu/dyn.json（不再放在 cpu.json 里）。
     * 这里重新载入，保证改配置后热重载也能生效。
     */
    load_dyn_config_json(mode);
    g_mode_runtime_features.dynamic_tuning_enabled = g_dyn_params.enabled;
    /*
     * 甜点频率与深睡地板都按档位不同，切档位必须一起重解析。
     * 历史 bug：这里只重载了甜点，深睡地板要等 fas.json 变更才刷新，
     * 导致切到 performance 后地板仍是上一个档位的值（例如 883200）。
     */
    load_fas_sweetspot_for_mode(mode);
    load_fas_deep_floor_for_mode(mode);

    /* cpu.json 里的 features 开关块已废弃：开关只认总开关与各功能自己的配置。 */

    apply_runtime_feature_overrides_from_shared_config(mode);

    load_scheduler_session_config_json(mode);
    g_scheduler_session_config_base = g_scheduler_session_config;
    /* 开关/规则/档位都变了，每应用的生效开关缓存要作废 */
    invalidate_runtime_feature_switch_cache();

    /* 每簇参数已由 load_dyn_config_json() 填好（全局 + clusters 覆盖） */

    /*
     * 模式层默认：respect_min_freq_in_game 写在本模式下时，套用到该模式的所有 policy。
     * policy 对象里各自再写一次可以覆盖（见下面 policy 层解析）。
     */
    {
        cJSON* mode_respect = cJSON_GetObjectItem(mode_obj, "respect_min_freq_in_game");
        if (mode_respect && (cJSON_IsBool(mode_respect) || cJSON_IsNumber(mode_respect))) {
            int on = cJSON_IsTrue(mode_respect) || mode_respect->valueint != 0;
            for (int k = 0; k < policy_count; k++) {
                cpu_policies[k].respect_min_freq_in_game = on;
            }
        }
    }

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

                                /*
                                 * respect_min_freq_in_game：
                                 *   1/true = 游戏里把本档位 min_freq 当硬地板（高刷场景想要高地板时用）
                                 *   0/false/不写 = 沿用历史省电行为（游戏里下限退到硬件最低档）
                                 */
                                cJSON* respect = cJSON_GetObjectItem(policy, "respect_min_freq_in_game");
                                if (respect && (cJSON_IsBool(respect) || cJSON_IsNumber(respect))) {
                                    p->respect_min_freq_in_game = cJSON_IsTrue(respect) || respect->valueint != 0;
                                }

                                /* optimum_freq 已搬到 fas.json（FAS 甜点频率），这里不再读取 */

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

    /*
     * 功能开关汇总：写主日志（档位加载时才发生，不会刷屏）。
     *
     * 之前这里用 log_debug_message 连打两行，而 log_debug_message 有 1 秒全局节流，
     * 第二行永远被吃掉，等于没有这条日志。现在汇总行走主日志，始终可见。
     */
    {
        /* 打印"生效值"而不是"存储值"：总开关关掉时解析结果全为 0，
           打印存储值会让人以为功能还开着。 */
        RuntimeFeatureSwitches eff = normalize_runtime_feature_switches(g_mode_runtime_features);
        char log_msg[320];
        safe_snprintf(log_msg, sizeof(log_msg),
                      "模式 %s 的功能开关(生效) | master:%d | dynamic_tuning:%d | fas:%d | custom_thread:%d | dynamic_thread:%d | scene:%d | base:%d",
                      mode,
                      eff.scheduler_master_enabled ? 1 : 0,
                      eff.dynamic_tuning_enabled ? 1 : 0,
                      eff.fas_enabled ? 1 : 0,
                      eff.custom_thread_enabled ? 1 : 0,
                      eff.dynamic_thread_scheduler_enabled ? 1 : 0,
                      eff.scene_category_enabled ? 1 : 0,
                      eff.base_profile_enabled ? 1 : 0);
        log_message(log_msg);
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
    /* task-41：本次尝试读到的内容 hash（加载成功后才提交到 last_hash）。 */
    uint64_t attempt_hash;
    /* 连续失败次数，用于重试退避；成功清零。 */
    int failure_count;
};

static const long long kConfigReloadDebounceMs = 1500;
static const long long kConfigReloadCooldownMs = 3000;

static ConfigReloadWatchState g_mode_reload_watch = {};
static ConfigReloadWatchState g_scene_categories_reload_watch = {};
static ConfigReloadWatchState g_skip_processes_reload_watch = {};
static ConfigReloadWatchState g_fas_reload_watch = {};
static ConfigReloadWatchState g_dyn_reload_watch = {};
static ConfigReloadWatchState g_thread_signature_reload_watch = {};
static ConfigReloadWatchState g_thread_persistent_reload_watch = {};
static ConfigReloadWatchState g_runtime_features_reload_watch = {};
static ConfigReloadWatchState g_scheduler_reload_watch = {};
static ConfigReloadWatchState g_cpu_reload_watch = {};

static void build_persistent_thread_override_path(char* out, size_t out_size) {
    if (!out || out_size == 0) return;
    safe_snprintf(out, out_size, "%s%s.persistent.json", CPU_CONFIG_DIR, cpu_model);
}

static bool compute_file_content_hash64(const char* path, uint64_t* out_hash) {
    if (!path || !path[0] || !out_hash || !file_exists(path)) {
        return false;
    }

    FILE* fp = fopen(path, "rb");
    if (!fp) return false;
    uint64_t hash = 1469598103934665603ULL;
    unsigned char buffer[4096];
    size_t bytes_read = 0;
    while ((bytes_read = fread(buffer, 1, sizeof(buffer), fp)) > 0) {
        for (size_t i = 0; i < bytes_read; i++) {
            hash ^= buffer[i];
            hash *= 1099511628211ULL;
        }
    }
    const bool read_ok = ferror(fp) == 0;
    fclose(fp);
    if (!read_ok) return false;
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

/* 配置重载的 JSON 预检：给不在本文件内的 loader 判断文件是否可解析（失败可重试）。 */
static bool config_reload_json_file_valid(const char* path) {
    if (!path || !path[0] || !file_exists(path)) return false;
    char* data = NULL;
    size_t len = 0;
    if (!read_file_all(path, &data, &len) || !data || len == 0) {
        if (data) free(data);
        return false;
    }
    cJSON* root = cJSON_Parse(data);
    free(data);
    if (!root) return false;
    cJSON_Delete(root);
    return true;
}

/* 热重载失败日志：按路径限流，避免坏配置/缺失文件把 debug 日志刷爆。 */
static void config_reload_log_failure(const char* name, const char* path, const char* reason) {
    char throttle_key[128];
    safe_snprintf(throttle_key, sizeof(throttle_key), "reload-fail:%s", path ? path : "?");
    char message[320];
    safe_snprintf(message, sizeof(message),
                  "配置热重载失败(%s): %s（原因: %s），稍后重试",
                  name && name[0] ? name : "config",
                  path && path[0] ? path : "?",
                  reason && reason[0] ? reason : "unknown");
    log_config_message_throttled(throttle_key, 10000LL, message);
}

/*
 * task-41：尝试一次热重载。
 * 只判断「该不该试」，不提交 hash；文件被删除时也返回 true（attempt_hash=0），
 * 由调用方按默认值加载后调 finish_config_reload() 收尾，避免旧实现「先提交 hash
 * 再加载，加载失败永不重试」的问题。
 */
static bool take_pending_config_reload(ConfigReloadWatchState* state, const char* path) {
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

    if (!file_exists(path)) {
        /* 文件被删除：允许尝试一次「回退默认值」。 */
        state->attempt_hash = 0;
        return true;
    }

    uint64_t hash = 0;
    if (!compute_file_content_hash64(path, &hash)) {
        return false;
    }
    if (state->hash_ready && state->last_hash == hash) {
        state->dirty = false;
        state->dirty_since_ms = 0;
        return false;
    }
    state->attempt_hash = hash;
    return true;
}

/* task-41：加载结束后提交/回滚。loaded_ok=false 时保留 dirty 并按指数退避重试。 */
static void finish_config_reload(ConfigReloadWatchState* state, const char* name,
                                 const char* path, bool loaded_ok) {
    if (!state) return;
    const long long now_ms = get_current_time_ms();
    if (loaded_ok) {
        state->dirty = false;
        state->dirty_since_ms = 0;
        state->last_hash = state->attempt_hash;
        state->hash_ready = state->attempt_hash != 0;
        state->failure_count = 0;
        state->cooldown_until_ms = now_ms + kConfigReloadCooldownMs;
        return;
    }

    state->failure_count++;
    int retry_level = state->failure_count > 5 ? 5 : state->failure_count;
    long long backoff_ms = 1000LL << retry_level;
    if (backoff_ms > 30000LL) backoff_ms = 30000LL;
    state->dirty = true;
    state->dirty_since_ms = 0;
    state->cooldown_until_ms = now_ms + backoff_ms;
    config_reload_log_failure(name, path, "加载失败或内容无效");
}

/* inotify 队列溢出/监听异常：把全部配置标记为 dirty，不错过后续事件。 */
static void mark_all_config_reload_pending(const char* reason) {
    mark_config_reload_pending(&g_mode_reload_watch);
    mark_config_reload_pending(&g_scene_categories_reload_watch);
    mark_config_reload_pending(&g_skip_processes_reload_watch);
    mark_config_reload_pending(&g_fas_reload_watch);
    mark_config_reload_pending(&g_dyn_reload_watch);
    mark_config_reload_pending(&g_thread_signature_reload_watch);
    mark_config_reload_pending(&g_thread_persistent_reload_watch);
    mark_config_reload_pending(&g_runtime_features_reload_watch);
    mark_config_reload_pending(&g_scheduler_reload_watch);
    mark_config_reload_pending(&g_cpu_reload_watch);
    char message[256];
    safe_snprintf(message, sizeof(message),
                  "inotify 事件丢失/监听异常，已标记全部配置待重载（原因: %s）",
                  reason && reason[0] ? reason : "unknown");
    log_config_message_throttled("config-reload-overflow", 10000LL, message);
}

/*
 * task-41：inotify 不可用（inotify_init 或 inotify_add_watch 失败）时降级为
 * 定时轮询：每 2s 比较一次各配置文件 hash，变了就标记 dirty，热重载继续可用。
 */
static bool g_config_reload_poll_fallback = false;
static long long g_config_reload_poll_last_ms = 0;

void config_reload_enable_poll_fallback(const char* reason) {
    if (g_config_reload_poll_fallback) return;
    g_config_reload_poll_fallback = true;
    char message[256];
    safe_snprintf(message, sizeof(message),
                  "inotify 监听不可用，配置热重载降级为定时轮询（原因: %s）",
                  reason && reason[0] ? reason : "unknown");
    log_message(message);
}

static void poll_config_reload_fallback_if_needed() {
    if (!g_config_reload_poll_fallback) return;
    const long long now_ms = get_current_time_ms();
    if (g_config_reload_poll_last_ms != 0 && now_ms - g_config_reload_poll_last_ms < 2000) {
        return;
    }
    g_config_reload_poll_last_ms = now_ms;

    char persistent_path[256];
    build_persistent_thread_override_path(persistent_path, sizeof(persistent_path));
    const char* thread_config_path = get_thread_signature_config_path();

    struct PollItem {
        ConfigReloadWatchState* state;
        const char* path;
    };
    const PollItem items[] = {
        { &g_mode_reload_watch, MODE_FILE },
        { &g_scene_categories_reload_watch, SCENE_CATEGORIES_FILE },
        { &g_skip_processes_reload_watch, SCHEDULER_SKIP_PROCESSES_FILE },
        { &g_fas_reload_watch, FAS_CONFIG_FILE },
        { &g_dyn_reload_watch, DYN_CONFIG_FILE },
        { &g_thread_signature_reload_watch, thread_config_path },
        { &g_thread_persistent_reload_watch, persistent_path },
        { &g_runtime_features_reload_watch, get_runtime_feature_config_path() },
        { &g_scheduler_reload_watch, get_scheduler_config_path() },
        { &g_cpu_reload_watch, get_cpu_config_path() },
    };
    for (size_t i = 0; i < sizeof(items) / sizeof(items[0]); i++) {
        ConfigReloadWatchState* state = items[i].state;
        const char* path = items[i].path;
        if (!state || !path || !path[0]) continue;
        uint64_t hash = 0;
        if (compute_file_content_hash64(path, &hash)) {
            if (!state->hash_ready || state->last_hash != hash) {
                mark_config_reload_pending(state);
            }
        } else if (state->hash_ready) {
            /* 文件被删除：标记一次，让重载路径回退默认值/给出限流日志。 */
            mark_config_reload_pending(state);
        }
    }
}

void prime_config_reload_watch_hashes() {
    prime_config_reload_watch_state(&g_mode_reload_watch, MODE_FILE);
    prime_config_reload_watch_state(&g_scene_categories_reload_watch, SCENE_CATEGORIES_FILE);
    prime_config_reload_watch_state(
        &g_skip_processes_reload_watch,
        SCHEDULER_SKIP_PROCESSES_FILE);
    prime_config_reload_watch_state(&g_fas_reload_watch, FAS_CONFIG_FILE);
    prime_config_reload_watch_state(&g_dyn_reload_watch, DYN_CONFIG_FILE);
    prime_config_reload_watch_state(&g_thread_signature_reload_watch, get_thread_signature_config_path());

    char persistent_path[256];
    build_persistent_thread_override_path(persistent_path, sizeof(persistent_path));
    prime_config_reload_watch_state(&g_thread_persistent_reload_watch, persistent_path);

    prime_config_reload_watch_state(&g_runtime_features_reload_watch, get_runtime_feature_config_path());
    prime_config_reload_watch_state(&g_scheduler_reload_watch, get_scheduler_config_path());
    prime_config_reload_watch_state(&g_cpu_reload_watch, get_cpu_config_path());
}

void process_pending_config_reloads() {
    poll_config_reload_fallback_if_needed();

    if (g_mode_reload_watch.dirty &&
        take_pending_config_reload(&g_mode_reload_watch, MODE_FILE)) {
        log_message("模式文件内容已变更，正在热重载");
        char mode[64] = "balance";
        bool mode_file_read = false;
        FILE* mode_file = fopen(MODE_FILE, "r");
        if (mode_file) {
            char line[256];
            if (fgets(line, sizeof(line), mode_file)) {
                line[strcspn(line, "\n")] = 0;
                safe_strncpy(mode, line, sizeof(mode));
                mode_file_read = mode[0] != '\0';
            }
            fclose(mode_file);
        }
        if (!mode_file_read) {
            safe_strncpy(mode, "balance", sizeof(mode));
            log_config_message_throttled("reload-mode-missing", 30000LL,
                "mode.txt 缺失或为空，热重载回退到 balance 模式");
        }
        safe_strncpy(g_current_mode, mode, sizeof(g_current_mode));
        /* load_mode_settings_json() 保持 void（cpp 里有前置声明，不能改签名）：
           这里用 CPU 配置 JSON 预检判定「加载是否可成功」，坏 JSON/缺文件时保留 dirty 重试。 */
        const bool ok = config_reload_json_file_valid(get_cpu_config_path());
        if (ok) {
            load_mode_settings_json(mode);
            UnifiedScheduler::initialize_cluster_policies();
        }
        finish_config_reload(&g_mode_reload_watch, "mode.txt", MODE_FILE, ok);
    }

    if (g_scene_categories_reload_watch.dirty &&
        take_pending_config_reload(&g_scene_categories_reload_watch, SCENE_CATEGORIES_FILE)) {
        log_message("场景分类配置内容已变更，正在热重载");
        bool ok = config_reload_json_file_valid(SCENE_CATEGORIES_FILE);
        if (ok) {
            load_scene_categories_config();
        } else if (!file_exists(SCENE_CATEGORIES_FILE)) {
            /* 删除 categories.json = 回退「无场景分类规则」，算一次成功加载，不无限重试。 */
            load_scene_categories_config();
            ok = true;
        } else {
            config_reload_log_failure("categories.json", SCENE_CATEGORIES_FILE,
                                      "JSON 解析失败或文件不可读");
        }
        finish_config_reload(&g_scene_categories_reload_watch, "categories.json",
                             SCENE_CATEGORIES_FILE, ok);
    }

    if (g_skip_processes_reload_watch.dirty &&
        take_pending_config_reload(&g_skip_processes_reload_watch, SCHEDULER_SKIP_PROCESSES_FILE)) {
        log_message("进程调度排除名单已变更，正在热重载");
        bool ok = load_scheduler_skip_processes_config();
        if (!ok && !file_exists(SCHEDULER_SKIP_PROCESSES_FILE)) {
            /* 删除名单 = 空名单（loader 内部已清空并记日志），算成功加载。 */
            ok = true;
        }
        if (ok && refresh_scheduler_skip_process_state()) {
            handle_scheduler_skip_process_transition();
        }
        finish_config_reload(&g_skip_processes_reload_watch, "skip_processes.txt",
                             SCHEDULER_SKIP_PROCESSES_FILE, ok);
    }

    if (g_fas_reload_watch.dirty &&
        take_pending_config_reload(&g_fas_reload_watch, FAS_CONFIG_FILE)) {
        log_message("FAS 配置内容已变更，正在热重载");
        const bool valid = config_reload_json_file_valid(FAS_CONFIG_FILE);
        if (valid) {
            load_fas_config();
            /* task-41：外部开关不依赖 sched.txt，热重载后同步一次。 */
            g_mode_runtime_features.fas_enabled = g_fas_enabled_config;
            invalidate_runtime_feature_switch_cache();
        } else {
            config_reload_log_failure("fas.json", FAS_CONFIG_FILE, "JSON 解析失败或文件不可读");
        }
        finish_config_reload(&g_fas_reload_watch, "fas.json", FAS_CONFIG_FILE, valid);
    }

    if (g_dyn_reload_watch.dirty &&
        take_pending_config_reload(&g_dyn_reload_watch, DYN_CONFIG_FILE)) {
        log_message("动态调频配置内容已变更，正在热重载");
        const bool valid = config_reload_json_file_valid(DYN_CONFIG_FILE);
        if (valid) {
            load_dyn_config_json(g_current_mode);
            g_mode_runtime_features.dynamic_tuning_enabled = g_dyn_params.enabled;
            invalidate_runtime_feature_switch_cache();
        } else {
            config_reload_log_failure("dyn.json", DYN_CONFIG_FILE, "JSON 解析失败或文件不可读");
        }
        finish_config_reload(&g_dyn_reload_watch, "dyn.json", DYN_CONFIG_FILE, valid);
    }

    if (g_runtime_features_reload_watch.dirty) {
        const char* runtime_feature_config_path = get_runtime_feature_config_path();
        if (take_pending_config_reload(&g_runtime_features_reload_watch, runtime_feature_config_path)) {
            log_message("调度开关配置内容已变更，正在热重载");
            const bool ok = config_reload_json_file_valid(get_cpu_config_path());
            if (ok) {
                load_mode_settings_json(g_current_mode);
                UnifiedScheduler::initialize_cluster_policies();
            }
            finish_config_reload(&g_runtime_features_reload_watch, "sched.txt",
                                 runtime_feature_config_path, ok);
        }
    }

    if (g_thread_signature_reload_watch.dirty) {
        const char* thread_config_path = get_thread_signature_config_path();
        if (take_pending_config_reload(&g_thread_signature_reload_watch, thread_config_path)) {
            log_message("线程专项配置内容已变更，正在热重载");
            const bool ok = load_thread_signature_config();
            if (ok) {
                UnifiedScheduler::reload_thread_config_now();
            }
            finish_config_reload(&g_thread_signature_reload_watch, "thread config",
                                 thread_config_path, ok);
        }
    }

    if (g_thread_persistent_reload_watch.dirty) {
        char persistent_path[256];
        build_persistent_thread_override_path(persistent_path, sizeof(persistent_path));
        if (take_pending_config_reload(&g_thread_persistent_reload_watch, persistent_path)) {
            log_message("设备持久规则覆盖内容已变更，正在热重载");
            bool ok;
            if (!file_exists(persistent_path)) {
                /* 删除覆盖文件 = 撤销设备级持久规则：重读主配置即可，算成功。 */
                load_thread_signature_config();
                UnifiedScheduler::reload_thread_config_now();
                ok = true;
            } else {
                ok = config_reload_json_file_valid(persistent_path);
                if (ok) {
                    load_thread_signature_config();
                    UnifiedScheduler::reload_thread_config_now();
                } else {
                    config_reload_log_failure("persistent threads", persistent_path,
                                              "JSON 解析失败或文件不可读");
                }
            }
            finish_config_reload(&g_thread_persistent_reload_watch, "persistent threads",
                                 persistent_path, ok);
        }
    }

    if (g_scheduler_reload_watch.dirty) {
        const char* scheduler_config_path = get_scheduler_config_path();
        if (take_pending_config_reload(&g_scheduler_reload_watch, scheduler_config_path)) {
            log_message("会话配置内容已变更，正在热重载");
            const bool ok = load_scheduler_session_config_json(g_current_mode);
            if (ok) {
                UnifiedScheduler::reload_thread_config_now();
            }
            finish_config_reload(&g_scheduler_reload_watch, "scheduler config",
                                 scheduler_config_path, ok);
        }
    }

    if (g_cpu_reload_watch.dirty) {
        const char* cpu_config_path = get_cpu_config_path();
        if (take_pending_config_reload(&g_cpu_reload_watch, cpu_config_path)) {
            log_message("CPU 配置内容已变更，正在热重载");
            const bool ok = config_reload_json_file_valid(cpu_config_path);
            if (ok) {
                load_mode_settings_json(g_current_mode);
                UnifiedScheduler::initialize_cluster_policies();
            } else {
                config_reload_log_failure("cpu config", cpu_config_path,
                                          "JSON 解析失败或文件不可读");
            }
            finish_config_reload(&g_cpu_reload_watch, "cpu config", cpu_config_path, ok);
        }
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

        /* task-41：队列溢出时丢弃的事件无法补，全部标记 dirty 让热重载重新扫描。 */
        if (event->mask & IN_Q_OVERFLOW) {
            mark_all_config_reload_pending("IN_Q_OVERFLOW");
            continue;
        }
        /* 被监听的目录本身被删除/移动：监听已失效，降级轮询并全量重载。 */
        if (event->mask & (IN_DELETE_SELF | IN_MOVE_SELF)) {
            mark_all_config_reload_pending("watch directory removed");
            config_reload_enable_poll_fallback("watch directory removed");
            continue;
        }
        if (event->len == 0) {
            continue;
        }

        // Ignore temporary files
        if (strstr(event->name, ".tmp") != NULL) {
            continue;
        }

        /*
         * 写完成 / 原子替换 / 删除都算配置变更：删除后由重载路径回退默认值
         * （task-41：add_watch 也订阅了 IN_DELETE|IN_MOVED_FROM）。
         */
        if (!(event->mask & (IN_MODIFY | IN_CLOSE_WRITE | IN_MOVED_TO | IN_CREATE |
                             IN_MOVED_FROM | IN_DELETE))) {
            continue;
        }

        if (event->wd == wd_general_config_dir) {
            if (strcmp(event->name, "mode.txt") == 0) {
                mark_config_reload_pending(&g_mode_reload_watch);
            } else if (strcmp(event->name, "categories.json") == 0) {
                mark_config_reload_pending(&g_scene_categories_reload_watch);
            } else if (event_name_matches_config_path(
                           event->name,
                           SCHEDULER_SKIP_PROCESSES_FILE)) {
                mark_config_reload_pending(&g_skip_processes_reload_watch);
            } else if (event_name_matches_config_path(event->name, get_runtime_feature_config_path())) {
                mark_config_reload_pending(&g_runtime_features_reload_watch);
            }
        } else if (event->wd == wd_cpu_config_dir) {
            if (strcmp(event->name, "fas.json") == 0) {
                mark_config_reload_pending(&g_fas_reload_watch);
            } else if (strcmp(event->name, "dyn.json") == 0) {
                mark_config_reload_pending(&g_dyn_reload_watch);
            } else if (thread_config_event_name_matches(event->name)) {
                /* 线程配置候选名（<cpuModel>.threads.json / <拓扑>.threads.json /
                   <拓扑>.json，含反向拓扑）由 sysfs_runtime 统一生成，避免每个事件 stat。 */
                mark_config_reload_pending(&g_thread_signature_reload_watch);
            } else {
                // 检查是否触发了设备级 persistent 规则覆盖文件变更
                char persistent_path[256];
                build_persistent_thread_override_path(persistent_path, sizeof(persistent_path));
                if (event_name_matches_config_path(event->name, persistent_path)) {
                    mark_config_reload_pending(&g_thread_persistent_reload_watch);
                }
            }
            if (event_name_matches_config_path(event->name, get_scheduler_config_path())) {
                mark_config_reload_pending(&g_scheduler_reload_watch);
            } else if (event_name_matches_config_path(event->name, get_cpu_config_path())) {
                mark_config_reload_pending(&g_cpu_reload_watch);
            }
        }
    }
}
