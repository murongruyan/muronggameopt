#ifndef FAS_SAMPLING_H
#define FAS_SAMPLING_H

#include "fas_types.h"

#ifdef __cplusplus
extern "C" {
#endif

// ── 帧缓冲 ──

typedef struct {
    // 帧时间环形缓冲（最旧=index 0，最新=index count-1）
    double frametimes_ms[FAS_FRAME_HISTORY_MAX];
    int count;

    // 时间戳辅助（用于 gap compensation）
    long long last_frame_time_ms;
    long long first_frame_time_ms;
} FasFrameBuffer;

void fas_buffer_init(FasFrameBuffer* buf);
void fas_buffer_push(FasFrameBuffer* buf, double interval_ms);
void fas_buffer_clear(FasFrameBuffer* buf);

// ── 快照采集 ──

FasFrameSnapshot fas_take_snapshot(
    const FasFrameBuffer* buf,
    const FasBufferStateInfo* state,
    int target_fps,
    int target_changed,
    long long now_ms);

// ── 窗口统计（内部可测试）──

void fas_compute_windows(
    const double* frametimes, int count,
    int short_count, int long_count,
    double* out_last_ms,
    double* out_avg4_ms, double* out_avg8_ms,
    double* out_avg_short_ms, double* out_avg_long_ms,
    double* out_slope_ms);

int fas_calc_fps(double avg_ms);

#ifdef __cplusplus
}
#endif

#endif // FAS_SAMPLING_H
