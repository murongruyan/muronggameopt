#include "fas_sampling.h"
#include <cmath>
#include <cstring>
#include <algorithm>

// ── 常量 ───────────────────────────────────────────────────

/*
 * 帧信号新鲜度窗口。
 *
 * 画面没在出帧（加载、菜单、暂停）时，queueBuffer 会安静下来，但引擎的 tick 还在跑。
 * 旧实现把这段静默时间当成"额外帧时间"加到窗口均值上，于是安静被读成"一帧 1.3 秒"，
 * 引擎把加载画面误判成严重卡顿，一路顶到 fmax，实测帧时间也比用户看到的差一个量级。
 *
 * 帧时间只能由真实帧构成：窗口均值只统计真正发生过的帧间隔，静默只表达为"信号过期"
 * （buffer 变不可用，控制自然释放），不再伪造巨大的帧时间。
 */
static const long long kFreshFrameMaxAgeMs = 250;
static const long long kBufferUsableWarmupMs = 1000;

// ── 帧缓冲 ─────────────────────────────────────────────────

void fas_buffer_init(FasFrameBuffer* buf) {
    if (!buf) return;
    buf->count = 0;
    buf->last_frame_time_ms = 0;
    buf->first_frame_time_ms = 0;
}

void fas_buffer_push(FasFrameBuffer* buf, double interval_ms) {
    if (!buf) return;

    // 过滤无效帧间隔（< 1ms 或 > 500ms）
    if (interval_ms < 1.0 || interval_ms > 500.0) return;

    if (buf->count < FAS_FRAME_HISTORY_MAX) {
        buf->frametimes_ms[buf->count++] = interval_ms;
    } else {
        // 队列已满，移除最旧的，插入最新的
        memmove(buf->frametimes_ms, buf->frametimes_ms + 1,
                (FAS_FRAME_HISTORY_MAX - 1) * sizeof(double));
        buf->frametimes_ms[FAS_FRAME_HISTORY_MAX - 1] = interval_ms;
    }
}

void fas_buffer_clear(FasFrameBuffer* buf) {
    if (!buf) return;
    buf->count = 0;
}

// ── 窗口统计 ────────────────────────────────────────────────

void fas_compute_windows(
    const double* frametimes, int count,
    int short_count, int long_count,
    double* out_last_ms,
    double* out_avg4_ms, double* out_avg8_ms,
    double* out_avg_short_ms, double* out_avg_long_ms,
    double* out_slope_ms)
{
    if (!frametimes || count <= 0) {
        if (out_last_ms) *out_last_ms = 0;
        if (out_avg4_ms) *out_avg4_ms = 0;
        if (out_avg8_ms) *out_avg8_ms = 0;
        if (out_avg_short_ms) *out_avg_short_ms = 0;
        if (out_avg_long_ms) *out_avg_long_ms = 0;
        if (out_slope_ms) *out_slope_ms = 0;
        return;
    }

    int n = count;
    int avg4_n = std::min(4, n);
    int avg8_n = std::min(8, n);
    int short_n = std::min(short_count, n);
    int long_n = std::min(long_count, n);

    double sum4 = 0, sum8 = 0, sum_short = 0, sum_long = 0;
    // frametimes 最新=最后，旧=最前；遍历从最新到最旧
    for (int i = 0; i < n; i++) {
        double v = frametimes[n - 1 - i];
        if (i < avg4_n) sum4 += v;
        if (i < avg8_n) sum8 += v;
        if (i < short_n) sum_short += v;
        if (i < long_n) sum_long += v;
    }

    if (out_last_ms) *out_last_ms = frametimes[n - 1];
    if (out_avg4_ms) *out_avg4_ms = sum4 / avg4_n;
    if (out_avg8_ms) *out_avg8_ms = sum8 / avg8_n;
    if (out_avg_short_ms) *out_avg_short_ms = sum_short / short_n;
    if (out_avg_long_ms) *out_avg_long_ms = sum_long / long_n;

    // 帧斜率: (最新 - 第4新的) / 3
    if (out_slope_ms) {
        if (n >= 4) {
            *out_slope_ms = (frametimes[n - 1] - frametimes[n - 4]) / 3.0;
        } else if (n >= 2) {
            *out_slope_ms = frametimes[n - 1] - frametimes[n - 2];
        } else {
            *out_slope_ms = 0;
        }
    }
}

int fas_calc_fps(double avg_ms) {
    if (avg_ms <= 0) return 0;
    return (int)(1000.0 / avg_ms + 0.5);
}

// ── 快照采集 ────────────────────────────────────────────────

FasFrameSnapshot fas_take_snapshot(
    const FasFrameBuffer* buf,
    const FasBufferStateInfo* state,
    int target_fps,
    int target_changed,
    long long now_ms)
{
    FasFrameSnapshot snap;
    memset(&snap, 0, sizeof(snap));

    if (!buf || buf->count <= 0) {
        snap.target_fps = target_fps;
        snap.target_changed = target_changed ? 1 : 0;
        return snap;
    }

    int frame_count = buf->count;

    // 自适应 short_count
    int short_count = 12;
    if (target_fps > 0) {
        short_count = target_fps / 10;
        if (short_count < 8) short_count = 8;
        if (short_count > 18) short_count = 18;
    }
    int long_count = std::min(frame_count, 60);

    double last_ms, avg4, avg8, avg_short, avg_long, slope;
    fas_compute_windows(buf->frametimes_ms, frame_count,
                        short_count, long_count,
                        &last_ms, &avg4, &avg8, &avg_short, &avg_long, &slope);

    // 帧信号新鲜度：只表达"多久没有新帧了"，不参与帧时间计算
    long long sample_age = (state && state->last_update_ms > 0)
        ? (now_ms - state->last_update_ms) : 0;
    if (sample_age < 0) sample_age = 0;
    bool frames_fresh = sample_age <= kFreshFrameMaxAgeMs;

    // FPS
    int fps_short = fas_calc_fps(avg_short);
    int fps_long = fas_calc_fps(avg_long);

    // Working state
    FasBufferState ws = FAS_BUFFER_UNUSABLE;
    if (frame_count >= 6 && avg_long > 0 && frames_fresh) {
        ws = FAS_BUFFER_USABLE;
        // warmup 检查
        if (state && state->working_state != FAS_BUFFER_USABLE) {
            long long warmup_elapsed = (state->working_state_since_ms > 0)
                ? (now_ms - state->working_state_since_ms) : 0;
            if (warmup_elapsed < kBufferUsableWarmupMs) {
                ws = FAS_BUFFER_UNUSABLE;
            }
        }
    }

    snap.frame_count = frame_count;
    snap.short_count = short_count;
    snap.long_count = long_count;
    snap.target_fps = target_fps;
    snap.target_changed = target_changed ? 1 : 0;
    snap.last_frame_ms = last_ms;
    snap.avg_4_ms = avg4;
    snap.avg_8_ms = avg8;
    snap.avg_short_ms = avg_short;
    snap.avg_long_ms = avg_long;
    snap.frame_slope_ms = slope;
    snap.frames_fresh = frames_fresh ? 1 : 0;
    snap.frame_age_ms = (double)sample_age;
    snap.current_fps_short = fps_short;
    snap.current_fps_long = fps_long;
    snap.working_state = ws;

    return snap;
}
