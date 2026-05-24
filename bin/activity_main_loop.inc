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

    // 设置策略文件权限
    set_policy_file_permissions();

    // 加载线程签名扩展配置
    load_thread_signature_config();

    // 加载场景分类配置
    load_scene_categories_config();

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

void run_main_loop() {
    // 初始化inotify
    int inotify_fd = inotify_init();
    if (inotify_fd < 0) {
        log_message("inotify初始化失败");
        return;
    }

    UnifiedScheduler::start_threads();

    // 监控配置文件目录变化，只在写入完成后再进入热重载处理。
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
        timeout.tv_usec = 100000; // 100ms

        int ret = select(inotify_fd + 1, &fds, NULL, NULL, &timeout);

        // 处理inotify事件
        if (ret > 0 && inotify_fd >= 0 && FD_ISSET(inotify_fd, &fds)) {
            handle_inotify_events(inotify_fd, wd_general_config_dir, wd_cpu_config_dir);
        }
        process_pending_config_reloads();

        long long now_ms = get_current_time_ms();
        if (now_ms - last_app_check_ms >= APP_CHECK_INTERVAL_MS) {
            last_app_check_ms = now_ms;

            // 处理前台应用检测
            char* current_app = get_foreground_app();

            // 检查内存分配是否成功
            if (!current_app) {
                log_message("获取前台应用失败，跳过本次检查");
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
                free(current_app);
            }
        }

        pthread_mutex_lock(&g_app_mutex);
        char current_app_buf[MAX_APP_NAME_LEN];
        safe_strncpy(current_app_buf, g_current_foreground_app, sizeof(current_app_buf));
        pthread_mutex_unlock(&g_app_mutex);

        UnifiedScheduler::run_scheduler_step();
        emit_runtime_stats_snapshot(current_app_buf, get_current_time_ms());
    }

    UnifiedScheduler::stop_threads();

    if (inotify_fd >= 0) {
        close(inotify_fd);
    }
}

int main() {
    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    // 初始化应用
    if (initialize_application() != 0) {
        return 1;
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
