#ifndef ACTIVITY_DYN_CONFIG_THREAD_INC
#define ACTIVITY_DYN_CONFIG_THREAD_INC

/*
 * activity_dyn_config_thread.inc —— 纯线程版（-DMURONG_THREAD_ONLY）的动态调频桩。
 *
 * 只在纯线程版构建里 include，替代 activity_dyn_config.inc。
 * 它不解析 dyn.json、不建走廊，因此不编译 activity_dynamic_tuning.inc。
 *
 * 关键：动态调频必须显式关闭。
 *   activity_scheduler_runtime.inc:688
 *     should_apply_freq_update = !any_dyn_enabled && !is_fas_enabled_for_app(...);
 *   纯线程版 dyn 关 + FAS 关之后，该式为 true，于是 <SoC>.json 的 min_freq/max_freq
 *   走廊会由 apply_settings() 真正写进 scaling_min/max —— 这正是纯线程版要的频率行为。
 *   （若不显式关闭，reset_dyn_params_to_defaults() 的默认值是 enabled = true，
 *     动态调频会用内置默认参数继续跑，达不到「纯线程」。）
 */

/*
 * 注意：g_dyn_params 的定义在 activity_diaodu.cpp（:187，初值 {true}），
 * 这里不要重复定义；线程版靠下面的 load_dyn_config_json() 把它置为关闭。
 */
#define DYN_CONFIG_FILE CPU_CONFIG_DIR "dyn.json"

static FasLadderConfig g_dyn_control = {30, 100, 300, 2, 250, 60};

/* 模式切换时的负载入口：纯线程版为空操作 */
static void load_dyn_config_json(const char* mode) {
    (void)mode;
    g_dyn_params.enabled = false;
}

/* 原 activity_dynamic_tuning.inc 的对外入口，主循环会调用：空操作 */
static void handle_dynamic_tuning(const char* current_app) {
    (void)current_app;
}

#endif /* ACTIVITY_DYN_CONFIG_THREAD_INC */
