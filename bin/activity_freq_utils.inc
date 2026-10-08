#ifndef ACTIVITY_FREQ_UTILS_INC
#define ACTIVITY_FREQ_UTILS_INC

/*
 * activity_freq_utils.inc —— 频率档位工具 + 场景策略刷新（两个版本共用）。
 *
 * 这几个函数原先住在 activity_dynamic_tuning.inc 里，但它们的调用者
 * （activity_scheduler_runtime.inc / activity_scene_policy.inc / activity_main_loop.inc）
 * 在纯线程版里依然存在，而 activity_dynamic_tuning.inc 在纯线程版被条件排除。
 * 因此把它们抽到这里共用，避免线程版丢失场景策略刷新能力。
 *
 * 依赖：g_freq_tables / cpu_policies / policy_count（调用方已定义）。
 */

static int round_freq_down_to_table(int policy_idx, int target_freq) {
    if (policy_idx < 0 || policy_idx >= policy_count || g_freq_tables[policy_idx].count <= 0) return target_freq;
    int closest = g_freq_tables[policy_idx].freqs[0]; bool found = false;
    for (int f = 0; f < g_freq_tables[policy_idx].count; f++) {
        int v = g_freq_tables[policy_idx].freqs[f];
        if (v <= target_freq) { closest = v; found = true; } else break;
    }
    return found ? closest : g_freq_tables[policy_idx].freqs[0];
}

static int get_policy_lowest_available_freq(int policy_idx) {
    if (policy_idx < 0 || policy_idx >= policy_count) return 0;
    int m = cpu_policies[policy_idx].hw_min_freq > 0 ? cpu_policies[policy_idx].hw_min_freq : cpu_policies[policy_idx].min_freq;
    if (g_freq_tables[policy_idx].count > 0) {
        m = g_freq_tables[policy_idx].freqs[0];
        for (int i = 1; i < g_freq_tables[policy_idx].count; i++)
            if (g_freq_tables[policy_idx].freqs[i] < m) m = g_freq_tables[policy_idx].freqs[i];
    }
    return m;
}

static int get_policy_safe_floor_freq(int policy_idx, int khz_floor) {
    if (policy_idx < 0 || policy_idx >= policy_count) return khz_floor;
    int best = get_policy_lowest_available_freq(policy_idx);
    if (g_freq_tables[policy_idx].count > 0) {
        for (int i = 0; i < g_freq_tables[policy_idx].count; i++)
            if (g_freq_tables[policy_idx].freqs[i] >= khz_floor) { best = g_freq_tables[policy_idx].freqs[i]; break; }
    } else if (best < khz_floor) {
        best = khz_floor;
    }
    return best;
}

static void refresh_scene_policy_state(const char* current_app) {
    long long now = get_current_time_ms();
    if (now - g_scene_policy.last_update_ms < 500) return;
    g_scene_policy.last_update_ms = now;
    update_official_game_interference_state(current_app);
#ifndef MURONG_THREAD_ONLY
    /*
     * FAS 在游戏中有帧信号时，只刷新 screen_off（帧优先，不做相机/音乐/加载判断）。
     * 纯线程版没有 FAS，恒走完整刷新分支。
     */
    if (g_cached_fas_buffer_usable.load() > 0) { g_scene_policy.screen_off = is_screen_off() ? 1 : 0; return; }
#endif
    g_scene_policy.screen_off = is_screen_off() ? 1 : 0;
    g_scene_policy.background_music = is_background_music_playing() ? 1 : 0;
    g_scene_policy.camera_active = is_camera_active() ? 1 : 0;
    g_scene_policy.game_loading = is_game_loading() ? 1 : 0;
}

#endif /* ACTIVITY_FREQ_UTILS_INC */
