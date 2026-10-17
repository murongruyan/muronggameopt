#include "fas_engine.h"
#include <cstdio>
#include <cstring>
#include <cmath>
#include <algorithm>

extern void log_message(const char* msg);
extern void log_fas_message(const char* msg);
extern long long get_current_time_ms();

/*
 * ═══════════════════════════════════════════════════════════════
 * FAS 控制器（v6.2）—— 真正的 Frame Aware Scheduling
 * ═══════════════════════════════════════════════════════════════
 *
 * 旧实现（v6.1 及以前）的问题：
 *   帧债 -> 查【人工枚举的档位表】(60/90/120/144/165) -> 四档动作 -> 执行器按档位取频率。
 *   全程没有一处「根据帧率算出该给多少频率」，本质是机械映射 ✗
 *   典型翻车：游戏稳跑 72fps，档位表里没有 72 -> 被向上取档判成 120 ->
 *   每帧都"严重超预算" -> 永久 MAX_FREQ（min=max=满频），CPU 85% 空闲却空转 ✗
 *
 * 新实现（用户方案）：
 *   ① 校准 CALIBRATING：app 切入后把频率固定在【甜点】，采集 2.5s 帧间隔，
 *      取中位数 = 基准帧间隔 base_interval。**不再需要任何人工档位表**。
 *   ② 稳定 STABLE：帧率 >= 基准 - 1.2fps 即算稳定 -> 降频
 *        quick_drop 开  -> 直接降到底板（一点不犹豫）
 *        quick_drop 关  -> 每 200ms 降 8%
 *   ③ 掉帧 DEGRADED：帧率 < 基准 - 1.2fps -> 按比例升频
 *        需要频率倍数 = 实际帧间隔 / 基准帧间隔   ← 这就是"按帧率算该升多少"
 *
 * 升帧率检测（引擎自己判断，不需要用户配置）：
 *   A 快攻：稳定态下连续 0.5s 帧率比基准快 >=2fps -> 游戏自己升帧率了，立即抬基准
 *   B 松手：掉帧态持续 3s -> 说明是场景真的变重（GPU 瓶颈 / 特效拉满），
 *          不是 CPU 不够，把基准降到当前帧率接受新水平，避免满频空转
 *
 * 占用率升频（可选附加触发，不替代掉帧升频）：
 *   重要线程占用 >= defaults.upscale_occupancy -> 直接升频。
 *   ⚠️ 掉帧时无论占用率多少都要升频，这里只是"多加一个更早介入的入口"。
 */

/* ── 动作表（仅 fas_engine_set_ladder 的兼容壳，新控制器不用）── */
static const FasLadderConfig kDefaultLadder = {
    30, 100, 300, 2, 250, 60
};

static FasLadderConfig sanitize_ladder(const FasLadderConfig* in) {
    FasLadderConfig out = kDefaultLadder;
    if (!in) return out;
    if (in->deadband_permille >= 0 && in->deadband_permille <= 500) out.deadband_permille = in->deadband_permille;
    if (in->sweetspot_permille > out.deadband_permille && in->sweetspot_permille <= 2000) out.sweetspot_permille = in->sweetspot_permille;
    if (in->ceiling_permille >= out.sweetspot_permille && in->ceiling_permille <= 5000) out.ceiling_permille = in->ceiling_permille;
    if (in->severe_streak >= 1 && in->severe_streak <= 30) out.severe_streak = in->severe_streak;
    if (in->release_step_permille >= 20 && in->release_step_permille <= 1000) out.release_step_permille = in->release_step_permille;
    if (in->release_hold_ms >= 10 && in->release_hold_ms <= 1000) out.release_hold_ms = in->release_hold_ms;
    return out;
}

/* ── 新控制器参数 ─────────────────────────────────────────── */
static const long long kCalibMs = 2500;        /* 校准时长：在甜点频率下观察 2.5 秒 */
static const int kCalibMinFrames = 20;         /* 至少这么多帧才算校准有效 */
static const int kCalibMaxSamples = 256;
static const float kFpsTolerance = 1.2f;       /* 用户口径：±1.2fps 内算稳定 */
static const long long kDegradeHoldMs = 150;   /* 连续掉出容差多久才判掉帧（抗抖动） */
static const long long kSlowDropIntervalMs = 200; /* 慢降间隔 */
static const float kSlowDropScale = 0.92f;     /* 慢降每次 -8% */
static const float kRaiseMargin = 1.04f;       /* 升频多给 4% 余量，避免刚好不够 */
static const int kUpshiftDetectFps = 2;        /* 比基准快 2fps 以上视为升帧率候选 */
static const long long kUpshiftHoldMs = 500;   /* 持续 0.5s 才接受 */
static const long long kSceneHeavyHoldMs = 3000; /* 掉帧持续 3s -> 接受新帧率（场景变重） */
static const float kOccupancyRaiseScale = 1.15f; /* 占用率触发升频时给 +15% */

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
    memmove(buf->frametimes_ms, buf->frametimes_ms + remove_count, max_frames * sizeof(double));
    buf->count = max_frames;
}

static int fps_from_interval(double interval_ms) {
    if (interval_ms <= 0.01) return 0;
    double fps = 1000.0 / interval_ms;
    if (fps < 1.0) fps = 1.0;
    if (fps > 1000.0) fps = 1000.0;
    return (int)(fps + 0.5);
}

/* 中位数（会排序传入副本的下标，不改原数组顺序由调用方负责） */
static double median_of(const double* values, int count) {
    if (!values || count <= 0) return 0.0;
    double tmp[kCalibMaxSamples];
    int n = count < kCalibMaxSamples ? count : kCalibMaxSamples;
    for (int i = 0; i < n; i++) tmp[i] = values[i];
    std::sort(tmp, tmp + n);
    if (n % 2 == 1) return tmp[n / 2];
    return (tmp[n / 2 - 1] + tmp[n / 2]) * 0.5;
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
    e->ladder = kDefaultLadder;
    e->phase = FAS_PHASE_UNKNOWN;
    e->base_interval_ms = 0.0;
    e->stable_fps = 0;
    e->busy_thread_usage = -1;
    e->initialized = 1;

    char msg[220];
    snprintf(msg, sizeof(msg),
        "fas_engine: v6.2 三相位控制器 | quick_drop=%d report_thread=%d upscale_occ=%d%%",
        e->config.quick_drop, e->config.report_thread_state, e->config.upscale_occupancy);
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
    trim_frame_history(&e->frame_buffer, e->stable_fps > 0 ? e->stable_fps : 120);
}

void fas_engine_set_thread_usage(FasEngine* e, int busy_pct) {
    if (!e) return;
    e->busy_thread_usage = (busy_pct < 0) ? -1 : (busy_pct > 100 ? 100 : busy_pct);
}

void fas_engine_on_app_change(FasEngine* e, const char* package, int pid) {
    if (!e || !e->initialized || !package) return;

    fas_buffer_clear(&e->frame_buffer);
    memset(&e->buffer_state, 0, sizeof(e->buffer_state));
    e->buffer_state.working_state = FAS_BUFFER_UNUSABLE;
    strncpy(e->buffer_state.package, package, sizeof(e->buffer_state.package) - 1);
    e->buffer_state.package[sizeof(e->buffer_state.package) - 1] = '\0';
    e->buffer_state.working_state_since_ms = get_current_time_ms();

    /* 换 app 必须重新校准：帧率基准是「每个游戏自己的」，不能跨游戏复用 */
    e->phase = FAS_PHASE_UNKNOWN;
    e->phase_since_ms = 0;
    e->calib_count = 0;
    e->calib_since_ms = 0;
    e->base_interval_ms = 0.0;
    e->stable_fps = 0;
    e->degrade_candidate_ms = 0;
    e->upshift_candidate_ms = 0;
    e->upshift_candidate_fps = 0;
    e->last_drop_ms = 0;
    e->busy_thread_usage = -1;

    char msg[256];
    snprintf(msg, sizeof(msg), "fas_engine: switch app pkg=%s pid=%d -> recalibrate", package, pid);
    log_fas_message(msg);
}

void fas_engine_set_ladder(FasEngine* e, const FasLadderConfig* ladder) {
    if (!e || !e->initialized || !ladder) return;
    FasLadderConfig sanitized = sanitize_ladder(ladder);
    if (memcmp(&sanitized, &e->ladder, sizeof(sanitized)) == 0) return;
    e->ladder = sanitized;
}

// ═══════════════════════════════════════════════════════════════
// 相位机
// ═══════════════════════════════════════════════════════════════

static void enter_phase(FasEngine* e, int phase, long long now_ms) {
    e->phase = phase;
    e->phase_since_ms = now_ms;
    if (phase == FAS_PHASE_STABLE) {
        e->degrade_candidate_ms = 0;
        e->upshift_candidate_ms = 0;
        e->last_drop_ms = now_ms;
    }
}

FasOutput fas_engine_tick(FasEngine* e) {
    FasOutput out;
    memset(&out, 0, sizeof(out));
    out.action_level = FAS_ACTION_INVALID;
    out.phase = FAS_PHASE_UNKNOWN;
    out.freq_scale = 1.0f;
    out.busy_thread_usage = -1;

    if (!e || !e->initialized) return out;
    if (e->buffer_state.package[0] == '\0') return out;

    long long now_ms = get_current_time_ms();

    double avg_short_ms = 0.0, last_ms = 0.0, slope = 0.0;
    int fps_long = 0;

    if (e->frame_buffer.count > 0) {
        int stable = e->stable_fps > 0 ? e->stable_fps : 120;
        FasFrameSnapshot snap = fas_take_snapshot(
            &e->frame_buffer, &e->buffer_state, stable, 0, now_ms);
        avg_short_ms = snap.avg_short_ms;
        last_ms = snap.last_frame_ms;
        slope = snap.frame_slope_ms;
        fps_long = snap.current_fps_long;

        if (e->buffer_state.working_state != snap.working_state) {
            e->buffer_state.working_state = snap.working_state;
            e->buffer_state.working_state_since_ms = now_ms;
        }
        if (snap.working_state != FAS_BUFFER_USABLE) {
            /* 画面没在稳定出帧（加载/暂停）：退到静态档位，不猜 */
            if (e->phase != FAS_PHASE_UNKNOWN) {
                e->phase = FAS_PHASE_UNKNOWN;
                e->calib_count = 0;
                e->base_interval_ms = 0.0;
                e->stable_fps = 0;
            }
            return out;
        }
    } else {
        return out;
    }

    out.current_fps = fps_long;
    out.valid_samples = e->frame_buffer.count;

    if (e->phase == FAS_PHASE_UNKNOWN) {
        enter_phase(e, FAS_PHASE_CALIBRATING, now_ms);
        e->calib_count = 0;
        e->calib_since_ms = now_ms;
    }

    /* ── ① 校准：在甜点频率下测这个游戏实际能跑多少帧 ── */
    if (e->phase == FAS_PHASE_CALIBRATING) {
        if (e->calib_since_ms <= 0) e->calib_since_ms = now_ms;
        if (e->calib_count < kCalibMaxSamples) {
            e->calib_samples[e->calib_count++] = avg_short_ms;
        }
        out.phase = FAS_PHASE_CALIBRATING;
        out.hold_sweetspot = 1;
        out.stable_fps = 0;
        out.action_level = FAS_ACTION_SWEETSPOT;

        long long elapsed = now_ms - e->calib_since_ms;
        if (elapsed >= kCalibMs && e->calib_count >= kCalibMinFrames) {
            double med = median_of(e->calib_samples, e->calib_count);
            if (med > 0.5) {
                e->base_interval_ms = med;
                e->stable_fps = fps_from_interval(med);
                enter_phase(e, FAS_PHASE_STABLE, now_ms);
                char msg[220];
                snprintf(msg, sizeof(msg),
                    "fas_calib done | app:%s | base:%.2fms stable_fps:%d (from %d samples)",
                    e->buffer_state.package, med, e->stable_fps, e->calib_count);
                log_fas_message(msg);
            } else {
                /* 帧间隔异常：继续校准 */
                e->calib_since_ms = now_ms;
                e->calib_count = 0;
            }
        }
        return out;
    }

    if (e->stable_fps <= 0 || e->base_interval_ms <= 0.0) {
        /* 兜底：基准丢了就重新校准 */
        enter_phase(e, FAS_PHASE_CALIBRATING, now_ms);
        e->calib_count = 0;
        e->calib_since_ms = now_ms;
        out.hold_sweetspot = 1;
        out.phase = FAS_PHASE_CALIBRATING;
        out.action_level = FAS_ACTION_SWEETSPOT;
        return out;
    }

    int busy = e->busy_thread_usage;
    int occ_trigger = 0;
    if (e->config.report_thread_state && e->config.upscale_occupancy > 0 && busy >= 0 &&
        busy >= e->config.upscale_occupancy) {
        occ_trigger = 1;
    }

    /* ── ② 稳定：降频 ── */
    if (e->phase == FAS_PHASE_STABLE) {
        bool under = ((float)fps_long < (float)e->stable_fps - kFpsTolerance);

        if (under) {
            if (e->degrade_candidate_ms <= 0) e->degrade_candidate_ms = now_ms;
            if (now_ms - e->degrade_candidate_ms >= kDegradeHoldMs) {
                enter_phase(e, FAS_PHASE_DEGRADED, now_ms);
                e->degrade_candidate_ms = 0;
            }
        } else {
            e->degrade_candidate_ms = 0;

            /* 升帧率检测：游戏自己把帧率抬上去了（改设置 / 开高刷 / 场景变轻） */
            if (fps_long >= e->stable_fps + kUpshiftDetectFps) {
                if (e->upshift_candidate_ms <= 0 ||
                    e->upshift_candidate_fps != fps_long) {
                    e->upshift_candidate_ms = now_ms;
                    e->upshift_candidate_fps = fps_long;
                } else if (now_ms - e->upshift_candidate_ms >= kUpshiftHoldMs) {
                    double new_med = median_of(e->calib_samples, e->calib_count);
                    if (new_med <= 0.5 || avg_short_ms < new_med) new_med = avg_short_ms;
                    int old_fps = e->stable_fps;
                    int new_fps = fps_from_interval(new_med);
                    e->upshift_candidate_ms = 0;
                    e->upshift_candidate_fps = 0;
                    /* 只在基准真的变化时才落日志，避免 48->48 这种空记录 */
                    if (new_fps > old_fps) {
                        e->base_interval_ms = new_med;
                        e->stable_fps = new_fps;
                        e->calib_count = 0;
                        char msg[240];
                        snprintf(msg, sizeof(msg),
                            "fas_upshift | app:%s | fps %d -> %d (game raised its own cap)",
                            e->buffer_state.package, old_fps, e->stable_fps);
                        log_fas_message(msg);
                    }
                }
            } else {
                e->upshift_candidate_ms = 0;
                e->upshift_candidate_fps = 0;
            }
        }

        if (e->phase == FAS_PHASE_STABLE) {
            out.phase = FAS_PHASE_STABLE;
            out.stable_fps = e->stable_fps;

            if (e->config.quick_drop) {
                /* 快速降频：稳定就毫不犹豫直接降到底板 */
                out.drop_to_floor = 1;
            } else if (now_ms - e->last_drop_ms >= kSlowDropIntervalMs) {
                out.freq_scale = kSlowDropScale;
                e->last_drop_ms = now_ms;
            }

            if (occ_trigger) {
                out.upscale_by_occupancy = 1;
                if (out.freq_scale < kOccupancyRaiseScale) out.freq_scale = kOccupancyRaiseScale;
            }

            out.action_level = occ_trigger ? FAS_ACTION_SWEETSPOT_CEILING : FAS_ACTION_DEEP_SLEEP;
        }
    }

    /* ── ③ 掉帧：按比例升频 ── */
    if (e->phase == FAS_PHASE_DEGRADED) {
        out.phase = FAS_PHASE_DEGRADED;
        out.stable_fps = e->stable_fps;

        /* 恢复：回到容差内就回稳定态 */
        if ((float)fps_long >= (float)e->stable_fps - kFpsTolerance * 0.5f) {
            enter_phase(e, FAS_PHASE_STABLE, now_ms);
            out.phase = FAS_PHASE_STABLE;
            out.freq_scale = 1.0f;
            out.action_level = FAS_ACTION_DEEP_SLEEP;
        } else {
            /*
             * 真正的 FAS：需要多少频率由帧间隔比例算出来。
             * 负载基本正比于频率，所以「实际帧间隔 / 基准帧间隔」就是需要的频率倍数。
             */
            double scale = avg_short_ms / e->base_interval_ms;
            if (scale < 1.0) scale = 1.0;
            if (scale > 4.0) scale = 4.0;
            scale *= kRaiseMargin;
            out.freq_scale = (float)scale;
            out.action_level = (scale < 1.3) ? FAS_ACTION_SWEETSPOT_CEILING : FAS_ACTION_MAX_FREQ;

            /* 场景真的变重（GPU 瓶颈）：顶了很久还是追不上，接受新帧率，避免满频空转 */
            if (now_ms - e->phase_since_ms >= kSceneHeavyHoldMs) {
                double new_med = avg_short_ms;
                int old_fps = e->stable_fps;
                e->base_interval_ms = new_med;
                e->stable_fps = fps_from_interval(new_med);
                e->calib_count = 0;
                enter_phase(e, FAS_PHASE_STABLE, now_ms);
                out.phase = FAS_PHASE_STABLE;
                out.freq_scale = 1.0f;
                out.action_level = FAS_ACTION_DEEP_SLEEP;
                char msg[240];
                snprintf(msg, sizeof(msg),
                    "fas_scene_heavy | app:%s | accept new base %d -> %d fps (stuck 3s)",
                    e->buffer_state.package, old_fps, e->stable_fps);
                log_fas_message(msg);
            }
        }
    }

    /* ── 诊断字段（日志 / App 状态页用）── */
    out.target_fps = e->stable_fps;
    out.effective_target_fps = e->stable_fps;
    out.frame_us = (int)(last_ms * 1000.0 + 0.5);
    out.avg_frame_us = (int)(avg_short_ms * 1000.0 + 0.5);
    out.budget_us = frame_budget_us_for_fps(e->stable_fps);
    out.debt_us = out.avg_frame_us - out.budget_us;
    out.severity_us = (int)(slope * 1000.0);
    out.over_budget_streak = (out.phase == FAS_PHASE_DEGRADED) ? 1 : 0;
    out.busy_thread_usage = busy;
    out.guard_signal = (out.phase == FAS_PHASE_DEGRADED) ? FAS_GUARD_TAIL_FRAME : FAS_GUARD_NONE;

    if (now_ms - e->last_log_ms >= 2000) {
        e->last_log_ms = now_ms;
        const char* ph = (out.phase == FAS_PHASE_CALIBRATING) ? "CALIB"
                       : (out.phase == FAS_PHASE_STABLE) ? "STABLE"
                       : (out.phase == FAS_PHASE_DEGRADED) ? "DEGRADE" : "UNKNOWN";
        char msg[288];
        snprintf(msg, sizeof(msg),
            "fas | app:%s | phase:%s | fps:%d base:%d | interval:%.2f base:%.2fms | scale:%.2f | occ:%d%s%s",
            e->buffer_state.package, ph,
            fps_long, e->stable_fps,
            avg_short_ms, e->base_interval_ms,
            (double)out.freq_scale, busy,
            out.drop_to_floor ? " | DROP_TO_FLOOR" : "",
            out.upscale_by_occupancy ? " | OCC_RAISE" : "");
        log_fas_message(msg);
    }

    return out;
}
