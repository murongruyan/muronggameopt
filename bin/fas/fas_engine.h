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

    /*
     * ── 自动目标帧率（auto target）────────────────────────────
     *
     * 为什么不再只依赖 fas.json 的档位表：
     *   档位表是【人工猜的】。一旦游戏实际帧率不在表里，就会判错方向：
     *   实例（用户实测）：com.netease.lrs 稳跑 72fps，但它所在分组只有 60/120/165 三档，
     *   resolve_instant_target_fps 把 72 【向上取档】判成 120 -> 预算 8333us，
     *   实际每帧 13845us -> 帧债 5556us > 严重阈值 -> 永久 MAX_FREQ（band_min=band_max=fmax），
     *   CPU 85% 空闲却在满频空转 ✗
     *
     * 现在的做法（用户提的方案）：
     *   「不用自己写帧率，就在甜点频率等几秒看帧率落在哪，再慢慢降频，
     *     哪里稳不住了（掉 1~2 帧）再拉上去」
     *   即：目标 = 这个游戏【实际能跑到的帧率】= 观测到的峰值帧率（带缓慢衰减），
     *   预算由它推出来，帧债机制（升/降频）完全复用，不需要人工档位表 ✓
     */
    int auto_peak_fps;      /* 最近观测到的可达到帧率（峰值） */
    long long auto_peak_ms; /* 上次刷新/维持峰值的时刻 */
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
