void initialize_cluster_policies() {
    for (int i = 0; i < cluster_count; i++) {
        int policy_num = cpu_clusters[i].policies[0];
        int policy_idx = -1;
        for (int k = 0; k < policy_count; k++) {
            if (cpu_policies[k].policy_num == policy_num) {
                policy_idx = k;
                break;
            }
        }
        ClusterPolicy policy = {};
        policy.cluster_id = i;
        policy.min_freq = (policy_idx >= 0) ? cpu_policies[policy_idx].min_freq : 0;
        policy.max_freq = (policy_idx >= 0) ? cpu_policies[policy_idx].max_freq : policy.min_freq;
        policy.dyn_params = g_cluster_dyn_params[i];
        policy.target_load = policy.dyn_params.target_load;
        policy.load_margin = policy.dyn_params.load_margin;
        int range = policy.max_freq - policy.min_freq;
        int render_floor = policy.min_freq + (int)((float)range * 0.6f);
        if (i == cluster_count - 1 && render_floor < 2500000) {
            render_floor = 2500000;
        }
        if (render_floor > policy.max_freq) render_floor = policy.max_freq;
        if (render_floor < policy.min_freq) render_floor = policy.min_freq;
        policy.render_min_boost_freq = render_floor;
        policy.w_idle = 1.0f + 0.1f * (float)(cluster_count - i);
        policy.w_freq = 0.8f + 0.1f * (float)i;
        policy.w_rq = 1.0f;
        policy.w_thermal = 0.8f + 0.1f * (float)i;
        g_cluster_policies[i] = policy;
        g_cluster_policy_base[i] = policy;
        g_cluster_min_overrides[i] = policy.min_freq;
        g_cluster_max_overrides[i] = policy.max_freq;
    }
}

void notify_foreground_app_changed(const char* package) {
    std::lock_guard<std::mutex> lock(g_mutex);
    const char* next_package = (package && package[0]) ? package : "unknown";
    if (g_scheduler_session.phase != SESSION_IDLE &&
        (!g_scheduler_session.package[0] || strcmp(g_scheduler_session.package, next_package) != 0)) {
        reset_scheduler_session_locked("foreground_game_changed");
    }
    restore_all_thread_runtime_adjustments_locked("foreground_app_changed");
    safe_strncpy(g_foreground_package, next_package, sizeof(g_foreground_package));
    refresh_active_thread_app_rule_locked(next_package);
    g_foreground_pid = -1;
    g_last_pid_lookup_ms = 0;
    g_last_render_tid_sync_ms = 0;
    g_threads.clear();
    clear_all_selection_epochs_locked();
    clear_performance_boosts();
    detach_frame_probe_locked();
    sync_target_tgid_locked();
    clear_thread_runtime_map_locked();
    clear_target_tid_map_locked();
    for (int i = 0; i < cluster_count; i++) {
        g_cluster_min_overrides[i] = g_cluster_policies[i].min_freq;
        g_cluster_max_overrides[i] = g_cluster_policies[i].max_freq;
    }
}

void reset_cluster_freq_overrides() {
    clear_performance_boosts();
    for (int i = 0; i < cluster_count; i++) {
        g_cluster_min_overrides[i] = g_cluster_policies[i].min_freq;
        g_cluster_max_overrides[i] = g_cluster_policies[i].max_freq;
    }
}

static void* runtime_monitor_thread(void* arg) {
    (void)arg;
    while (!g_runtime_thread_stop.load()) {
        if (should_pause_our_game_scheduler()) {
            {
                std::lock_guard<std::mutex> lock(g_mutex);
                detach_frame_probe_locked();
                if (!g_threads.empty()) {
                    restore_all_thread_runtime_adjustments_locked("paused_by_scheduler_conflict");
                    clear_thread_runtime_map_locked();
                    clear_target_tid_map_locked();
                }
            }
            usleep(1000 * 1000);
            continue;
        }
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            refresh_foreground_pid_locked(false);
            detach_frame_probe_locked();
            if (is_bpf_thread_runtime_enabled() &&
                !is_scheduler_control_panel_package(g_foreground_package)) {
                sync_target_tgid_locked();
                poll_process_event_ring_locked();
                collect_foreground_threads_locked();
                pull_bpf_runtimes_locked(get_current_time_ms());
                sync_render_thread_targets_locked(false);
            } else {
                if (!g_threads.empty()) {
                    restore_all_thread_runtime_adjustments_locked(
                        "rules_only_disabled");
                    clear_thread_runtime_map_locked();
                    clear_target_tid_map_locked();
                }
            }
        }
        usleep(100 * 1000);
    }
    return NULL;
}

void refresh_rules_only_targets_now() {
    if (should_pause_our_game_scheduler()) {
        std::lock_guard<std::mutex> lock(g_mutex);
        detach_frame_probe_locked();
        if (!g_threads.empty()) {
            restore_all_thread_runtime_adjustments_locked("paused_by_scheduler_conflict");
            clear_thread_runtime_map_locked();
            clear_target_tid_map_locked();
        }
        return;
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    refresh_foreground_pid_locked(true);
    detach_frame_probe_locked();
    if (is_bpf_thread_runtime_enabled() &&
        !is_scheduler_control_panel_package(g_foreground_package)) {
        sync_target_tgid_locked();
        poll_process_event_ring_locked();
        collect_foreground_threads_locked();
        pull_bpf_runtimes_locked(get_current_time_ms());
        sync_render_thread_targets_locked(true);
    } else {
        if (!g_threads.empty()) {
            restore_all_thread_runtime_adjustments_locked("rules_only_disabled");
            clear_thread_runtime_map_locked();
            clear_target_tid_map_locked();
        }
    }
}

void reload_thread_config_now() {
    std::lock_guard<std::mutex> lock(g_mutex);
    restore_all_thread_runtime_adjustments_locked("thread_config_reloaded");
    reset_scheduler_session_locked("thread_config_reloaded");
    refresh_active_thread_app_rule_locked(g_foreground_package);
    clear_performance_boosts();
    g_threads.clear();
    clear_all_selection_epochs_locked();
    g_foreground_pid = -1;
    g_last_pid_lookup_ms = 0;
    g_last_thread_scan_ms = 0;
    g_last_render_tid_sync_ms = 0;
    g_foreground_pid_refresh_pending = true;
    sync_target_tgid_locked();
    clear_thread_runtime_map_locked();
    clear_target_tid_map_locked();
    detach_frame_probe_locked();
}

void start_threads() {
    init_bpf();
    if (!g_runtime_thread_started.exchange(1)) {
        g_runtime_thread_stop.store(0);
        pthread_create(&g_runtime_tid, NULL, runtime_monitor_thread, NULL);
    }
}

void stop_threads() {
    if (g_runtime_thread_started.exchange(0)) {
        g_runtime_thread_stop.store(1);
        pthread_join(g_runtime_tid, NULL);
    }
    cleanup_bpf();
}

void run_scheduler_step() {
    long long now_ms = get_current_time_ms();
    refresh_scene_policy_state(g_current_foreground_app);
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_scheduler_session.pid > 0 && kill(g_scheduler_session.pid, 0) != 0) {
            reset_scheduler_session_locked("pid_gone");
        }
        if (consume_urgent_render_flag_locked() && cluster_count > 0) {
            request_performance_boost(cluster_count - 1, 180);
        }
    }

    if (should_pause_our_game_scheduler()) {
        return;
    }

    if (!is_bpf_thread_runtime_enabled() ||
        is_scheduler_control_panel_package(g_current_foreground_app)) {
        std::lock_guard<std::mutex> lock(g_mutex);
        clear_performance_boosts();
        if (!g_threads.empty()) {
            restore_all_thread_runtime_adjustments_locked("rules_only_disabled");
        }
        return;
    }

    AllCpuStats stats;
    if (!read_all_cpu_stats(&stats) || !stats.valid) {
        return;
    }

    {
        std::lock_guard<std::mutex> lock(g_mutex);
        for (auto& kv : g_threads) {
            ThreadRuntimeState& state = kv.second;
            const char* state_package = state.package.empty() ? g_foreground_package : state.package.c_str();
            bool is_foreground_state = state.pid > 0 &&
                                       state.pid == g_foreground_pid &&
                                       state_package &&
                                       strcmp(state_package, g_foreground_package) == 0;
            if (is_foreground_state) {
                ensure_scheduler_session_tracking_locked();
            }
            ThreadJsonResolvedRule resolved_rule = {};
            const ThreadJsonResolvedRule* matched_rule = resolve_thread_json_rule_locked(
                state_package, state.tid, state.comm.c_str(), state.cpu_usage_pct, state.cls, &resolved_rule);
            if (matched_rule) {
                apply_thread_json_action_locked(state, matched_rule);
            } else {
                restore_thread_runtime_adjustments_locked(state, "rule_not_matched");
                state.has_explicit_affinity_rule = false;
            }

            std::vector<int> explicit_cpus;
            int explicit_cluster = -1;
            bool has_explicit_mask = false;
            if (matched_rule && matched_rule->action &&
                (matched_rule->action->has_clusters || matched_rule->action->has_cpus)) {
                resolve_thread_json_action_cpus_locked(*matched_rule->action, explicit_cpus);
                has_explicit_mask = !explicit_cpus.empty();
                if (has_explicit_mask) {
                    explicit_cluster = highest_cluster_for_mask(explicit_cpus);
                    bool prewarm_expanded = maybe_expand_main_single_core_mask_for_spawn_locked(
                        state, matched_rule, now_ms, explicit_cpus, explicit_cluster);
                    if (prewarm_expanded && now_ms - state.last_event_log_ms > 2000) {
                        char cpu_list[96] = {0};
                        size_t pos = 0;
                        for (size_t i = 0; i < explicit_cpus.size(); i++) {
                            int written = safe_snprintf(
                                cpu_list + pos,
                                sizeof(cpu_list) - pos,
                                "%s%d",
                                i == 0 ? "" : ",",
                                explicit_cpus[i]);
                            if (written <= 0 || (size_t)written >= sizeof(cpu_list) - pos) break;
                            pos += (size_t)written;
                        }
                        char msg[256];
                        safe_snprintf(
                            msg,
                            sizeof(msg),
                            "主线程预热放宽 | app:%s | tid:%d | comm:%s | prewarm_mask:%s",
                            state_package ? state_package : "unknown",
                            (int)state.tid,
                            state.comm.c_str(),
                            cpu_list[0] ? cpu_list : "unknown");
                        log_affinity_message(msg);
                        state.last_event_log_ms = now_ms;
                    }
                }
            }
            /* 标记显式规则命中 */
            state.has_explicit_affinity_rule = has_explicit_mask && matched_rule &&
                (matched_rule->source_kind != THREAD_JSON_RULE_SOURCE_DEFAULT &&
                 matched_rule->source_kind != THREAD_JSON_RULE_SOURCE_SCENE_DEFAULT);
            if (state.has_explicit_affinity_rule) {
                if (matched_rule->rebind_cooldown_ms > 0) {
                    state.static_bind_cooldown_ms = static_cast<long long>(matched_rule->rebind_cooldown_ms);
                } else {
                    state.static_bind_cooldown_ms = kRebindCooldownMs;
                }
            } else {
                state.static_bind_cooldown_ms = 0;
            }

            if (!is_foreground_state) {
                normalize_cpu_mask(explicit_cpus);
                if (!has_explicit_mask) {
                    continue;
                }
                bool actual_mask_mismatch =
                    !is_thread_affinity_mask_matching_locked(state.tid, explicit_cpus);
                bool need_apply = (state.assigned_cluster != explicit_cluster) ||
                                  !cpu_mask_equals(state.assigned_cpus, explicit_cpus) ||
                                  actual_mask_mismatch;
                if (need_apply &&
                    (actual_mask_mismatch ||
                     state.last_migrate_ms == 0 ||
                     now_ms - state.last_migrate_ms >= 500)) {
                    bool applied = set_thread_affinity_mask(state.tid, explicit_cpus);
                    if (applied) {
                        state.assigned_cluster = explicit_cluster;
                        state.last_migrate_ms = now_ms;
                        state.assigned_cpus = explicit_cpus;
                    }
                }
                continue;
            }

            /* ===== 静态规则快速路径 — 跳过动态分类/迁移/冻结 ===== */
            if (state.has_explicit_affinity_rule && has_explicit_mask) {
                /* 清除动态链路残留状态（冻结/稳定/热度），避免影响其他线程选核 */
                state.frozen = false;
                state.frozen_cpus.clear();
                state.frozen_cluster = -1;
                state.frozen_at_ms = 0;
                state.target_stable_since_ms = 0;
                state.last_target_cluster = -1;
                state.last_target_cpus.clear();
                state.first_hot_ms = 0;
                state.hot_sample_count = 0;
                bool actual_mask_mismatch =
                    !is_thread_affinity_mask_matching_locked(state.tid, explicit_cpus);
                bool mask_changed = (state.assigned_cluster != explicit_cluster) ||
                                    !cpu_mask_equals(state.assigned_cpus, explicit_cpus) ||
                                    actual_mask_mismatch;
                if (mask_changed) {
                    bool cooldown_ok = actual_mask_mismatch ||
                        state.last_static_bind_ms == 0 ||
                        now_ms - state.last_static_bind_ms >= state.static_bind_cooldown_ms;
                    if (cooldown_ok) {
                        bool applied = set_thread_affinity_mask(state.tid, explicit_cpus);
                        if (applied) {
                            state.assigned_cluster = explicit_cluster;
                            state.last_migrate_ms = now_ms;
                            state.last_static_bind_ms = now_ms;
                            state.assigned_cpus = explicit_cpus;
                        }
                    }
                }
                if (is_foreground_state &&
                    is_thread_affinity_mask_matching_locked(state.tid, explicit_cpus)) {
                    maybe_latch_primary_thread_tid_locked(state);
                }
                continue;
            }
            restore_thread_runtime_adjustments_locked(state, "free_rules_only");
            state.has_explicit_affinity_rule = false;
            state.frozen = false;
            state.frozen_cpus.clear();
            state.frozen_cluster = -1;
            state.frozen_at_ms = 0;
            state.target_stable_since_ms = 0;
            state.last_target_cluster = -1;
            state.last_target_cpus.clear();
            state.first_hot_ms = 0;
            state.hot_sample_count = 0;
            state.last_classify_ms = now_ms;

        }
    }
    /* 定期清理不再使用的 epoch 状态 */
    cleanup_stale_selection_epochs_locked();
    for (int i = 0; i < cluster_count; i++) {
        int min_override = g_cluster_policies[i].min_freq;
        int policy_num = cpu_clusters[i].policies[0];
        int policy_idx = -1;
        for (int k = 0; k < policy_count; k++) {
            if (cpu_policies[k].policy_num == policy_num) {
                policy_idx = k;
                break;
            }
        }
        int max_override = g_cluster_policies[i].max_freq;
        if (g_scene_policy.screen_off) {
            int floor = get_policy_lowest_available_freq(policy_idx);
            min_override = floor;
            max_override = floor;
        } else {
            int scene_floor = get_scene_floor_freq_for_cluster(policy_idx, i, g_foreground_package);
            if (scene_floor > min_override) {
                min_override = scene_floor;
            }
        }
        if (max_override < min_override) max_override = min_override;
        g_cluster_min_overrides[i] = min_override;
        g_cluster_max_overrides[i] = max_override;
        bool any_dyn_enabled = false;
        for (int j = 0; j < cluster_count; j++) {
            if (g_cluster_dyn_params[j].enabled) {
                any_dyn_enabled = true;
                break;
            }
        }
        if (!any_dyn_enabled) {
            update_cluster_freq(i, min_override, max_override);
        }
    }

    g_prev_stats = stats;
    g_prev_stats_valid = 1;
}

int get_cluster_min_override(int cluster_id) {
    if (cluster_id < 0 || cluster_id >= cluster_count) return 0;
    return g_cluster_min_overrides[cluster_id];
}

int get_cluster_max_override(int cluster_id) {
    if (cluster_id < 0 || cluster_id >= cluster_count) return 0;
    return g_cluster_max_overrides[cluster_id];
}
