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
} FasEngine;

// ── 公开 API ────────────────────────────────────────────────

bool fas_engine_init(FasEngine* e, const char* config_path);
void fas_engine_destroy(FasEngine* e);

// 推帧：interval_ns = 该 surface 相邻两次 queueBuffer 的间隔
void fas_engine_push_frame(FasEngine* e, long long interval_ns);

void fas_engine_on_app_change(FasEngine* e, const char* package, int pid);

// 更新动作表参数（动态调频配置）
void fas_engine_set_ladder(FasEngine* e, const FasLadderConfig* ladder);

// 主 tick：把当前帧债换算成动作档位
FasOutput fas_engine_tick(FasEngine* e);

#ifdef __cplusplus
}
#endif

#endif // FAS_ENGINE_H
