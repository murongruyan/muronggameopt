#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <algorithm>
#include <string>



static bool is_detected_official_tuner_active(int mode) {
    switch (mode) {
    case OFFICIAL_TUNER_XIAOMI:
        return process_exists_by_package("com.xiaomi.joyose");
    case OFFICIAL_TUNER_VIVO:
        return process_exists_by_package("com.vivo.gamewatch");
    case OFFICIAL_TUNER_OPPO:
        // Package existence alone is not enough here. Users may stop the
        // service by script while the package still exists on the system.
        return process_exists_by_name("gameopt_hal_service-1-0");
    default:
        return false;
    }
}

static void sync_official_tuner_for_game(const char* current_app) {
    bool is_game = current_app && current_app[0] && strcmp(current_app, "unknown") != 0 && is_game_package_name(current_app);
    if (is_game) {
        if (is_official_scheduler_governor_active()) {
            return;
        }
        if (!g_official_tuner_disabled) {
            int mode = detect_official_tuner_mode();
            if (mode != OFFICIAL_TUNER_NONE && is_detected_official_tuner_active(mode)) {
                disable_official_tuner_for_game(mode);
                g_official_tuner_mode = mode;
                g_official_tuner_disabled = 1;
                g_official_tuner_managed_by_module = 1;
                log_message("检测到官方游戏调度，已在游戏前台临时禁用");
            }
        }
    } else if (g_official_tuner_disabled && g_official_tuner_managed_by_module) {
        restore_official_tuner_after_game(g_official_tuner_mode);
        g_official_tuner_mode = OFFICIAL_TUNER_NONE;
        g_official_tuner_disabled = 0;
        g_official_tuner_managed_by_module = 0;
        log_message("已退出游戏前台，恢复官方游戏调度");
    } else if (!is_game && !g_official_tuner_disabled) {
        g_official_tuner_mode = OFFICIAL_TUNER_NONE;
        g_official_tuner_managed_by_module = 0;
    }
}

static bool is_official_scheduler_governor_active() {
    for (int i = 0; i < policy_count; i++) {
        char current_gov[64] = {0};
        if (!read_current_governor(cpu_policies[i].policy_num, current_gov, sizeof(current_gov))) {
            continue;
        }
        std::string lower = current_gov;
        std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return (char)tolower(c); });
        bool scx_like = (lower == "scx" || lower.rfind("scx_", 0) == 0 || lower.rfind("scx-", 0) == 0);
        bool hybrid_like = (lower == "hmbird" || lower == "hmbrid" || lower == "hybrid");
        if (scx_like || hybrid_like) {
            return true;
        }
    }
    return false;
}

static void update_official_game_interference_state(const char* current_app) {
    static long long last_check_ms = 0;
    static int last_state = 0;
    static char last_logged_game_app[MAX_APP_NAME_LEN] = {0};
    static char last_game_app[MAX_APP_NAME_LEN] = {0};
    static long long game_foreground_since_ms = 0;
    long long now_ms = get_current_time_ms();
    if (now_ms - last_check_ms < 1000) return;
    last_check_ms = now_ms;

    bool is_game = current_app && current_app[0] && strcmp(current_app, "unknown") != 0 && is_game_package_name(current_app);
    if (is_game) {
        if (strcmp(last_game_app, current_app) != 0) {
            safe_strncpy(last_game_app, current_app, sizeof(last_game_app));
            game_foreground_since_ms = now_ms;
        }
    } else {
        last_game_app[0] = '\0';
        game_foreground_since_ms = 0;
    }

    sync_official_tuner_for_game(current_app);

    int paused = 0;
    bool in_grace_period = is_game && game_foreground_since_ms > 0 && (now_ms - game_foreground_since_ms < 2000);
    if (is_game) {
        if (in_grace_period) {
            paused = 1;
        } else if (is_official_scheduler_governor_active()) {
            paused = 1;
        }
    }

    char gov_summary[256] = {0};
    size_t pos = 0;
    for (int i = 0; i < policy_count; i++) {
        char current_gov[64] = {0};
        if (!read_current_governor(cpu_policies[i].policy_num, current_gov, sizeof(current_gov))) {
            continue;
        }
        int written = safe_snprintf(
            gov_summary + pos,
            sizeof(gov_summary) - pos,
            "%spolicy%d=%s",
            pos == 0 ? "" : ", ",
            cpu_policies[i].policy_num,
            current_gov);
        if (written <= 0 || (size_t)written >= sizeof(gov_summary) - pos) {
            break;
        }
        pos += (size_t)written;
    }

    g_pause_game_scheduler.store(paused);
    g_scene_policy.official_conflict_active = paused;
    bool game_changed = false;
    if (current_app && current_app[0] && strcmp(current_app, "unknown") != 0 && is_game_package_name(current_app)) {
        game_changed = strcmp(last_logged_game_app, current_app) != 0;
        if (game_changed) {
            safe_strncpy(last_logged_game_app, current_app, sizeof(last_logged_game_app));
        }
    } else if (last_logged_game_app[0]) {
        last_logged_game_app[0] = '\0';
    }

    if (paused != last_state || game_changed) {
        char msg[512];
        safe_snprintf(
            msg,
            sizeof(msg),
            "%s | app:%s | governors: %s",
            paused ?
                (in_grace_period ?
                    "游戏刚进入前台，先暂停本模块调频与线程调度 2 秒等待官方调速器切换完成" :
                    "检测到SCX/HMBIRD类官方调速器仍在运行，暂停本模块调频与线程调度") :
                "未检测到SCX/HMBIRD类官方调速器，恢复本模块调频与线程调度",
            (current_app && current_app[0]) ? current_app : "unknown",
            gov_summary[0] ? gov_summary : "unknown");
        log_message(msg);
        last_state = paused;
    }
}

static bool should_pause_our_game_scheduler() {
    return g_pause_game_scheduler.load() != 0;
}

