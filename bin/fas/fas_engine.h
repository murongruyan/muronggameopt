#ifndef FAS_ENGINE_H
#define FAS_ENGINE_H

#include "fas_types.h"
#include "fas_config.h"
#include "fas_sampling.h"
#include "fas_target.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * FAS 引擎：只看画面。
 *
 * 输入是真实的帧间隔（queueBuffer 相邻两帧的 ktime 差），输出是"这一帧还欠多少时间"
 * 对应的动作档位。引擎不做 CPU 负载统计、不预测频率、不为决策扫 /proc：
 * 频率怎么给是执行器按档位查表的事，引擎只判断该花多少预算。
 */

typedef struct {
    // 配置
    FasConfig config;

    // 帧缓冲
    FasFrameBuffer frame_buffer;

    // Buffer 状态（目标帧率、working state）
    FasBufferStateInfo buffer_state;

    // 连续严重超预算的帧数（按帧事件计，用于"连续两帧顶格"）
    int over_budget_streak;

    // 最近一次发布的动作档位
    int last_action_level;

    // 动作表参数（由 dyn.json 的 control 提供，运行时可改）
    FasLadderConfig ladder;

    // 日志抑制
    long long last_log_ms;

    int initialized;

    /* ══════════════════════════════════════════════════════════
     * 新控制器状态（v6.2）—— 三相位 + 帧率比例控制
     * ══════════════════════════════════════════════════════════
     */
    int phase;                    /* FasPhase */
    long long phase_since_ms;     /* 进入当前相位的时刻 */

    /* 校准：在甜点频率下采集真实帧间隔，取中位数当基准 */
    double calib_samples[256];
    int calib_count;
    long long calib_since_ms;

    /* 基准（校准产物，也是唯一的帧率参考，取代 fas.json 的 target_fps） */
    double base_interval_ms;      /* 基准帧间隔 */
    int stable_fps;               /* 基准帧率 = 1000/base_interval */

    /* 掉帧判定的连续计时（抗抖动） */
    long long degrade_candidate_ms;
    /* 升帧率检测：基准可能被游戏自己抬高 */
    long long upshift_candidate_ms;
    int upshift_candidate_fps;

    /* 降频节流 */
    long long last_drop_ms;

    /* 重要线程占用（由 main_loop 每拍喂入；-1 = 未上报） */
    int busy_thread_usage;
} FasEngine;

// ── 公开 API ────────────────────────────────────────────────

bool fas_engine_init(FasEngine* e, const char* config_path);
void fas_engine_destroy(FasEngine* e);

// 推帧：interval_ns = 该 surface 相邻两次 queueBuffer 的间隔
void fas_engine_push_frame(FasEngine* e, long long interval_ns);

void fas_engine_on_app_change(FasEngine* e, const char* package, int pid);

// 更新动作表参数（动态调频配置）—— 兼容保留，新控制器不用它
void fas_engine_set_ladder(FasEngine* e, const FasLadderConfig* ladder);

/*
 * 喂入「重要线程占用率」（game/render/rhi/taskgraph/worker 里最高的那个，百分比）。
 * 只在 fas.json 的 defaults.report_thread_state = 1 时由 main_loop 调用；
 * 传 -1 表示本拍没有上报（FAS 就只靠帧信号决策）。
 */
void fas_engine_set_thread_usage(FasEngine* e, int busy_pct);

// 主 tick：把当前帧债换算成动作档位
FasOutput fas_engine_tick(FasEngine* e);

#ifdef __cplusplus
}
#endif

#endif // FAS_ENGINE_H
