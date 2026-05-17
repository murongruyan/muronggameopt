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
    long long now_ms = get_current_time_ms();
    bool next_is_game = next_package[0] && strcmp(next_package, "unknown") != 0 && is_game_package_name(next_package);
    bool tracked_session = g_scheduler_session.phase != SESSION_IDLE && g_scheduler_session.package[0];
    bool same_tracked_game = tracked_session && strcmp(g_scheduler_session.package, next_package) == 0;
    if (tracked_session && !same_tracked_game) {
        if (next_is_game) {
            reset_scheduler_session_locked("foreground_game_changed");
        } else if (g_scheduler_session.phase == SESSION_LEARNING || g_scheduler_session.phase == SESSION_LOCKED) {
            g_scheduler_session.phase = SESSION_BACKGROUND_HOLD;
            g_scheduler_session.background_since_ms = now_ms;
            char msg[320];
            safe_snprintf(msg, sizeof(msg),
                          "调度会话转后台保持 | app:%s | pid:%d | phase:%s",
                          g_scheduler_session.package,
                          (int)g_scheduler_session.pid,
                          session_phase_to_string(g_scheduler_session.phase));
            log_affinity_message(msg);
            trace_instantf("murong_session", "background_hold app=%s pid=%d",
                           g_scheduler_session.package,
                           (int)g_scheduler_session.pid);
        }
    }
    restore_all_thread_runtime_adjustments_locked("foreground_app_changed");
    safe_strncpy(g_foreground_package, next_package, sizeof(g_foreground_package));
    refresh_active_thread_app_rule_locked(next_package);
    apply_active_scheduler_session_override_locked();
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
            std::lock_guard<std::mutex> lock(g_mutex);
            detach_frame_probe_locked();
            if (!g_threads.empty()) {
                restore_all_thread_runtime_adjustments_locked("paused_by_scheduler_conflict");
                clear_thread_runtime_map_locked();
                clear_target_tid_map_locked();
            }
            usleep(1000 * 1000);
            continue;
        }
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            refresh_foreground_pid_locked(false);
            detach_frame_probe_locked();
            if (is_bpf_thread_enabled_for_app(g_foreground_package)) {
                sync_target_tgid_locked();
                poll_process_event_ring_locked();
                collect_foreground_threads_locked();
                pull_bpf_runtimes_locked(get_current_time_ms());
                sync_render_thread_targets_locked(false);
            } else {
                if (!g_threads.empty()) {
                    restore_all_thread_runtime_adjustments_locked(
                        "bpf_thread_disabled");
                    clear_thread_runtime_map_locked();
                    clear_target_tid_map_locked();
                }
            }
        }
        usleep(100 * 1000);
    }
    return NULL;
}

void refresh_bpf_targets_now() {
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
    if (is_bpf_thread_enabled_for_app(g_foreground_package)) {
        sync_target_tgid_locked();
        poll_process_event_ring_locked();
        collect_foreground_threads_locked();
        sync_render_thread_targets_locked(true);
    } else {
        if (!g_threads.empty()) {
            restore_all_thread_runtime_adjustments_locked("bpf_thread_disabled");
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
    apply_active_scheduler_session_override_locked();
    clear_performance_boosts();
    g_learning_samples.clear();
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

    if (!is_bpf_thread_enabled_for_app(g_current_foreground_app)) {
        std::lock_guard<std::mutex> lock(g_mutex);
        clear_performance_boosts();
        if (!g_threads.empty()) {
            restore_all_thread_runtime_adjustments_locked("bpf_thread_disabled");
        }
        return;
    }

    AllCpuStats stats;
    if (!read_all_cpu_stats(&stats) || !stats.valid) {
        return;
    }

    std::vector<float> cluster_scores(cluster_count, 0.0f);
    float thermal = estimate_thermal_pressure();
    for (int i = 0; i < cluster_count; i++) {
        cluster_scores[i] = compute_cluster_score(i, g_cluster_policies[i], stats, thermal);
    }

    {
        std::lock_guard<std::mutex> lock(g_mutex);
        /* 命中显式规则时，会话层彻底短路 — 不学习、不锁定、不守护 */
        if (should_suppress_dynamic_thread_scheduler_locked(g_current_foreground_app)) {
            if (g_scheduler_session.phase != SESSION_IDLE) {
                reset_scheduler_session_locked("explicit_thread_affinity");
            }
        } else {
            apply_active_scheduler_session_override_locked();
            update_scheduler_session_locked(now_ms);
            if (guard_locked_assignments_locked(now_ms)) {
                goto freq_only;
            }
        }
        for (auto& kv : g_threads) {
            ThreadRuntimeState& state = kv.second;
            const char* state_package = state.package.empty() ? g_foreground_package : state.package.c_str();
            bool is_foreground_state = state.pid > 0 &&
                                       state.pid == g_foreground_pid &&
                                       state_package &&
                                       strcmp(state_package, g_foreground_package) == 0;
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
                continue;
            }

            if (state.frozen) {
                continue;
            }
            if (now_ms - state.last_classify_ms >= 500) {
                state.cls = classify_thread(state.tid, state.comm.c_str(), state.cpu_usage_pct);
                state.last_classify_ms = now_ms;
            }
            bool hot_priority = is_hot_priority_thread_locked(state.tid, state.comm.c_str(), state.cpu_usage_pct);
            bool priority_name = is_render_thread_name(state.comm.c_str()) ||
                                 is_main_thread_name_for_package(g_foreground_package, state.comm.c_str(), state.tid);
            bool freeze_candidate = is_thread_freeze_candidate_locked(
                state.tid, state.comm.c_str(), state.cpu_usage_pct);
            if (freeze_candidate) {
                if (state.first_hot_ms == 0) {
                    state.first_hot_ms = now_ms;
                }
                if (state.hot_sample_count < 1000000) {
                    state.hot_sample_count++;
                }
            } else {
                state.first_hot_ms = 0;
                state.hot_sample_count = 0;
                state.target_stable_since_ms = 0;
                state.last_target_cluster = -1;
                state.last_target_cpus.clear();
            }
            if (priority_name && !hot_priority) {
                state.cls = THREAD_CLASS_BACKGROUND;
            }
            int target_cluster = select_target_cluster(state.cls, cluster_scores);
            /* 主线程/渲染线程防漂移 */
            if (state.cls == THREAD_CLASS_MAIN && state.assigned_cluster >= 0 &&
                state.assigned_cluster != target_cluster &&
                state.last_migrate_ms > 0 && now_ms - state.last_migrate_ms < 5000) {
                target_cluster = state.assigned_cluster;
            } else if (state.cls == THREAD_CLASS_RENDER && state.assigned_cluster >= 0 &&
                       state.assigned_cluster != target_cluster) {
                target_cluster = state.assigned_cluster;
            }
            if (!has_explicit_mask) {
                has_explicit_mask = resolve_affinity_template(
                    state_package, state.tid, state.comm.c_str(), state.cls, state.cpu_usage_pct, stats, explicit_cpus, explicit_cluster);
            }
            if (priority_name && !hot_priority &&
                !(matched_rule && matched_rule->action &&
                  (matched_rule->action->has_clusters || matched_rule->action->has_cpus))) {
                has_explicit_mask = false;
            }
            normalize_cpu_mask(explicit_cpus);
            if (has_explicit_mask) {
                target_cluster = explicit_cluster;
            }

            update_thread_target_stability_locked(state, target_cluster, explicit_cpus, now_ms);
            if (freeze_candidate &&
                should_freeze_thread_locked(state, target_cluster, explicit_cpus, now_ms)) {
                state.frozen = true;
                state.frozen_at_ms = now_ms;
                state.frozen_cluster = state.assigned_cluster;
                state.frozen_cpus = state.assigned_cpus;

                char msg[320];
                safe_snprintf(
                    msg, sizeof(msg),
                    "线程冻结 | app:%s | tid:%d | comm:%s | freeze_cluster:%d | mask_size:%d | learn_ms:%lld | action:stop_reassign",
                    g_foreground_package,
                    (int)state.tid,
                    state.comm.c_str(),
                    state.frozen_cluster,
                    (int)state.frozen_cpus.size(),
                    (long long)(now_ms - state.first_hot_ms));
                log_affinity_message(msg);
                continue;
            }

            bool need_apply = false;
            if (has_explicit_mask) {
                need_apply = (state.assigned_cluster != target_cluster) ||
                             !cpu_mask_equals(state.assigned_cpus, explicit_cpus);
            } else {
                need_apply = (state.assigned_cluster != target_cluster) ||
                             !state.assigned_cpus.empty();
            }

            if (need_apply &&
                (state.last_migrate_ms == 0 || now_ms - state.last_migrate_ms >= 500)) {
                bool applied = has_explicit_mask ?
                    set_thread_affinity_mask(state.tid, explicit_cpus) :
                    set_thread_affinity(state.tid, target_cluster);
                if (applied) {
                    state.assigned_cluster = target_cluster;
                    state.last_migrate_ms = now_ms;
                    state.assigned_cpus = has_explicit_mask ? explicit_cpus : std::vector<int>();
                }
            }

        }
    }
    /* 定期清理不再使用的 epoch 状态 */
    cleanup_stale_selection_epochs_locked();

freq_only:
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

    record_learning_sample();
    tune_learning_engine_if_needed();
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
