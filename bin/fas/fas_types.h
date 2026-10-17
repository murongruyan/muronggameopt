#ifndef FAS_TYPES_H
#define FAS_TYPES_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// ── 常量 ───────────────────────────────────────────────────

#define FAS_MAX_POLICIES 8
#define FAS_MAX_GOV_PARAMS 20
#define FAS_MAX_FAS_APPS 256
#define FAS_MAX_PACKAGE_LEN 128
#define FAS_MAX_FREQ_TABLE 128
#define FAS_MAX_CLUSTERS 4
#define FAS_FRAME_HISTORY_MAX 1440

// ── 枚举 ───────────────────────────────────────────────────

typedef enum {
    FAS_BUFFER_UNUSABLE = 0,
    FAS_BUFFER_USABLE = 1
} FasBufferState;

typedef enum {
    FAS_GUARD_NONE = 0,
    FAS_GUARD_MISPREDICT,
    FAS_GUARD_TAIL_FRAME,
    FAS_GUARD_QUEUEBUFFER,
    FAS_GUARD_THREAD_PRESSURE
} FasGuardReason;

/*
 * FAS 动作档位：FAS 不再输出"目标频率"，而是输出"这一帧还欠多少时间"对应的动作。
 * 具体频率由执行器按档位查表（地板 / 甜点 / 档位上限），FAS 只负责判断该花多少。
 */
/*
 * FAS 三相位（v6.2 起）：这是新的控制骨架。
 *
 * 旧的四档动作（DEEP_SLEEP / SWEETSPOT / CEILING / MAX_FREQ）本质是
 * 「帧债 → 查人工枚举的档位表 → 机械映射到频率区间」，
 * 中间没有一处是「按帧率算出该给多少频率」，所以它不是真正的 FAS。
 *
 * 新骨架把控制拆成三个物理上可解释的相位：
 *   CALIBRATING  在甜点频率下测这个游戏实际能跑多少帧（取中位数）
 *   STABLE       帧率在基准 ±1.2fps 内 -> 可以降频（快降到底板 / 慢降 8%）
 *   DEGRADED     掉出容差 -> 需要频率倍数 = 实际帧间隔 / 基准帧间隔
 */
typedef enum {
    FAS_PHASE_UNKNOWN = 0,      /* 没有帧信号 */
    FAS_PHASE_CALIBRATING = 1,  /* 甜点频率下校准帧率 */
    FAS_PHASE_STABLE = 2,       /* 帧率稳定，可降频 */
    FAS_PHASE_DEGRADED = 3      /* 掉帧，按比例升频 */
} FasPhase;

/*
 * action_level 仅作为【兼容字段】保留：
 * 执行器的 GPU/DDR 侧与 trace 日志还在读它，内部按相位映射一次即可。
 */
typedef enum {
    FAS_ACTION_INVALID = -1,
    FAS_ACTION_DEEP_SLEEP = 0,
    FAS_ACTION_SWEETSPOT = 1,
    FAS_ACTION_SWEETSPOT_CEILING = 2,
    FAS_ACTION_MAX_FREQ = 3
} FasActionLevel;

// ── 运行时快照结构 ──────────────────────────────────────────

typedef struct {
    int frame_count;
    int short_count;
    int long_count;
    int target_fps;
    int target_changed;
    double last_frame_ms;
    double avg_4_ms;
    double avg_8_ms;
    double avg_short_ms;
    double avg_long_ms;
    double frame_slope_ms;
    /* 帧信号新鲜度：frame_age_ms 是距上一帧过去的时间，frames_fresh=0 表示画面已停止出帧 */
    int frames_fresh;
    double frame_age_ms;
    int current_fps_short;
    int current_fps_long;
    FasBufferState working_state;
} FasFrameSnapshot;

typedef struct {
    float game_thread_usage;
    float render_thread_usage;
    float rhi_thread_usage;
    float taskgraph_thread_usage;
    float worker_top_n_usage;
    float busy_thread_usage;
    float runnable_pressure;
} FasThreadSnapshot;

typedef struct {
    int gpu_busy_valid;
    int gpu_freq_valid;
    int ddr_freq_valid;
    int gpu_busy_pct;
    int gpu_freq_khz;
    int ddr_freq_khz;
    int gpu_pressure;
} FasGpuSnapshot;

// ── 配置结构 ────────────────────────────────────────────────

typedef struct {
    char package[FAS_MAX_PACKAGE_LEN];
    char friendly[96];
    int target_fps;
    float margin_fps;
    float kp;            /* 帧债分档灵敏度：3.0 为基准，越大同样的帧债判得越重 */
    int fallback;
} FasAppConfig;

typedef struct {
    /* ⚠️ 旧字段（target_fps / margin / kp）保留只为 ABI 兼容，新控制器不再读它们 */
    int default_target_fps;
    float base_margin_fps;
    float default_kp;

    /* ── 新开关（fas.json 的 defaults）── */
    int quick_drop;            /* 帧率稳定后直接降到底板（1=开） */
    int report_thread_state;   /* 把重要线程占用报告给 FAS（1=开） */
    int upscale_occupancy;     /* 占用率升频阈值（0=关闭） */

    int app_count;
    FasAppConfig apps[FAS_MAX_FAS_APPS];
} FasConfig;

// ── Buffer 状态（运行时）─────────────────────────────────────

typedef struct {
    char package[FAS_MAX_PACKAGE_LEN];
    FasBufferState working_state;
    long long last_update_ms;
    long long working_state_since_ms;
    int stable_target_fps;
    int pending_target_fps;
    long long pending_target_since_ms;
} FasBufferStateInfo;

// ── 引擎输出 ────────────────────────────────────────────────

typedef struct {
    int guard_signal;          // FasGuardReason：动作档位 >= 2 时表达"这一帧需要保"
    int action_level;          // FasActionLevel，FAS_ACTION_INVALID 表示没有帧信号
    int current_fps;
    int target_fps;            // 已定档的目标帧率
    int effective_target_fps;  // 扣掉 margin 后的生效目标帧率（预算的来源）
    int valid_samples;
    int buffer_state;          // FasBufferState
    /*
     * 渲染时间观测：FAS 理念下的被控量来源
     *   frame_us   = 最近一帧的实测帧时间（回到"看画面"的原始口径）
     *   avg_frame_us = 短窗口平均帧时间，动作档位按它判
     *   budget_us  = 1e6 / effective_target_fps，这一帧的渲染时间预算
     *   debt_us    = avg_frame_us - budget_us，正数 = 这一帧已经晚了
     *   severity_us = debt 经 kp（灵敏度）缩放后的严重度，用于分档
     */
    int frame_us;
    int avg_frame_us;
    int budget_us;
    int debt_us;
    int severity_us;
    int over_budget_streak;    // 连续严重超预算的帧数（按帧事件计数，不是按 tick）

    /* ── 新控制器（v6.2）：三相位 + 频率倍数 ─────────────────── */
    int phase;                 /* FasPhase */
    int stable_fps;            /* 校准得到的基准帧率（0 = 还没校准好） */
    /*
     * 频率倍数：执行器拿「当前目标频率 × freq_scale」得到本拍目标频率。
     *   1.0  = 保持
     *   >1   = 升频（掉帧时按 实际帧间隔/基准帧间隔 算出来，不是阶梯）
     *   <1   = 降频（稳定时按固定比例慢降）
     */
    float freq_scale;
    int hold_sweetspot;        /* 1 = 校准期：频率固定在甜点 */
    int drop_to_floor;         /* 1 = 帧率稳定：直接降到底板（quick_drop） */
    int upscale_by_occupancy;  /* 1 = 因重要线程占用率触发的升频 */
    int busy_thread_usage;     /* 重要线程占用（百分比；-1 = 未上报） */
} FasOutput;

/*
 * 动作表参数：动态调频（dyn.json 的 control）在运行时调的旋钮。
 *
 * 阈值都是"帧债 / 这一帧的预算"的千分比：
 *   深睡  severity <= deadband             → 贴地板
 *   甜点  deadband < severity <= sweetspot → 抬到甜点频率
 *   放开  sweetspot < severity <= ceiling  → 甜点做底，上限放开
 *   顶格  severity > ceiling，或连续 severe_streak 帧严重超预算 → 顶 fmax
 * 默认值就是原来写死的常量，改配置才会变。
 */
typedef struct {
    int deadband_permille;      /* 死区：3% 预算（30） */
    int sweetspot_permille;     /* 抬甜点阈值：10% 预算（100） */
    int ceiling_permille;       /* 放开上限阈值：30% 预算（300） */
    int severe_streak;          /* 连续几帧严重超预算就顶格（2） */
    int release_step_permille;  /* 降频步长：占该簇范围的千分比（250） */
    int release_hold_ms;        /* 两次降频之间的最小间隔 ms（60） */
} FasLadderConfig;

#ifdef __cplusplus
}
#endif

#endif // FAS_TYPES_H
