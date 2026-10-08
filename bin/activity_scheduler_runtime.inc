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
        policy.w_idle = 1.0f + 0.1f * (float)(cluster_count - i);
        policy.w_freq = 0.8f + 0.1f * (float)i;
        policy.w_rq = 1.0f;
        policy.w_thermal = 0.8f + 0.1f * (float)i;
        g_cluster_policies[i] = policy;
        g_cluster_min_overrides[i] = policy.min_freq;
        g_cluster_max_overrides[i] = policy.max_freq;
    }
}

void notify_foreground_app_changed(const char* package) {
    std::lock_guard<std::shared_mutex> lock(g_mutex);
    /* 前台变化会影响"当前应用走哪个场景分类"的判断，缓存必须作废 */
    invalidate_runtime_feature_switch_cache();
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
    release_dynamic_thread_runtime_locked("foreground_app_changed", true);
    safe_strncpy(g_foreground_package, next_package, sizeof(g_foreground_package));
    refresh_active_thread_app_rule_locked(next_package);
    apply_active_scheduler_session_override_locked();
    reset_process_discovery_state_locked(true, false);
    clear_performance_boosts();
    detach_queuebuffer_uprobe_locked();
    sync_target_tgid_locked();
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
            // 【关键修复】如果被官调挂起，彻底清理我们的痕迹，防止 0.4W 的后台鬼火漏电
            std::lock_guard<std::shared_mutex> lock(g_mutex);
            detach_queuebuffer_uprobe_locked(); // 停止 eBPF 轮询
            if (!g_threads.empty()) {
                // 释放所有被我们绑过核的线程，让官调完全接管
                release_dynamic_thread_runtime_locked("paused_by_scheduler_conflict", false);
            }
            usleep(1000 * 1000); // 挂起时降低轮询频率到 1 秒
            continue;
        }
        bool runtime_work_pending = false;
        {
            std::shared_lock<std::shared_mutex> lock(g_mutex);
            bool fas_enabled =
                is_fas_enabled_for_app(g_foreground_package);
            bool thread_runtime_enabled =
                is_custom_thread_enabled_for_app(g_foreground_package) ||
                is_dynamic_thread_scheduler_enabled_for_app(g_foreground_package);
            if (should_suppress_dynamic_thread_scheduler_for_package_locked(g_foreground_package)) {
                thread_runtime_enabled = is_custom_thread_enabled_for_app(g_foreground_package);
            }
            bool queuebuffer_attached =
                g_bpf_queuebuffer_link_v1 != nullptr || g_bpf_queuebuffer_link_v2 != nullptr;
            runtime_work_pending =
                fas_enabled ||
                thread_runtime_enabled ||
                queuebuffer_attached ||
                !g_threads.empty() ||
                g_foreground_pid_refresh_pending;
        }
        if (!runtime_work_pending) {
            usleep(100 * 1000);
            continue;
        }
        {
            std::lock_guard<std::shared_mutex> lock(g_mutex);
            refresh_foreground_pid_locked(false);
            if (is_fas_enabled_for_app(g_foreground_package)) {
                refresh_queuebuffer_attachment_locked();
                sync_target_tgid_locked();
                poll_queuebuffer_ring_locked();
            } else {
                detach_queuebuffer_uprobe_locked();
            }
            bool thread_runtime_enabled =
                is_custom_thread_enabled_for_app(g_foreground_package) ||
                is_dynamic_thread_scheduler_enabled_for_app(g_foreground_package);
            if (should_suppress_dynamic_thread_scheduler_for_package_locked(g_foreground_package)) {
                thread_runtime_enabled = is_custom_thread_enabled_for_app(g_foreground_package);
            }
            if (thread_runtime_enabled) {
                sync_target_tgid_locked();
                poll_process_event_ring_locked();
                collect_foreground_threads_locked();
                pull_bpf_runtimes_locked(get_current_time_ms());
                sync_render_thread_targets_locked(false);
            } else {
                if (!g_threads.empty()) {
                    release_dynamic_thread_runtime_locked(
                        "dynamic_thread_scheduler_disabled", false);
                }
            }
        }
        usleep(100 * 1000);
    }
    return NULL;
}

void refresh_bpf_targets_now() {
    if (should_pause_our_game_scheduler()) {
        std::lock_guard<std::shared_mutex> lock(g_mutex);
        detach_queuebuffer_uprobe_locked();
        if (!g_threads.empty()) {
            release_dynamic_thread_runtime_locked("paused_by_scheduler_conflict", false);
        }
        return;
    }
    std::lock_guard<std::shared_mutex> lock(g_mutex);
    refresh_foreground_pid_locked(true);
    if (is_fas_enabled_for_app(g_foreground_package)) {
        refresh_queuebuffer_attachment_locked();
        sync_target_tgid_locked();
        poll_queuebuffer_ring_locked();
    } else {
        detach_queuebuffer_uprobe_locked();
    }
    bool thread_runtime_enabled =
        is_custom_thread_enabled_for_app(g_foreground_package) ||
        is_dynamic_thread_scheduler_enabled_for_app(g_foreground_package);
    if (should_suppress_dynamic_thread_scheduler_for_package_locked(g_foreground_package)) {
        thread_runtime_enabled = is_custom_thread_enabled_for_app(g_foreground_package);
    }
    if (thread_runtime_enabled) {
        sync_target_tgid_locked();
        poll_process_event_ring_locked();
        collect_foreground_threads_locked();
        sync_render_thread_targets_locked(true);
    } else {
        if (!g_threads.empty()) {
            release_dynamic_thread_runtime_locked("dynamic_thread_scheduler_disabled", false);
        }
    }
}

void reload_thread_config_now() {
    std::lock_guard<std::shared_mutex> lock(g_mutex);
    release_dynamic_thread_runtime_locked("thread_config_reloaded", true);
    reset_scheduler_session_locked("thread_config_reloaded");
    refresh_active_thread_app_rule_locked(g_foreground_package);
    apply_active_scheduler_session_override_locked();
    clear_performance_boosts();
    reset_process_discovery_state_locked(true, true);
    sync_target_tgid_locked();
    if (!is_fas_enabled_for_app(g_foreground_package)) {
        detach_queuebuffer_uprobe_locked();
    }
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
    /*
     * 退出前把改动过的线程还原（含 CPU 掩码）。否则停止守护进程或卸载模块后，
     * 被绑过的游戏线程会一直停在窄掩码上，直到进程重启。
     */
    {
        std::lock_guard<std::shared_mutex> lock(g_mutex);
        if (!g_threads.empty()) {
            release_dynamic_thread_runtime_locked("daemon_stopping", true);
        }
    }
    cleanup_bpf();
}

void run_scheduler_step() {
    long long now_ms = get_current_time_ms();
    refresh_scene_policy_state(g_current_foreground_app);
    /*
     * pipeline 登记的定期自愈（每 2s 一次）。
     * 线程可能在"命中 role 规则"之后又被兜底规则改绑（例如两个 UnityMain：
     * 第一个吃 main 规则绑 6-7，第二个落到 other 绑 0-5），
     * 这时旧登记的核已经不在掩码里，不能在节点上留着。
     */
    {
        static long long last_pipeline_flush_ms = 0;
        if (now_ms - last_pipeline_flush_ms >= 2000) {
            last_pipeline_flush_ms = now_ms;
            std::lock_guard<std::shared_mutex> lock(g_mutex);
            flush_pipeline_tasks_locked();
            refresh_pipeline_tid_snapshot_locked();
        }
    }
    {
        std::lock_guard<std::shared_mutex> lock(g_mutex);
        if (g_scheduler_session.pid > 0 && kill(g_scheduler_session.pid, 0) != 0) {
            reset_scheduler_session_locked("pid_gone");
        }
        /*
         * 线程唤醒紧张度是线程侧产物：动态线程与自定义线程都关闭时，只清掉标志、
         * 不触发调频 boost，避免"线程功能已关但仍在按线程信号动频率"。
         */
        bool thread_features_on =
            is_custom_thread_enabled_for_app(g_current_foreground_app) ||
            is_dynamic_thread_scheduler_enabled_for_app(g_current_foreground_app);
        bool urgent_from_threads = consume_urgent_render_flag_locked();
        if (thread_features_on && urgent_from_threads && cluster_count > 0) {
            request_performance_boost(cluster_count - 1, 180);
        }
    }

    if (should_pause_our_game_scheduler()) {
        return;
    }
    bool custom_thread_enabled =
        is_custom_thread_enabled_for_app(g_current_foreground_app);
    bool dynamic_thread_enabled =
        is_dynamic_thread_scheduler_enabled_for_app(g_current_foreground_app);
    if (should_suppress_dynamic_thread_scheduler_for_package_locked(g_current_foreground_app)) {
        dynamic_thread_enabled = false;
    }
    if (!custom_thread_enabled && !dynamic_thread_enabled) {
        std::lock_guard<std::shared_mutex> lock(g_mutex);
        clear_performance_boosts();
        if (!g_threads.empty()) {
            release_dynamic_thread_runtime_locked("thread_scheduler_disabled", false);
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

    std::vector<ClusterPolicy> cluster_policy_snapshot;
    cluster_policy_snapshot.resize(cluster_count);
    std::vector<int> min_override_snapshot(cluster_count, 0);
    std::vector<int> max_override_snapshot(cluster_count, 0);
    char foreground_package_snapshot[MAX_APP_NAME_LEN] = {0};
    bool screen_off_snapshot = false;
    bool should_apply_freq_update = false;
    bool skip_thread_assignments = false;

    {
        std::lock_guard<std::shared_mutex> lock(g_mutex);
        if (dynamic_thread_enabled) {
            apply_active_scheduler_session_override_locked();
            update_scheduler_session_locked(now_ms);
            bool force_guard_recheck = g_dynamic_affinity_recheck_pending;
            if (guard_locked_assignments_locked(now_ms, force_guard_recheck)) {
                g_dynamic_affinity_recheck_pending = false;
                skip_thread_assignments = true;
            }
            g_dynamic_affinity_recheck_pending = false;
            /* 动态模式下清除所有线程的显式规则标记 — 确权信态 */
            for (auto& kv : g_threads) {
                if (kv.second.has_explicit_affinity_rule) {
                    kv.second.has_explicit_affinity_rule = false;
                }
            }
        } else {
            /* 优化：只在状态切换时重置会话，避免每轮重复调用 */
            if (g_scheduler_session.phase != SESSION_IDLE) {
                reset_scheduler_session_locked("dynamic_thread_scheduler_disabled");
            }
            clear_performance_boosts();
        }

        if (!skip_thread_assignments) {
            /* Phase2: 定期清理不再使用的 epoch 状态 */
            cleanup_stale_selection_epochs_locked();
            for (auto& kv : g_threads) {
                ThreadRuntimeState& state = kv.second;
                const char* state_package = state.package.empty() ? g_foreground_package : state.package.c_str();
                bool custom_thread_enabled_for_state =
                    is_custom_thread_enabled_for_app(state_package);
                bool dynamic_thread_enabled_for_state =
                    is_dynamic_thread_scheduler_enabled_for_app(state_package);
                if (should_suppress_dynamic_thread_scheduler_for_package_locked(state_package)) {
                    dynamic_thread_enabled_for_state = false;
                }
                bool is_foreground_state = state.pid > 0 &&
                                           state.pid == g_foreground_pid &&
                                           state_package &&
                                           strcmp(state_package, g_foreground_package) == 0;
                ThreadJsonResolvedRule resolved_rule = {};
                const ThreadJsonResolvedRule* matched_rule = custom_thread_enabled_for_state ?
                    resolve_thread_json_rule_locked(
                        state_package, state.tid, state.comm.c_str(), state.cpu_usage_pct, state.cls, &resolved_rule) :
                    NULL;
                if (matched_rule) {
                    apply_thread_json_action_locked(state, matched_rule);
                } else {
                    release_thread_runtime_locked(state, "rule_not_matched", true, false);
                }

                std::vector<int> explicit_cpus;
                int explicit_cluster = -1;
                bool has_explicit_mask = false;
                bool allow_default_explicit_affinity =
                    !(is_foreground_state &&
                      matched_rule &&
                      dynamic_thread_enabled_for_state &&
                      (matched_rule->source_kind == THREAD_JSON_RULE_SOURCE_DEFAULT ||
                       matched_rule->source_kind == THREAD_JSON_RULE_SOURCE_SCENE_DEFAULT));
                if (matched_rule && matched_rule->action &&
                    allow_default_explicit_affinity &&
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

                /* Phase1: 标记线程是否命中显式 cpus/clusters 规则 */
                state.has_explicit_affinity_rule = has_explicit_mask && matched_rule &&
                    (matched_rule->source_kind != THREAD_JSON_RULE_SOURCE_DEFAULT &&
                     matched_rule->source_kind != THREAD_JSON_RULE_SOURCE_SCENE_DEFAULT);
                if (state.has_explicit_affinity_rule) {
                    /* 每次命中规则都刷新冷却参数，避免旧规则残留影响新规则 */
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
                            mark_pipeline_task_for_rule_locked(state.tid, matched_rule, explicit_cpus);
                            state.assigned_cluster = explicit_cluster;
                            state.last_migrate_ms = now_ms;
                            state.assigned_cpus = explicit_cpus;
                        }
                    }
                    continue;
                }

                /* ===== Phase1: 静态规则快速路径 — 跳过动态分类/迁移/冻结 ===== */
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
                                mark_pipeline_task_for_rule_locked(state.tid, matched_rule, explicit_cpus);
                                state.assigned_cluster = explicit_cluster;
                                state.last_migrate_ms = now_ms;
                                state.last_static_bind_ms = now_ms;
                                state.assigned_cpus = explicit_cpus;
                                /* static_bind_apply 日志 + trace */
                                if (now_ms - state.last_event_log_ms > 5000) {
                                    char cpu_list[96] = {0};
                                    size_t pos = 0;
                                    for (size_t i = 0; i < explicit_cpus.size() && pos < sizeof(cpu_list) - 8; i++) {
                                        int written = safe_snprintf(cpu_list + pos, sizeof(cpu_list) - pos,
                                            "%s%d", i == 0 ? "" : ",", explicit_cpus[i]);
                                        if (written <= 0) break;
                                        pos += (size_t)written;
                                    }
                                    char msg[256];
                                    safe_snprintf(msg, sizeof(msg),
                                        "静态规则绑核 | app:%s | tid:%d | comm:%s | cpus:%s",
                                        state_package ? state_package : "unknown",
                                        (int)state.tid, state.comm.c_str(),
                                        cpu_list[0] ? cpu_list : "-");
                                    log_affinity_message(msg);
                                    trace_instantf("murong_static_rule", "bind_apply app=%s tid=%d comm=%s cpus=%s",
                                        state_package ? state_package : "unknown",
                                        (int)state.tid, state.comm.c_str(),
                                        cpu_list[0] ? cpu_list : "-");
                                    state.last_event_log_ms = now_ms;
                                }
                            }
                        } else {
                            /* static_bind_skip_hold — 冷却期内保持现有绑定 */
                            if (now_ms - state.last_event_log_ms > 10000) {
                                char msg[192];
                                safe_snprintf(msg, sizeof(msg),
                                    "静态规则保持(冷却) | app:%s | tid:%d | comm:%s | remain:%lldms",
                                    state_package ? state_package : "unknown",
                                    (int)state.tid, state.comm.c_str(),
                                    (long long)(state.static_bind_cooldown_ms - (now_ms - state.last_static_bind_ms)));
                                log_scheduler_probe_once(msg);
                                trace_instantf("murong_static_rule", "skip_hold app=%s tid=%d comm=%s",
                                    state_package ? state_package : "unknown",
                                    (int)state.tid, state.comm.c_str());
                                state.last_event_log_ms = now_ms;
                            }
                        }
                    }
                    /* 显式规则线程跳过后续动态分类/迁移/冻结逻辑 */
                    continue;
                }

                if (is_foreground_state && g_scheduler_session.phase == SESSION_LOCKED) {
                    LockedClassAssignment locked_assignment = {};
                    if (resolve_locked_assignment_for_state_locked(state, &locked_assignment)) {
                        state.frozen = false;
                        state.frozen_cpus.clear();
                        state.frozen_cluster = -1;
                        state.frozen_at_ms = 0;
                        state.target_stable_since_ms = 0;
                        state.last_target_cluster = -1;
                        state.last_target_cpus.clear();
                        state.first_hot_ms = 0;
                        state.hot_sample_count = 0;

                        bool actual_assignment_match =
                            is_thread_assignment_matching_locked(state.tid, locked_assignment);
                        bool target_changed =
                            state.assigned_cluster != locked_assignment.cluster ||
                            !cpu_mask_equals(state.assigned_cpus, locked_assignment.cpus);
                        if ((target_changed || !actual_assignment_match) &&
                            (!actual_assignment_match ||
                             state.last_migrate_ms == 0 ||
                             now_ms - state.last_migrate_ms >= kRebindCooldownMs)) {
                            bool applied = !locked_assignment.cpus.empty() ?
                                set_thread_affinity_mask(state.tid, locked_assignment.cpus) :
                                set_thread_affinity(state.tid, locked_assignment.cluster);
                            if (applied) {
                                state.assigned_cluster = locked_assignment.cluster;
                                state.assigned_cpus = locked_assignment.cpus;
                                state.last_migrate_ms = now_ms;
                            }
                        }
                        if (is_thread_assignment_matching_locked(state.tid, locked_assignment)) {
                            maybe_latch_primary_thread_tid_locked(state);
                        }
                        continue;
                    }
                }

                if (state.frozen) {
                    continue;
                }
                if (now_ms - state.last_classify_ms >= 500) {
                    state.cls = classify_thread(state.tid, state.comm.c_str(), state.cpu_usage_pct);
                    state.last_classify_ms = now_ms;
                }
                state.cls = apply_latched_primary_role_override_locked(state, state.cls);
                bool hot_priority = is_hot_priority_thread_locked(state.tid, state.comm.c_str(), state.cpu_usage_pct);
                bool priority_name = is_render_thread_name(state.comm.c_str()) ||
                                     is_main_thread_name_for_package(g_foreground_package, state.comm.c_str(), state.tid);
                bool latched_primary_tid = is_latched_primary_tid_locked(state.tid);
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
                if (priority_name && !hot_priority && !latched_primary_tid) {
                    state.cls = THREAD_CLASS_BACKGROUND;
                }
                int target_cluster = select_target_cluster(state.cls, cluster_scores);
                /* Phase2: 主线程/渲染线程防漂移 */
                if (state.cls == THREAD_CLASS_MAIN && state.assigned_cluster >= 0 &&
                    state.assigned_cluster != target_cluster &&
                    state.last_migrate_ms > 0 && now_ms - state.last_migrate_ms < 5000) {
                    /* 主线程禁止因短时热度波动跨 cluster 换绑 */
                    target_cluster = state.assigned_cluster;
                } else if (state.cls == THREAD_CLASS_RENDER && state.assigned_cluster >= 0 &&
                           state.assigned_cluster != target_cluster) {
                    /* 渲染线程禁止跨 cluster 漂移 */
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
                LockedClassAssignment target_assignment = {};
                target_assignment.valid = true;
                target_assignment.cluster = target_cluster;
                target_assignment.cpus = has_explicit_mask ? explicit_cpus : std::vector<int>();
                bool actual_assignment_match =
                    is_thread_assignment_matching_locked(state.tid, target_assignment);
                if (has_explicit_mask) {
                    need_apply = (state.assigned_cluster != target_cluster) ||
                                 !cpu_mask_equals(state.assigned_cpus, explicit_cpus) ||
                                 !actual_assignment_match;
                } else {
                    need_apply = (state.assigned_cluster != target_cluster) ||
                                 !state.assigned_cpus.empty() ||
                                 !actual_assignment_match;
                }

                if (need_apply &&
                    !thread_affinity_backoff_active_locked(state, now_ms) &&
                    (!actual_assignment_match ||
                     state.last_migrate_ms == 0 ||
                     now_ms - state.last_migrate_ms >= 500)) {
                    bool applied = has_explicit_mask ?
                        set_thread_affinity_mask(state.tid, explicit_cpus) :
                        set_thread_affinity(state.tid, target_cluster);
                    if (applied) {
                        state.assigned_cluster = target_cluster;
                        state.last_migrate_ms = now_ms;
                        state.assigned_cpus = has_explicit_mask ? explicit_cpus : std::vector<int>();
                    }
                    /*
                     * 回读确认是否真的收敛。受 cpuset 限制的线程写成功但永远不
                     * 收敛，这里记账，连续多次就退避，避免每 500ms 重写一次。
                     */
                    thread_affinity_note_result_locked(
                        state,
                        applied && is_thread_assignment_matching_locked(state.tid, target_assignment),
                        now_ms);
                }

                bool assignment_live = is_thread_assignment_matching_locked(state.tid, target_assignment);
                if (assignment_live) {
                    thread_affinity_note_result_locked(state, true, now_ms);
                }
                if (assignment_live &&
                    (state.cls == THREAD_CLASS_MAIN || state.cls == THREAD_CLASS_RENDER)) {
                    maybe_latch_primary_thread_tid_locked(state);
                }
            }
        }

        safe_strncpy(foreground_package_snapshot, g_foreground_package, sizeof(foreground_package_snapshot));
        screen_off_snapshot = g_scene_policy.screen_off;
        bool any_dyn_enabled = false;
        for (int j = 0; j < cluster_count; j++) {
            cluster_policy_snapshot[(size_t)j] = g_cluster_policies[j];
            if (g_cluster_dyn_params[j].enabled) {
                any_dyn_enabled = true;
            }
        }
        should_apply_freq_update = !any_dyn_enabled && !is_fas_enabled_for_app(g_foreground_package);
    }

    for (int i = 0; i < cluster_count; i++) {
        int min_override = cluster_policy_snapshot[(size_t)i].min_freq;
        int policy_num = cpu_clusters[i].policies[0];
        int policy_idx = -1;
        for (int k = 0; k < policy_count; k++) {
            if (cpu_policies[k].policy_num == policy_num) {
                policy_idx = k;
                break;
            }
        }
        /*
         * 游戏里下限用硬件最低档：档位 min_freq 是"日常保底"，游戏里它变成硬地板——
         * 实测王者荣耀跑在 balance 档（c1 min 1017600）时 prime 簇被顶在 1017MHz，
         * 同帧率下比官方（游戏配置的 scaling_min = 频率表第 0 档）多烧 ~0.6W。
         * 官方怎么做我们就怎么做：空闲瞬间让它自己掉下去，突发再让 governor 爬。
         */
        if (is_game_package_name(foreground_package_snapshot)) {
            int hw_low = get_policy_lowest_available_freq(policy_idx);
            if (hw_low > 0 && hw_low < min_override) {
                min_override = hw_low;
            }
        }
        int max_override = cluster_policy_snapshot[(size_t)i].max_freq;
        if (screen_off_snapshot) {
            int floor = get_policy_lowest_available_freq(policy_idx);
            min_override = floor;
            max_override = floor;
        } else {
            int scene_floor = get_scene_floor_freq_for_cluster(policy_idx, i, foreground_package_snapshot);
            if (scene_floor > min_override) {
                min_override = scene_floor;
            }
        }
        if (max_override < min_override) max_override = min_override;
        min_override_snapshot[(size_t)i] = min_override;
        max_override_snapshot[(size_t)i] = max_override;
        if (should_apply_freq_update) {
            update_cluster_freq(i, min_override, max_override);
        }
    }

    {
        std::lock_guard<std::shared_mutex> lock(g_mutex);
        if (strcmp(g_foreground_package, foreground_package_snapshot) == 0 &&
            g_scene_policy.screen_off == screen_off_snapshot) {
            for (int i = 0; i < cluster_count; i++) {
                g_cluster_min_overrides[i] = min_override_snapshot[(size_t)i];
                g_cluster_max_overrides[i] = max_override_snapshot[(size_t)i];
            }
        }
    }

    /* Phase1: 记录显式规则对动态层的压制日志（防抖 30s） */
    if (!dynamic_thread_enabled && custom_thread_enabled &&
        g_foreground_explicit_thread_rule_active &&
        g_current_foreground_app[0]) {
        if (now_ms - g_last_dynamic_suppressed_log_ms > 30000 &&
            g_active_thread_app_rule_index >= 0) {
            char msg[256];
            safe_snprintf(msg, sizeof(msg),
                "动态层被显式规则压制 | app:%s | 规则索引:%d",
                g_current_foreground_app,
                g_active_thread_app_rule_index);
            log_affinity_message(msg);
            trace_instantf("murong_session", "dynamic_suppressed_by_explicit_rule app=%s",
                g_current_foreground_app);
            g_last_dynamic_suppressed_log_ms = now_ms;
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
