int initialize_application() {
    // 备份并清理日志
    backup_and_clear_log();
    cleanup_old_logs();

    log_message("启动调度服务");

    // 检查root权限
    if (getuid() != 0) {
        log_message("错误：必须以root权限运行");
        return 1;
    }

    // 检测CPU型号
    if (!detect_cpu_model()) {
        log_message("无法检测CPU型号，使用默认配置");
        safe_strncpy(cpu_model, "default", sizeof(cpu_model));
    }

    // 设置平台类型
    if (strncmp(cpu_model, "SM", 2) == 0) {
        current_platform = PLATFORM_QUALCOMM;
    } else if (strncmp(cpu_model, "MT", 2) == 0) {
        current_platform = PLATFORM_MEDIATEK;
    } else {
        current_platform = PLATFORM_UNKNOWN;
    }

    char log_msg[128];
    safe_snprintf(log_msg, sizeof(log_msg), "检测到CPU型号: %s, 平台类型: %d",
        cpu_model, current_platform);
    log_message(log_msg);

    // 重新挂载/sys为读写模式
    if (!remount_sysfs_rw()) {
        log_message("警告：无法重新挂载/sys，部分功能可能受限");
    }

    // 检测CPU集群
    detect_cpu_clusters();

    // 初始化调度器信息
    init_scheduler_info();

    initialize_gpu_ddr_control_state();

    // 新引擎负责算法配置；运行时 gating/profile 仍依赖 g_fas_config。
    g_fas_engine_inited = fas_engine_init(&g_fas_engine,
        "/data/adb/modules/muronggameopt/bin/cpu/fas.json");
    load_fas_config();

    // 加载线程签名扩展配置
    load_thread_signature_config();

    // 加载场景分类配置
    load_scene_categories_config();

    // 加载按真实进程检测的全局调度排除名单。
    load_scheduler_skip_processes_config();
    refresh_scheduler_skip_process_state();

    // 加载初始模式配置
    FILE* mode_file = fopen(MODE_FILE, "r");
    if (mode_file) {
        char line[256];
        if (fgets(line, sizeof(line), mode_file)) {
            line[strcspn(line, "\n")] = 0;
            safe_strncpy(g_current_mode, line, sizeof(g_current_mode));
        }
        fclose(mode_file);
    }
    load_mode_settings_json(g_current_mode);
    UnifiedScheduler::initialize_cluster_policies();
    prime_config_reload_watch_hashes();

    for (int i = 0; i < policy_count && i < MAX_POLICIES; i++) {
        freq_table_load(cpu_policies[i].policy_num, &g_freq_tables[i]);

        cpu_policies[i].hw_min_freq = cpu_policies[i].min_freq;
        if (g_freq_tables[i].count > 0) {
            cpu_policies[i].hw_min_freq = g_freq_tables[i].freqs[0];
            for (int f = 1; f < g_freq_tables[i].count; f++) {
                if (g_freq_tables[i].freqs[f] < cpu_policies[i].hw_min_freq) {
                    cpu_policies[i].hw_min_freq = g_freq_tables[i].freqs[f];
                }
            }
        }
    }

    return 0;
}

void handle_signal(int sig) {
    g_keep_running = 0;
}

/*
 * 纯线程版不编这一整段 FAS 帧信号管线：
 *   get_fas_reference_freq_khz()      —— 普通版里也无人调用（历史死函数）
 *   sync_fas_engine_queuebuffer_samples() —— 依赖 FAS 内部的 surface 状态
 *   fas_prediction_thread()           —— FAS 预测线程本体
 * 它们只被 FAS 线程自身使用，去掉后线程侧行为不受影响。
 */
#ifndef MURONG_THREAD_ONLY
static int get_fas_reference_freq_khz() {
    int max_cur_freq = 0;
    for (int i = 0; i < cluster_count; i++) {
        int policy_num = cpu_clusters[i].policies[0];
        char path[256];
        safe_snprintf(path, sizeof(path),
            "/sys/devices/system/cpu/cpufreq/policy%d/scaling_cur_freq",
            policy_num);
        int cur_freq = UnifiedScheduler::read_int_from_file(path, 0);
        if (cur_freq > max_cur_freq) {
            max_cur_freq = cur_freq;
        }
    }
    if (max_cur_freq > 0) {
        return max_cur_freq;
    }

    for (int i = cluster_count - 1; i >= 0; i--) {
        if (g_virtual_freq[i] > 0) {
            return g_virtual_freq[i];
        }
    }
    return 0;
}

static int sync_fas_engine_queuebuffer_samples(FasEngine* engine,
    const char* package,
    uint64_t* last_seen_accepted_count,
    uint64_t* last_surface_key)
{
    if (!engine || !package || !package[0] || !last_seen_accepted_count || !last_surface_key) return 0;
    std::lock_guard<std::mutex> lock(g_frame_sample_mutex);
    if (strcmp(package, g_queuebuffer_package) != 0) return 0;
    QueueBufferSurfaceState* primary = select_primary_queuebuffer_surface_locked();
    if (!primary) return 0;
    if (primary->last_wall_ms <= 0) return 0;
    long long now_ms = get_current_time_ms();
    if (now_ms - primary->last_wall_ms > 2000) return 0;

    /*
     * 主 surface 换了就从当前累计数开始接，不把上一块 surface 的历史帧灌进引擎。
     * 旧实现只记"推到哪里"，切换 surface 时索引会对不上，产生整段偏差帧。
     */
    if (primary->surface_key != *last_surface_key) {
        *last_surface_key = primary->surface_key;
        *last_seen_accepted_count = primary->accepted_count;
        return 0;
    }

    uint64_t total = primary->accepted_count;
    size_t size = primary->frametimes_ms.size();
    if (total == 0 || size == 0) return 0;

    uint64_t first_index = total > (uint64_t)size ? total - (uint64_t)size : 0;
    if (*last_seen_accepted_count < first_index) {
        *last_seen_accepted_count = first_index;
    }
    if (*last_seen_accepted_count > total) {
        *last_seen_accepted_count = total;
    }

    int pushed = 0;
    for (uint64_t sample_index = *last_seen_accepted_count; sample_index < total; sample_index++) {
        size_t vector_index = (size_t)(sample_index - first_index);
        if (vector_index >= size) continue;
        double frame_ms = primary->frametimes_ms[vector_index];
        fas_engine_push_frame(engine, (long long)(frame_ms * 1000000.0));
        pushed++;
    }
    *last_seen_accepted_count = total;
    return pushed;
}

// FAS 预测线程
void* fas_prediction_thread(void*) {
    char last_engine_app[MAX_APP_NAME_LEN] = "";
    uint64_t last_queuebuffer_accepted_count = 0;
    uint64_t last_fas_surface_key = 0;
    while (g_keep_running) {
        if (should_pause_our_game_scheduler()) {
            last_engine_app[0] = '\0';
            last_queuebuffer_accepted_count = 0;
            last_fas_surface_key = 0;
            usleep(1000 * 1000);
            continue;
        }
        char app[MAX_APP_NAME_LEN];
        pthread_mutex_lock(&g_app_mutex);
        safe_strncpy(app, g_current_foreground_app, sizeof(app));
        pthread_mutex_unlock(&g_app_mutex);
        if (g_fas_engine_inited && app[0]) {
            if (strcmp(app, last_engine_app) != 0) {
                fas_engine_on_app_change(&g_fas_engine, app, -1);
                safe_strncpy(last_engine_app, app, sizeof(last_engine_app));
                last_queuebuffer_accepted_count = 0;
                last_fas_surface_key = 0;
            }

            /*
             * FAS 只吃帧信号：不为决策扫 /proc、不读 GPU/DDR/线程占用。
             * 帧间隔由 queueBuffer uprobe 推入，tick 只把帧债换算成动作档位。
             */
            /* 动作表参数来自 dyn.json（动态调频配置），热重载后这里会同步给引擎 */
            fas_engine_set_ladder(&g_fas_engine, &g_dyn_control);
            sync_fas_engine_queuebuffer_samples(
                &g_fas_engine, app, &last_queuebuffer_accepted_count, &last_fas_surface_key);
            FasOutput o = fas_engine_tick(&g_fas_engine);
            g_cached_fas_guard_signal.store(o.guard_signal);
            g_cached_fas_target_fps.store(o.target_fps);
            g_cached_fas_current_fps.store(o.current_fps);
            g_cached_fas_valid_samples.store(o.valid_samples);
            g_cached_fas_buffer_usable.store(o.buffer_state);
            g_cached_fas_frame_us.store(o.frame_us);
            g_cached_fas_avg_frame_us.store(o.avg_frame_us);
            g_cached_fas_budget_us.store(o.budget_us);
            g_cached_fas_debt_us.store(o.debt_us);
            g_cached_fas_severity_us.store(o.severity_us);
            g_cached_fas_action_level.store(o.action_level);
            g_cached_fas_over_budget_streak.store(o.over_budget_streak);
            g_cached_fas_guard_active.store(o.guard_signal > 0 ? 1 : 0);
            g_cached_fas_guard_reason.store(o.guard_signal);
            g_fas_last_good_sample_ms = get_current_time_ms();
        }
        int tf = g_cached_fas_target_fps.load();
        usleep(tf >= 30 ? std::min(std::max(1000000/tf*5, 30000), 100000) : 100000);
    }
    return NULL;
}
#endif /* !MURONG_THREAD_ONLY —— FAS 帧信号管线结束 */

void run_main_loop() {
    // 初始化inotify
    int inotify_fd = inotify_init();
    if (inotify_fd < 0) {
        log_message("inotify初始化失败");
        return;
    }

#ifndef MURONG_THREAD_ONLY
    pthread_t fas_tid;
    pthread_create(&fas_tid, NULL, fas_prediction_thread, NULL);
#endif
    UnifiedScheduler::start_threads();

    // 监控配置文件目录变化（只认写入完成，不认 IN_MODIFY 防止半写即加载）
    int wd_general_config_dir = inotify_add_watch(inotify_fd, CONFIG_DIR, IN_MOVED_TO | IN_CLOSE_WRITE);
    int wd_cpu_config_dir = inotify_add_watch(inotify_fd, CPU_CONFIG_DIR, IN_MOVED_TO | IN_CLOSE_WRITE);

    char last_app[MAX_APP_NAME_LEN] = "";
    long long last_app_check_ms = 0;

    while (g_keep_running) {
        g_runtime_stats.loop_ticks.fetch_add(1);
        fd_set fds;
        struct timeval timeout;

        FD_ZERO(&fds);
        FD_SET(inotify_fd, &fds);

        timeout.tv_sec = 0;
        /*
         * 30ms：频率区间要跟着负载走，而平台（perfservice / thermal / msm_performance）
         * 会往同一对 scaling_min/max_freq 里写；核对得比它快，区间才守得住。
         * 重活（场景策略、详细日志）各自有 500ms/1s 节流，这里只是多几次文件读。
         */
        timeout.tv_usec = 30000;

        int ret = select(inotify_fd + 1, &fds, NULL, NULL, &timeout);

        // 处理inotify事件
        if (ret > 0 && inotify_fd >= 0 && FD_ISSET(inotify_fd, &fds)) {
            handle_inotify_events(inotify_fd, wd_general_config_dir, wd_cpu_config_dir);
        }
        process_pending_config_reloads();

        long long now_ms = get_current_time_ms();
        if (now_ms - last_app_check_ms >= APP_CHECK_INTERVAL_MS) {
            last_app_check_ms = now_ms;

            const bool skip_process_state_changed =
                refresh_scheduler_skip_process_state();
            if (skip_process_state_changed) {
                handle_scheduler_skip_process_transition();
            }

            // 处理前台应用检测
            char current_app[MAX_APP_NAME_LEN] = {0};
            bool current_app_ok = get_foreground_app(current_app, sizeof(current_app));

            // 检查内存分配是否成功
            if (!current_app_ok) {
                /* 前台识别失败会按检查周期反复发生，主日志只留限流后的摘要。 */
                log_message_throttled("获取前台应用失败", 60000,
                                      "获取前台应用失败，跳过本次检查");
            } else {
                // 过滤无效应用名称（长度过短、包含空格等）
                if (strlen(current_app) >= 3 && strchr(current_app, ' ') == NULL &&
                    strcmp(current_app, "com.omarea.vtools") != 0 &&
                    strcmp(current_app, "NotificationShade") != 0 &&
                    strcmp(current_app, "com.android.systemui") != 0) {
                    pthread_mutex_lock(&g_app_mutex);
                    safe_strncpy(g_current_foreground_app, current_app, sizeof(g_current_foreground_app));
                    pthread_mutex_unlock(&g_app_mutex);

                    // 处理前台应用
                    process_foreground_app(current_app, last_app);
                }
            }
        }

        // --- 动态调频处理 ---
        pthread_mutex_lock(&g_app_mutex);
        char current_app_buf[MAX_APP_NAME_LEN];
        safe_strncpy(current_app_buf, g_current_foreground_app, sizeof(current_app_buf));
        pthread_mutex_unlock(&g_app_mutex);

        handle_dynamic_tuning(current_app_buf);
        enforce_gpu_ddr_limits(false);
        UnifiedScheduler::run_scheduler_step();
        emit_runtime_stats_snapshot(current_app_buf, get_current_time_ms());
        // --- 动态调频处理结束 ---
    }

#ifndef MURONG_THREAD_ONLY
    pthread_join(fas_tid, NULL);
#endif
    UnifiedScheduler::stop_threads();

    if (inotify_fd >= 0) {
        close(inotify_fd);
    }
}

int main() {
    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    if (!g_keep_running) {
        return 0;
    }

    // 初始化应用
    if (initialize_application() != 0) {
        return 1;
    }

    /*
     * 开机时动态线程与自定义线程都关着，就不做任何线程调度工作：
     * 进程照常运行（动态调频与 FAS 仍然需要它），但线程侧完全空闲。
     * 这条只打一次日志，方便确认设备确实处于"无线程调度"状态。
     */
    {
        RuntimeFeatureSwitches boot_switches =
            normalize_runtime_feature_switches(g_mode_runtime_features);
        if (!boot_switches.custom_thread_enabled &&
            !boot_switches.dynamic_thread_scheduler_enabled) {
            log_message("线程调度：关闭（动态线程与自定义线程均为关闭，进程继续运行动态调频与 FAS）");
        }
    }

    // 将调度器守护进程自身及所有后续派生的子线程绑定到特定核心
    // 避免抢占游戏的主力渲染核心（如超大核），同时也要避免挤占处理中断的 CPU0
    if (cluster_count > 0 && cpu_clusters[0].cpu_count > 1) {
        cpu_set_t mask;
        CPU_ZERO(&mask);
        // 绑定到 cluster 0 的第 2 到第 4 个核心（例如 CPU1, CPU2, CPU3）
        int cores_to_bind = std::min(3, cpu_clusters[0].cpu_count - 1);
        for (int i = 1; i <= cores_to_bind; i++) {
            int cpu = cpu_clusters[0].cpu_ids[i];
            if (cpu >= 0 && cpu < CPU_SETSIZE) {
                CPU_SET(cpu, &mask);
            }
        }
        if (sched_setaffinity(0, sizeof(mask), &mask) != 0) {
            log_message("警告：无法将守护进程绑定到指定核心");
        } else {
            log_message("守护进程已成功绑定到后台核心，防止抢占游戏算力");
        }
    }

    // 运行主循环
    run_main_loop();

    return 0;
}
