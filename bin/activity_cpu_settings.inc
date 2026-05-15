void apply_cpuctl_settings(const CpuCtlSettings* settings) {
    const char* groups[] = {
        "background",
        "system-background",
        "foreground",
        "top-app"
    };

    const CpuCtlGroup* group_settings[] = {
        &settings->background,
        &settings->systembackground,
        &settings->foreground,
        &settings->topapp
    };

    for (int i = 0; i < 4; i++) {
        char base_path[256];
        safe_snprintf(base_path, sizeof(base_path), "/dev/cpuctl/%s", groups[i]);

        if (group_settings[i]->uclamp_min >= 0) {
            char path[512], value[16];
            safe_snprintf(path, sizeof(path), "%s/cpu.uclamp.min", base_path);
            safe_snprintf(value, sizeof(value), "%d", group_settings[i]->uclamp_min);
            lock_val(path, value);
        }

        if (group_settings[i]->uclamp_max >= 0) {
            char path[512], value[16];
            safe_snprintf(path, sizeof(path), "%s/cpu.uclamp.max", base_path);
            safe_snprintf(value, sizeof(value), "%d", group_settings[i]->uclamp_max);
            lock_val(path, value);
        }

        if (group_settings[i]->latency_sensitive >= 0) {
            char path[512], value[16];
            safe_snprintf(path, sizeof(path), "%s/cpu.uclamp.latency_sensitive", base_path);
            safe_snprintf(value, sizeof(value), "%d", group_settings[i]->latency_sensitive);
            lock_val(path, value);
        }

        if (group_settings[i]->shares >= 0) {
            char path[512], value[16];
            safe_snprintf(path, sizeof(path), "%s/cpu.shares", base_path);
            safe_snprintf(value, sizeof(value), "%d", group_settings[i]->shares);
            lock_val(path, value);
        }
    }
}

void apply_settings() {
    g_runtime_stats.apply_settings_count.fetch_add(1);

    const bool base_profile_enabled = is_base_profile_runtime_enabled();

    if (base_profile_enabled) {
        lock_val("/dev/cpuset/background/cpus", cpuset.background);
        lock_val("/dev/cpuset/system-background/cpus", cpuset.systembackground);
        lock_val("/dev/cpuset/foreground/cpus", cpuset.foreground);
        lock_val("/dev/cpuset/top-app/cpus", cpuset.topapp);
    }

    for (int i = 0; i < policy_count; i++) {
        CpuPolicy* policy = &cpu_policies[i];
        char path[256];

        safe_snprintf(path, sizeof(path),
            "/sys/devices/system/cpu/cpufreq/policy%d/scaling_max_freq",
            policy->policy_num);
        char max_freq_str[32];
        safe_snprintf(max_freq_str, sizeof(max_freq_str), "%d", policy->max_freq);
        lock_val(path, max_freq_str);

        safe_snprintf(path, sizeof(path),
            "/sys/devices/system/cpu/cpufreq/policy%d/scaling_min_freq",
            policy->policy_num);
        char min_freq_str[32];
        safe_snprintf(min_freq_str, sizeof(min_freq_str), "%d", policy->min_freq);
        lock_val(path, min_freq_str);

        if (policy->governor[0] != 0) {
            if (!is_governor_supported(policy->policy_num, policy->governor)) {
                char log_msg[128];
                safe_snprintf(log_msg, sizeof(log_msg),
                    "调速器 %s 在policy%d上不支持，跳过设置",
                    policy->governor, policy->policy_num);
                log_message(log_msg);
                continue;
            }

            safe_snprintf(path, sizeof(path),
                "/sys/devices/system/cpu/cpufreq/policy%d/scaling_governor",
                policy->policy_num);
            lock_val(path, policy->governor);
            set_governor_params(policy->policy_num, policy->governor);
        } else {
            char current_gov[64] = {0};
            if (read_current_governor(policy->policy_num, current_gov, sizeof(current_gov))) {
                set_governor_params(policy->policy_num, current_gov);
            } else {
                set_governor_params(policy->policy_num, "");
            }
        }
    }

    if (base_profile_enabled) {
        apply_cpuctl_settings(&cpuctl);

        if (sched_boost_settings.top_boost >= 0 && sched_boost_settings.sched_boost >= 0) {
            set_sched_boost(sched_boost_settings.top_boost, sched_boost_settings.sched_boost);
        }

        if (sched_config_settings.downmigrate >= 0) {
            set_sched_config(sched_config_settings.downmigrate, sched_config_settings.upmigrate,
                sched_config_settings.group_downmigrate, sched_config_settings.group_upmigrate);
        }

        if (stune_settings.prefer_idle >= 0 && stune_settings.boost >= 0) {
            set_stune_topapp(stune_settings.prefer_idle, stune_settings.boost);
        }

        for (int i = 0; i < g_extra_write_count; i++) {
            if (g_extra_writes[i].path[0] != 0) {
                lock_val(g_extra_writes[i].path, g_extra_writes[i].value);
            }
        }
    }
}
