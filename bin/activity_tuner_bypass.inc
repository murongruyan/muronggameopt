// 第三方版不再根据风驰/SCX/HMBIRD governor 自避让。
// 风驰冲突由 fengchi_cleanup.sh 通过 sqlite3 循环清空配置表解决。

static void sync_official_tuner_for_game(const char* current_app) {
    (void)current_app;
    g_official_tuner_mode = OFFICIAL_TUNER_NONE;
    g_official_tuner_disabled = 0;
    g_official_tuner_managed_by_module = 0;
    g_official_tuner_restore_mask = OFFICIAL_TUNER_RESTORE_NONE;
    g_self_bypass_by_official_tuner.store(0);
}

static bool is_official_scheduler_governor_active() {
    return false;
}

static void update_official_game_interference_state(const char* current_app) {
    (void)current_app;
    g_pause_game_scheduler.store(0);
    g_self_bypass_by_official_tuner.store(0);
    g_scene_policy.official_conflict_active = 0;
}

static bool should_pause_our_game_scheduler() {
    return is_scheduler_skip_process_active();
}

static bool is_self_bypass_by_official_tuner_active() {
    return false;
}
