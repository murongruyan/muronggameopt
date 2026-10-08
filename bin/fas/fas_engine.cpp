#include "fas_engine.h"
#include <cstdio>
#include <cstring>
#include <cmath>
#include <algorithm>

extern void log_message(const char* msg);
extern void log_fas_message(const char* msg);
extern long long get_current_time_ms();

/*
 * 分档参数。
 *
 * 动作档位的判据是"帧债"：这一帧的渲染时间 - 这一帧的预算。
 *   <= 0                 → 深睡地板（预算还有富余，能省就省）
 *   0 ~ 10% 预算         → 抬到甜点
 *   10% ~ 30% 预算       → 甜点做底 + 放开档位上限
 *   > 30% 预算，或连续两帧严重超预算 → 顶格
 *
 * "严重超预算"按帧事件计（每来一帧判断一次），不是按 tick 计——tick 是 100ms 级别，
 * 按 tick 计会把 144fps 的连续两帧漏掉。
 */
static const int kBaseSensitivity = 3;         /* 灵敏度基准：配置 kp=3.0 对应 1.0 倍 */

/* 动作表默认值（和以前写死的常量一致），dyn.json 的 control 可以覆盖。 */
static const FasLadderConfig kDefaultLadder = {
    30,     /* deadband_permille    3% 预算：稳态帧债在 ±150us 抖，算噪声 */
    100,    /* sweetspot_permille   10% 预算 */
    300,    /* ceiling_permille     30% 预算 */
    2,      /* severe_streak        连续两帧严重超预算就顶格 */
    250,    /* release_step_permille 降频步长占该簇范围 25% */
    60      /* release_hold_ms      两次降频至少隔 60ms */
};

static FasLadderConfig sanitize_ladder(const FasLadderConfig* in) {
    FasLadderConfig out = kDefaultLadder;
    if (!in) return out;
    if (in->deadband_permille >= 0 && in->deadband_permille <= 500) {
        out.deadband_permille = in->deadband_permille;
    }
    if (in->sweetspot_permille > out.deadband_permille && in->sweetspot_permille <= 2000) {
        out.sweetspot_permille = in->sweetspot_permille;
    }
    if (in->ceiling_permille >= out.sweetspot_permille && in->ceiling_permille <= 5000) {
        out.ceiling_permille = in->ceiling_permille;
    }
    if (in->severe_streak >= 1 && in->severe_streak <= 30) {
        out.severe_streak = in->severe_streak;
    }
    if (in->release_step_permille >= 20 && in->release_step_permille <= 1000) {
        out.release_step_permille = in->release_step_permille;
    }
    if (in->release_hold_ms >= 10 && in->release_hold_ms <= 1000) {
        out.release_hold_ms = in->release_hold_ms;
    }
    return out;
}

static int frame_budget_us_for_fps(int fps) {
    if (fps <= 0) return 0;
    return 1000000 / fps;
}

static void trim_frame_history(FasFrameBuffer* buf, int target_fps) {
    if (!buf) return;
    int max_frames = (target_fps > 0 ? target_fps : 144) * 5;
    if (max_frames < 1) max_frames = 1;
    if (max_frames > FAS_FRAME_HISTORY_MAX) max_frames = FAS_FRAME_HISTORY_MAX;
    if (buf->count <= max_frames) return;

    int remove_count = buf->count - max_frames;
    memmove(buf->frametimes_ms, buf->frametimes_ms + remove_count,
        max_frames * sizeof(double));
    buf->count = max_frames;
}

static void evaluate_frame_debt(FasEngine* e,
                                const FasFrameSnapshot* snap,
                                int target_fps,
                                FasOutput* out) {
    /*
     * 生效目标帧率：留 margin 作为安全余量，别等真掉帧了才动。
     * margin 来自 fas.json 的档位配置（静态值，不是 CPU 信号）。
     */
    float margin = fas_config_margin_fps_for_target(&e->config, e->buffer_state.package, target_fps);
    int effective_fps = (int)((float)target_fps - margin + 0.5f);
    if (effective_fps < 1) effective_fps = 1;
    if (effective_fps > target_fps) effective_fps = target_fps;

    int budget_us = frame_budget_us_for_fps(effective_fps);
    int frame_us = (int)(snap->last_frame_ms * 1000.0 + 0.5);
    int avg_frame_us = (int)(snap->avg_short_ms * 1000.0 + 0.5);
    int debt_us = avg_frame_us - budget_us;

    /*
     * 灵敏度：kp 越大，同样的帧债被判得越重（升频越积极）。
     * 默认 3.0，与 APK 里"比例增益"的语义一致。
     */
    float kp = fas_config_debt_sensitivity_for_target(
        &e->config, e->buffer_state.package, target_fps);
    int severity_us = (int)(debt_us * kp / (float)kBaseSensitivity);

    int moderate_us = budget_us * e->ladder.sweetspot_permille / 1000;
    int severe_us = budget_us * e->ladder.ceiling_permille / 1000;
    int deadband_us = budget_us * e->ladder.deadband_permille / 1000;

    int level = FAS_ACTION_DEEP_SLEEP;
    if (severity_us > deadband_us) level = FAS_ACTION_SWEETSPOT;
    if (severity_us > moderate_us) level = FAS_ACTION_SWEETSPOT_CEILING;
    if (severity_us > severe_us || e->over_budget_streak >= e->ladder.severe_streak) {
        level = FAS_ACTION_MAX_FREQ;
    }

    out->action_level = level;
    out->frame_us = frame_us;
    out->avg_frame_us = avg_frame_us;
    out->budget_us = budget_us;
    out->debt_us = debt_us;
    out->severity_us = severity_us;
    out->over_budget_streak = e->over_budget_streak;
    out->effective_target_fps = effective_fps;
    /*
     * guard_signal 仍然对外发布，但语义收敛为"这一帧需要保"：
     * 只有动作档位到"放开档位上限"及其以上才算需要保帧。
     */
    out->guard_signal =
        (level >= FAS_ACTION_SWEETSPOT_CEILING) ? FAS_GUARD_TAIL_FRAME : FAS_GUARD_NONE;
    e->last_action_level = level;
}

// ═══════════════════════════════════════════════════════════════
// 生命周期
// ═══════════════════════════════════════════════════════════════

bool fas_engine_init(FasEngine* e, const char* config_path) {
    if (!e || !config_path) return false;
    memset(e, 0, sizeof(*e));

    if (!fas_config_load_file(&e->config, config_path)) {
        char msg[256];
        snprintf(msg, sizeof(msg), "fas_engine: config load failed %s", config_path);
        log_message(msg);
        return false;
    }

    fas_buffer_init(&e->frame_buffer);

    memset(&e->buffer_state, 0, sizeof(e->buffer_state));
    e->buffer_state.working_state = FAS_BUFFER_UNUSABLE;
    e->last_action_level = FAS_ACTION_INVALID;
    e->ladder = kDefaultLadder;

    e->initialized = 1;

    char msg[160];
    snprintf(msg, sizeof(msg),
        "fas_engine: ladder deadband=%d sweet=%d ceiling=%d streak=%d release=%d/%dms",
        e->ladder.deadband_permille, e->ladder.sweetspot_permille,
        e->ladder.ceiling_permille, e->ladder.severe_streak,
        e->ladder.release_step_permille, e->ladder.release_hold_ms);
    log_message(msg);
    log_message("fas_engine: init ok");
    return true;
}

void fas_engine_destroy(FasEngine* e) {
    if (!e) return;
    memset(e, 0, sizeof(*e));
}

// ═══════════════════════════════════════════════════════════════
// 数据输入
// ═══════════════════════════════════════════════════════════════

void fas_engine_push_frame(FasEngine* e, long long interval_ns) {
    if (!e || !e->initialized) return;
    double frame_ms = interval_ns / 1000000.0;
    fas_buffer_push(&e->frame_buffer, frame_ms);
    e->buffer_state.last_update_ms = get_current_time_ms();
    trim_frame_history(&e->frame_buffer, e->buffer_state.stable_target_fps);

    /* 帧事件口径的"连续严重超预算" */
    int budget_us = frame_budget_us_for_fps(e->buffer_state.stable_target_fps);
    int frame_us = (int)(frame_ms * 1000.0 + 0.5);
    if (budget_us > 0 &&
        frame_us > budget_us + budget_us * e->ladder.ceiling_permille / 1000) {
        if (e->over_budget_streak < 1000) e->over_budget_streak++;
    } else {
        e->over_budget_streak = 0;
    }
}

void fas_engine_on_app_change(FasEngine* e, const char* package, int pid) {
    if (!e || !e->initialized || !package) return;

    fas_buffer_clear(&e->frame_buffer);
    memset(&e->buffer_state, 0, sizeof(e->buffer_state));
    e->buffer_state.working_state = FAS_BUFFER_UNUSABLE;
    strncpy(e->buffer_state.package, package, sizeof(e->buffer_state.package) - 1);
    e->buffer_state.package[sizeof(e->buffer_state.package) - 1] = '\0';
    e->buffer_state.working_state_since_ms = get_current_time_ms();
    e->over_budget_streak = 0;
    e->last_action_level = FAS_ACTION_INVALID;

    char msg[256];
    snprintf(msg, sizeof(msg), "fas_engine: switch app pkg=%s pid=%d", package, pid);
    log_fas_message(msg);
}

void fas_engine_set_ladder(FasEngine* e, const FasLadderConfig* ladder) {
    if (!e || !e->initialized || !ladder) return;
    FasLadderConfig sanitized = sanitize_ladder(ladder);
    if (memcmp(&sanitized, &e->ladder, sizeof(sanitized)) == 0) return;
    e->ladder = sanitized;
    char msg[160];
    snprintf(msg, sizeof(msg),
        "fas_engine: ladder 更新 deadband=%d sweet=%d ceiling=%d streak=%d release=%d/%dms",
        e->ladder.deadband_permille, e->ladder.sweetspot_permille,
        e->ladder.ceiling_permille, e->ladder.severe_streak,
        e->ladder.release_step_permille, e->ladder.release_hold_ms);
    log_fas_message(msg);
}

// ═══════════════════════════════════════════════════════════════
// 主 Tick
// ═══════════════════════════════════════════════════════════════

FasOutput fas_engine_tick(FasEngine* e) {
    FasOutput out;
    memset(&out, 0, sizeof(out));
    out.action_level = FAS_ACTION_INVALID;

    if (!e || !e->initialized) return out;
    if (e->buffer_state.package[0] == '\0') return out;

    long long now_ms = get_current_time_ms();

    // 1. 先用 long-window FPS 定档
    FasFrameSnapshot base_snap = fas_take_snapshot(
        &e->frame_buffer, &e->buffer_state,
        0, 0, now_ms);

    int target_fps = 0;
    bool target_changed = false;
    fas_resolve_target_fps(
        &e->buffer_state, &e->config,
        e->buffer_state.package,
        base_snap.current_fps_long, base_snap.current_fps_short, now_ms, false,
        &target_fps, &target_changed);

    out.current_fps = base_snap.current_fps_long;
    out.target_fps = target_fps;
    out.valid_samples = base_snap.frame_count;
    out.buffer_state = (int)base_snap.working_state;

    /*
     * 换档不清帧缓冲、不重新暖机：已经发生过的帧时间不会因为换档而失效。
     */
    if (target_changed) {
        e->over_budget_streak = 0;
    }

    if (target_fps <= 0) {
        // 画面帧率低于所有档位（加载/菜单）：不做帧感知决策，交给静态档位
        out.action_level = FAS_ACTION_INVALID;
        return out;
    }

    // 2. 按已解析的 target_fps 重新采样（short window 跟着档位走）
    FasFrameSnapshot snap = fas_take_snapshot(
        &e->frame_buffer, &e->buffer_state,
        target_fps, target_changed, now_ms);
    if (snap.working_state != e->buffer_state.working_state) {
        e->buffer_state.working_state = snap.working_state;
        e->buffer_state.working_state_since_ms = now_ms;
    }

    out.current_fps = snap.current_fps_long;
    out.target_fps = target_fps;
    out.valid_samples = snap.frame_count;
    out.buffer_state = (int)snap.working_state;

    // 3. buffer 还不可用时既不发布动作，也不让执行器去猜
    if (snap.working_state != FAS_BUFFER_USABLE) {
        out.action_level = FAS_ACTION_INVALID;
        return out;
    }

    // 4. 帧债 → 动作档位
    evaluate_frame_debt(e, &snap, target_fps, &out);

    // 5. 日志 (每 2s)
    if (now_ms - e->last_log_ms >= 2000) {
        e->last_log_ms = now_ms;
        char msg[288];
        snprintf(msg, sizeof(msg),
            "fas_debt | app:%s | tfps:%d(%.1f) | cfps:%d | frame:%.1fms avg:%.1fms | "
            "budget:%dus debt:%dus sev:%dus | action:%d | streak:%d",
            e->buffer_state.package,
            target_fps, (double)out.effective_target_fps,
            snap.current_fps_long,
            snap.last_frame_ms, snap.avg_short_ms,
            out.budget_us, out.debt_us, out.severity_us,
            out.action_level, out.over_budget_streak);
        log_fas_message(msg);
    }

    return out;
}
